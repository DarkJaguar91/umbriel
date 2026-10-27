#!/usr/bin/env bash
# Uses a private bus and a real compositor with backend-boundary input devices.
# No pointer, keyboard, configuration, or session service on the host is changed.
set -euo pipefail

readonly BUILD=$(realpath "${1:?usage: input_capture.sh UMBRIEL_BUILD PORTAL_BUILD [XDG_DESKTOP_PORTAL]}")
readonly PORTAL_BUILD=$(realpath "${2:?portal build directory required}")
readonly COMPOSITOR=${UMBRIEL_CAPTURE_COMPOSITOR:-$BUILD/tests/input-capture-compositor}
readonly PORTAL_SERVER=${UMBRIEL_CAPTURE_PORTAL:-$PORTAL_BUILD/xdg-desktop-portal-umbriel}
readonly FRONTEND=${3:-$(command -v xdg-desktop-portal || true)}
readonly REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly PORTAL_REPO=${UMBRIEL_PORTAL_REPO:-$(dirname "$REPO")/xdg-desktop-portal-umbriel}
if [[ ! -x $FRONTEND ]]; then
  echo "Pass the xdg-desktop-portal executable as the third argument."
  exit 2
fi
readonly RUNTIME=$(mktemp -d "${TMPDIR:-/tmp}/umbriel-input-capture.XXXXXXXX")
chmod 700 "$RUNTIME"
PIDS=()
SUCCESS=0
cleanup() {
  local pid
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  if ((SUCCESS)) && [[ ${UMBRIEL_CAPTURE_KEEP_LOGS:-0} != 1 ]]; then
    rm -rf "$RUNTIME"
  else
    echo "InputCapture logs retained in $RUNTIME"
  fi
}
trap cleanup EXIT

export XDG_RUNTIME_DIR=$RUNTIME
export XDG_CONFIG_HOME=$RUNTIME/config
export XDG_DATA_HOME=$RUNTIME/data-home
export XDG_CACHE_HOME=$RUNTIME/cache
export XDG_DATA_DIRS=$RUNTIME/data
export XDG_CONFIG_DIRS=$RUNTIME/config-dirs
export XDG_CURRENT_DESKTOP=umbriel
export XDG_SESSION_TYPE=wayland
export GSETTINGS_BACKEND=memory
export NO_AT_BRIDGE=1
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/bus
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/no-system-bus
unset DISPLAY WAYLAND_SOCKET NOTIFY_SOCKET SYSTEMD_EXEC_PID
mkdir -p "$XDG_CONFIG_HOME/xdg-desktop-portal" "$RUNTIME/portals" "$RUNTIME/data" "$XDG_DATA_HOME"
cp "$PORTAL_REPO/data/umbriel.portal" "$RUNTIME/portals/umbriel.portal"
export XDG_DESKTOP_PORTAL_DIR=$RUNTIME/portals
cat > "$XDG_CONFIG_HOME/xdg-desktop-portal/portals.conf" <<'CONFIG'
[preferred]
default=umbriel
org.freedesktop.impl.portal.InputCapture=umbriel
CONFIG
cat > "$RUNTIME/config.toml" <<'CONFIG'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
[animation]
enabled = false
[input.keyboard]
layout = "us"
numlock_toggle = false
CONFIG

dbus-daemon --session --nofork --address="$DBUS_SESSION_BUS_ADDRESS" > "$RUNTIME/bus.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 100); do [[ -S $RUNTIME/bus ]] && break; sleep 0.025; done

mkfifo "$RUNTIME/physical.fifo"
exec {PHYSICAL_FD}<> "$RUNTIME/physical.fifo"
WAYLAND_DISPLAY=input-capture-isolated-parent WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
  "$COMPOSITOR" "$RUNTIME/config.toml" <&"$PHYSICAL_FD" > "$RUNTIME/compositor.log" 2>&1 &
PIDS+=("$!")

source "$REPO/tests/harness/lib.sh"
await_events "$RUNTIME/compositor.log" 'ready display=' 1 "isolated physical-input compositor"
export WAYLAND_DISPLAY
WAYLAND_DISPLAY=$(sed -n 's/^ready display=\([^ ]*\).*/\1/p' "$RUNTIME/compositor.log" | head -1)
export UMBRIEL_SOCKET
UMBRIEL_SOCKET=$(sed -n 's/^ready .*socket=\([^ ]*\).*/\1/p' "$RUNTIME/compositor.log" | head -1)
export UMBRIEL=$BUILD/umbriel
readonly LOCAL_LOG=$RUNTIME/local.log
mkfifo "$RUNTIME/observer.fifo"
exec {OBSERVER_FD}<> "$RUNTIME/observer.fifo"
LOG_POINTER_INPUT=1 LOG_MODIFIERS=1 POINTER_CONSTRAINT_CONTROL=1 \
  "$BUILD/tests/seat-log-client" input-capture-observer <&"$OBSERVER_FD" > "$LOCAL_LOG" 2>&1 &
PIDS+=("$!")
await_events "$LOCAL_LOG" keyboard-enter 1 "local input observer"

physical() { printf '%s\n' "$@" >&"$PHYSICAL_FD"; }
wait_confirmation() {
  local previous
  previous=$(events "$RUNTIME/compositor.log" 'status confirmation=1')
  for _ in $(seq 100); do
    physical status
    (($(events "$RUNTIME/compositor.log" 'status confirmation=1') > previous)) && return 0
    sleep 0.025
  done
  echo "capture confirmation did not become visible"
  return 1
}
assert_count() {
  local actual
  actual=$(events "$1" "$2")
  if ((actual != $3)); then
    echo "expected $3 '$2' events, got $actual: $(cat "$1")"
    return 1
  fi
}
sync_local() {
  local previous
  previous=$(events "$LOCAL_LOG" pointer-position)
  printf p >&"$OBSERVER_FD"
  await_events "$LOCAL_LOG" pointer-position "$((previous + 1))" "local observer roundtrip"
}

# Direct protocol requests also need physical approval. A virtual keyboard
# cannot confirm the compositor's permission panel.
mkfifo "$RUNTIME/private.fifo"
exec {PRIVATE_FD}<> "$RUNTIME/private.fifo"
"$BUILD/tests/input-capture-client" <&"$PRIVATE_FD" > "$RUNTIME/private.log" 2>&1 &
private_pid=$!
PIDS+=("$private_pid")
await_events "$RUNTIME/private.log" ready 1 "private capture client"
printf 'start\n' >&"$PRIVATE_FD"
wait_confirmation
"$BUILD/tests/pointer-client" 1280 720 keyboard-only tap 28
status_before=$(events "$RUNTIME/compositor.log" status)
physical status
await_events "$RUNTIME/compositor.log" status "$((status_before + 1))" "status after virtual Enter"
if ! grep '^status ' "$RUNTIME/compositor.log" | tail -1 | grep -q 'confirmation=1 active=0'; then
  echo "virtual Enter changed capture permission"
  exit 1
fi
assert_count "$RUNTIME/private.log" 'started .*status=0' 0
physical 'tap 1'
await_events "$RUNTIME/private.log" 'started .*status=1' 1 "physical rejection"
printf 'quit\n' >&"$PRIVATE_FD"
wait "$private_pid"

"$PORTAL_SERVER" > "$RUNTIME/portal.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 100); do
  if dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
      org.freedesktop.DBus.NameHasOwner string:org.freedesktop.impl.portal.desktop.umbriel 2>/dev/null | grep -q 'boolean true'; then break; fi
  sleep 0.025
done
"$FRONTEND" > "$RUNTIME/frontend.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 100); do
  if dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
      org.freedesktop.DBus.NameHasOwner string:org.freedesktop.portal.Desktop 2>/dev/null | grep -q 'boolean true'; then break; fi
  sleep 0.025
done

mkfifo "$RUNTIME/client.fifo"
exec {CLIENT_FD}<> "$RUNTIME/client.fifo"
readonly CAPTURE_LOG=$RUNTIME/capture.log
"$PORTAL_BUILD/input-capture-test-client" <&"$CLIENT_FD" > "$CAPTURE_LOG" 2>&1 &
client_pid=$!
PIDS+=("$client_pid")
send() { printf '%s\n' "$@" >&"$CLIENT_FD"; }
await_events "$CAPTURE_LOG" 'properties version=2 capabilities=3' 1 "real portal frontend properties"
await_events "$CAPTURE_LOG" ready 1 "v2 session creation"
send start
wait_confirmation
send attack-cancel
await_events "$CAPTURE_LOG" 'untrusted-cancel rejected' 1 "backend request cancellation rejects unrelated bus clients"
physical 'tap 28'
await_events "$CAPTURE_LOG" 'started capabilities=3 restore=0 clipboard=0' 1 "physical capture consent without persistence"
send zones
await_events "$CAPTURE_LOG" 'zone x=' 1 "logical output zone"
read -r zone_x zone_y zone_w zone_h < <(sed -n 's/^zone x=\(-*[0-9]*\) y=\(-*[0-9]*\) width=\([0-9]*\) height=\([0-9]*\)$/\1 \2 \3 \4/p' "$CAPTURE_LOG" | head -1)
readonly RIGHT=$((zone_x + zone_w))
readonly BOTTOM=$((zone_y + zone_h - 1))
send 'barrier 999 10 10 20 20'
await_events "$CAPTURE_LOG" 'failed-barrier id=999' 1 "diagonal barrier rejection"
send "barrier 1 $RIGHT $zone_y $RIGHT $BOTTOM" connect
await_events "$CAPTURE_LOG" 'barriers failed=0' 1 "exterior barrier acceptance"
await_events "$CAPTURE_LOG" ei-device-ready 2 "keyboard and pointer EI devices"

physical 'at 400 300' 'tap 32'
await_events "$LOCAL_LOG" 'keyboard-key code=32 state=pressed' 1 "local input before activation"
physical 'press 42'
await_events "$LOCAL_LOG" 'keyboard-key code=42 state=pressed' 1 "held Shift before capture"
send enable
await_events "$CAPTURE_LOG" 'requested enable' 1 "capture enable"
physical "at $((RIGHT - 1)) $((zone_y + zone_h / 2))" 'move 20 0'
await_events "$CAPTURE_LOG" activated 1 "barrier activation through frontend"
activation=$(sed -n 's/^activated id=\([0-9]*\).*/\1/p' "$CAPTURE_LOG" | tail -1)
await_events "$CAPTURE_LOG" "ei-start id=$activation" 2 "matching EI activation sequences"
await_events "$CAPTURE_LOG" 'ei-key code=42 state=pressed' 1 "held Shift handoff"
sync_local
status_before=$(events "$RUNTIME/compositor.log" status)
physical status
await_events "$RUNTIME/compositor.log" status "$((status_before + 1))" "cursor position at capture start"
capture_position=$(grep '^status ' "$RUNTIME/compositor.log" | tail -1 | sed 's/^.* x=/x=/')
local_motion=$(events "$LOCAL_LOG" pointer-motion)
local_relative=$(events "$LOCAL_LOG" relative-motion)
local_axis=$(events "$LOCAL_LOG" pointer-axis)
"$BUILD/tests/pointer-client" 1280 720 keyboard-only tap 34
physical 'tap 30' 'button 272 1' 'scroll 15 120' 'move 7 3'
await_events "$CAPTURE_LOG" 'ei-key code=30 state=released' 1 "captured key transition"
await_events "$CAPTURE_LOG" 'ei-button code=272 state=pressed' 1 "captured button"
await_events "$CAPTURE_LOG" 'ei-discrete x=0 y=120' 1 "captured wheel"
await_events "$CAPTURE_LOG" 'ei-motion dx=7.000 dy=3.000' 1 "captured motion"
physical 'absolute 0.25 0.5' 'absolute 0.5 0.5' 'absolute 0.75 0.5'
absolute_delta=$(printf 'ei-motion dx=%.3f dy=0.000' "$(awk -v width="$zone_w" 'BEGIN {print width / 4}')")
await_events "$CAPTURE_LOG" "$absolute_delta" 2 "successive absolute samples use the previous device position"
sync_local
status_before=$(events "$RUNTIME/compositor.log" status)
physical status
await_events "$RUNTIME/compositor.log" status "$((status_before + 1))" "cursor remains frozen during capture"
if [[ $(grep '^status ' "$RUNTIME/compositor.log" | tail -1 | sed 's/^.* x=/x=/') != "$capture_position" ]]; then
  echo "capture moved the local compositor cursor"
  exit 1
fi
assert_count "$LOCAL_LOG" 'keyboard-key code=30 state=pressed' 0
assert_count "$LOCAL_LOG" 'keyboard-key code=34 state=pressed' 0
assert_count "$CAPTURE_LOG" 'ei-key code=34 state=pressed' 0
assert_count "$LOCAL_LOG" 'pointer-button code=272 state=pressed' 0
assert_count "$LOCAL_LOG" pointer-motion "$local_motion"
assert_count "$LOCAL_LOG" relative-motion "$local_relative"
assert_count "$LOCAL_LOG" pointer-axis "$local_axis"

send 'release 0 400 300'
await_events "$CAPTURE_LOG" 'requested release id=0' 1 "stale release"
physical 'tap 31'
await_events "$CAPTURE_LOG" 'ei-key code=31 state=pressed' 1 "capture survives a stale release"
send "release $activation 400 300"
await_events "$CAPTURE_LOG" ei-stop 2 "explicit release stops both devices"
await_events "$CAPTURE_LOG" 'ei-key code=42 state=released' 1 "remote held Shift cleanup"
await_events "$CAPTURE_LOG" 'ei-button code=272 state=released' 1 "remote held button cleanup"
physical 'release 42' 'button 272 0' 'tap 30'
await_events "$LOCAL_LOG" 'keyboard-key code=30 state=pressed' 1 "local input restored on release"
assert_count "$CAPTURE_LOG" deactivated 0

# Release leaves the session enabled and the same EI connection reusable.
physical "at $((RIGHT - 1)) $((zone_y + zone_h / 2))" 'move 20 0'
await_events "$CAPTURE_LOG" activated 2 "reactivation on the same EI connection"
physical 'press 29' 'press 56' 'tap 1' 'release 56' 'release 29'
await_events "$CAPTURE_LOG" disabled 1 "physical emergency release"
await_events "$CAPTURE_LOG" deactivated 1 "emergency deactivation"
physical 'tap 33'
await_events "$LOCAL_LOG" 'keyboard-key code=33 state=pressed' 1 "local input after emergency release"
assert_count "$CAPTURE_LOG" 'ei-key code=1 state=pressed' 0
status_before=$(events "$RUNTIME/compositor.log" status)
physical status
await_events "$RUNTIME/compositor.log" status "$((status_before + 1))" "emergency release cursor position"
if ! grep '^status ' "$RUNTIME/compositor.log" | tail -1 |
    awk -v right="$RIGHT" '{for(i=1;i<=NF;i++)if($i~/^x=/){split($i,a,"=");exit !(a[2]>=right-1&&a[2]<=right)}}'; then
  echo "emergency release did not leave the cursor at the barrier"
  exit 1
fi

# Locking revokes active capture before any password input is delivered.
send enable
await_events "$CAPTURE_LOG" 'requested enable' 2 "reenabling capture before lock"
physical "at $((RIGHT - 1)) $((zone_y + zone_h / 2))" 'move 20 0'
await_events "$CAPTURE_LOG" activated 3 "capture before session lock"
mkfifo "$RUNTIME/lock.fifo"
exec {LOCK_FD}<> "$RUNTIME/lock.fifo"
LOG_LOCK_INPUT=1 "$BUILD/tests/lock-client" <&"$LOCK_FD" > "$RUNTIME/lock.log" 2>&1 &
lock_pid=$!
PIDS+=("$lock_pid")
await_events "$RUNTIME/lock.log" locked 1 "real session lock"
await_events "$CAPTURE_LOG" disabled 2 "capture revoked by locking"
physical 'tap 35'
await_events "$RUNTIME/lock.log" 'keyboard-key code=35 state=pressed' 1 "locker receives physical input"
assert_count "$CAPTURE_LOG" 'ei-key code=35 state=pressed' 0
printf 'unlock\n' >&"$LOCK_FD"
wait "$lock_pid"
await_events "$RUNTIME/lock.log" unlocked 1 "unlocking the isolated session"

"$UMBRIEL" output-create capture-hotplug 640x360 > /dev/null
await_events "$CAPTURE_LOG" zones-changed 1 "output change invalidates capture zones"
send close
await_events "$CAPTURE_LOG" ei-disconnected 1 "session close revokes EI transport"
send quit
wait "$client_pid"

# A new session from the same application must ask again, despite the requested
# persistent permission mode in the first Start call.
"$PORTAL_BUILD/input-capture-test-client" <&"$CLIENT_FD" > "$RUNTIME/second.log" 2>&1 &
second_pid=$!
PIDS+=("$second_pid")
await_events "$RUNTIME/second.log" ready 1 "second session creation"
send start
wait_confirmation
assert_count "$RUNTIME/second.log" started 0
physical 'tap 1'
await_events "$RUNTIME/second.log" 'response Start status=1' 1 "fresh session requires approval again"
send quit
wait "$second_pid"

# Losing the application connection while it owns held input must restore
# local control through the frontend's session cleanup.
readonly CRASH_LOG=$RUNTIME/crash.log
"$PORTAL_BUILD/input-capture-test-client" <&"$CLIENT_FD" > "$CRASH_LOG" 2>&1 &
crash_pid=$!
PIDS+=("$crash_pid")
await_events "$CRASH_LOG" ready 1 "session for disconnect test"
send start
wait_confirmation
physical 'tap 28'
await_events "$CRASH_LOG" 'started capabilities=3' 1 "disconnect test consent"
send zones
await_events "$CRASH_LOG" 'zone x=' 1 "updated zones after hotplug"
send "barrier 5 $zone_x $zone_y $zone_x $BOTTOM" connect
await_events "$CRASH_LOG" 'barriers failed=0' 1 "left exterior barrier"
await_events "$CRASH_LOG" ei-device-ready 2 "disconnect test EI devices"
send enable
await_events "$CRASH_LOG" 'requested enable' 1 "disconnect test enable"
physical "at $((zone_x + 1)) $((zone_y + zone_h / 2))" 'move -20 0'
await_events "$CRASH_LOG" activated 1 "capture before application disconnect"
physical 'press 30' 'button 272 1'
await_events "$CRASH_LOG" 'ei-key code=30 state=pressed' 1 "held key before disconnect"
kill "$crash_pid"
wait "$crash_pid" 2>/dev/null || true
for _ in $(seq 100); do
  status_before=$(events "$RUNTIME/compositor.log" status)
  physical status
  await_events "$RUNTIME/compositor.log" status "$((status_before + 1))" "capture state after disconnect"
  if grep '^status ' "$RUNTIME/compositor.log" | tail -1 | grep -q 'active=0'; then break; fi
  sleep 0.025
done
physical 'release 30' 'button 272 0' 'at 400 300' 'tap 36'
await_events "$LOCAL_LOG" 'keyboard-key code=36 state=pressed' 1 "application disconnect restores local input"

# The negotiated mask selects which input kinds leave the desktop. The other
# kind remains available to the local client during the same capture session.
for capabilities in 1 2; do
  CAPABILITY_LOG=$RUNTIME/capability-$capabilities.log
  "$PORTAL_BUILD/input-capture-test-client" <&"$CLIENT_FD" > "$CAPABILITY_LOG" 2>&1 &
  capability_pid=$!
  PIDS+=("$capability_pid")
  await_events "$CAPABILITY_LOG" ready 1 "capability subset session"
  send "start $capabilities"
  wait_confirmation
  physical 'tap 28'
  await_events "$CAPABILITY_LOG" "started capabilities=$capabilities" 1 "capability subset consent"
  send zones
  await_events "$CAPABILITY_LOG" 'zone x=' 1 "capability subset zones"
  send "barrier 6 $zone_x $zone_y $zone_x $BOTTOM" connect
  await_events "$CAPABILITY_LOG" 'barriers failed=0' 1 "capability subset barrier"
  await_events "$CAPABILITY_LOG" ei-device-ready 1 "negotiated EI device"
  physical 'at 400 300'
  sync_local
  local_keys=$(events "$LOCAL_LOG" 'keyboard-key code=40 state=pressed')
  local_motion=$(events "$LOCAL_LOG" pointer-motion)
  send enable
  await_events "$CAPABILITY_LOG" 'requested enable' 1 "capability subset enable"
  physical "at $((zone_x + 1)) $((zone_y + zone_h / 2))" 'move -20 0'
  await_events "$CAPABILITY_LOG" activated 1 "capability subset activation"
  sync_local
  local_motion=$(events "$LOCAL_LOG" pointer-motion)
  if ((capabilities == 1)); then
    physical 'at 400 300' 'move 3 1' 'tap 40'
    await_events "$CAPABILITY_LOG" 'ei-key code=40 state=pressed' 1 "keyboard-only capture exports keys"
    await_events "$LOCAL_LOG" pointer-motion "$((local_motion + 1))" "keyboard-only capture leaves pointer local"
    assert_count "$LOCAL_LOG" 'keyboard-key code=40 state=pressed' "$local_keys"
    assert_count "$CAPABILITY_LOG" ei-motion 0
  else
    physical 'move 7 3' 'tap 40'
    await_events "$CAPABILITY_LOG" 'ei-motion dx=7.000 dy=3.000' 1 "pointer-only capture exports motion"
    await_events "$LOCAL_LOG" 'keyboard-key code=40 state=pressed' "$((local_keys + 1))" "pointer-only capture leaves keyboard local"
    sync_local
    assert_count "$LOCAL_LOG" pointer-motion "$local_motion"
    assert_count "$CAPABILITY_LOG" ei-key 0
  fi
  send close
  await_events "$CAPABILITY_LOG" ei-disconnected 1 "capability subset close"
  send quit
  wait "$capability_pid"
done

# Touch is neither advertised nor silently substituted with keyboard/pointer.
readonly TOUCH_LOG=$RUNTIME/touch.log
"$PORTAL_BUILD/input-capture-test-client" <&"$CLIENT_FD" > "$TOUCH_LOG" 2>&1 &
touch_pid=$!
PIDS+=("$touch_pid")
await_events "$TOUCH_LOG" ready 1 "unsupported capability session"
send 'start 4'
await_events "$TOUCH_LOG" '\(response Start status=[12]\|input-capture-test-client:\)' 1 "touch-only request fails"
status_before=$(events "$RUNTIME/compositor.log" status)
physical status
await_events "$RUNTIME/compositor.log" status "$((status_before + 1))" "unsupported request does not prompt"
if ! grep '^status ' "$RUNTIME/compositor.log" | tail -1 | grep -q 'confirmation=0 active=0'; then
  echo "unsupported touch capture opened a confirmation or started capture"
  exit 1
fi
send quit
wait "$touch_pid" 2>/dev/null || true
physical quit
echo "InputCapture v2: frontend consent, EI handoff, local suppression, release, escape, lock and disconnect passed"
SUCCESS=1
