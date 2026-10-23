#include "input/keyboard_leds.h"

#include "check.h"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <wlr/interfaces/wlr_keyboard.h>
}

namespace {

  class EventLoop {
  public:
    EventLoop() : m_loop(wl_event_loop_create()) {
      if (m_loop == nullptr) {
        throw std::runtime_error("could not create the keyboard LED test event loop");
      }
    }
    ~EventLoop() { wl_event_loop_destroy(m_loop); }

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    [[nodiscard]] wl_event_loop* get() const { return m_loop; }
    void flush() const { wl_event_loop_dispatch_idle(m_loop); }

  private:
    wl_event_loop* m_loop = nullptr;
  };

  // Only the hardware boundary is supplied here. Key processing, XKB state,
  // modifier/keymap signals, LED deduplication, and idle dispatch are real.
  class BackendKeyboard {
  public:
    explicit BackendKeyboard(bool hasLeds = true, const char* layouts = "us") {
      wlr_keyboard_init(&m_keyboard, hasLeds ? &kWithLeds : &kWithoutLeds, "keyboard LED test");
      m_keyboard.data = this;
      setKeymap(layouts);
      m_key.notify = onKey;
      wl_signal_add(&m_keyboard.events.key, &m_key);
      m_modifiers.notify = onModifiers;
      wl_signal_add(&m_keyboard.events.modifiers, &m_modifiers);
    }

    ~BackendKeyboard() { finish(); }

    BackendKeyboard(const BackendKeyboard&) = delete;
    BackendKeyboard& operator=(const BackendKeyboard&) = delete;

    [[nodiscard]] wlr_keyboard* get() { return &m_keyboard; }
    [[nodiscard]] const std::vector<uint32_t>& writes() const { return m_writes; }
    void clearWrites() { m_writes.clear(); }
    void stopObserving() { m_leds = nullptr; }

    void observe(umbriel::KeyboardLeds& leds, bool changesSource = true, bool virtualDevice = false) {
      m_leds = &leds;
      m_changesSource = changesSource;
      leds.add(&m_keyboard, virtualDevice);
    }

    void setKeymap(const char* layouts) {
      xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      const xkb_rule_names names{
          .rules = nullptr, .model = nullptr, .layout = layouts, .variant = nullptr, .options = nullptr
      };
      xkb_keymap* keymap =
          context != nullptr ? xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
      if (keymap == nullptr) {
        xkb_context_unref(context);
        throw std::runtime_error("could not create the keyboard LED test keymap");
      }
      const bool applied = wlr_keyboard_set_keymap(&m_keyboard, keymap);
      xkb_keymap_unref(keymap);
      xkb_context_unref(context);
      if (!applied) {
        throw std::runtime_error("could not apply the keyboard LED test keymap");
      }
    }

    void renameCapsIndicator() {
      char* serialized = xkb_keymap_get_as_string(m_keyboard.keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
      if (serialized == nullptr) {
        throw std::runtime_error("could not serialize the keyboard LED test keymap");
      }
      std::string text(serialized);
      std::free(serialized);
      constexpr std::string_view original = "\"Caps Lock\"";
      constexpr std::string_view replacement = "\"Private Caps Indicator\"";
      size_t position = 0;
      while ((position = text.find(original, position)) != std::string::npos) {
        text.replace(position, original.size(), replacement);
        position += replacement.size();
      }
      xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      xkb_keymap* keymap = context != nullptr
          ? xkb_keymap_new_from_string(context, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS)
          : nullptr;
      if (keymap == nullptr) {
        xkb_context_unref(context);
        throw std::runtime_error("could not compile the keyboard LED test keymap with a renamed indicator");
      }
      const bool applied = wlr_keyboard_set_keymap(&m_keyboard, keymap);
      xkb_keymap_unref(keymap);
      xkb_context_unref(context);
      if (!applied) {
        throw std::runtime_error("could not apply the keyboard LED test keymap with a renamed indicator");
      }
    }

    void key(uint32_t code, wl_keyboard_key_state state) {
      wlr_keyboard_key_event event{.time_msec = ++m_time, .keycode = code, .update_state = true, .state = state};
      wlr_keyboard_notify_key(&m_keyboard, &event);
    }

    void tap(uint32_t code) {
      key(code, WL_KEYBOARD_KEY_STATE_PRESSED);
      key(code, WL_KEYBOARD_KEY_STATE_RELEASED);
    }

    void modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
      wlr_keyboard_notify_modifiers(&m_keyboard, depressed, latched, locked, group);
    }

    [[nodiscard]] uint32_t modifierMask(const char* name) const {
      const xkb_mod_index_t index = xkb_keymap_mod_get_index(m_keyboard.keymap, name);
      if (index == XKB_MOD_INVALID || index >= 32) {
        throw std::runtime_error("keyboard LED test keymap lacks the requested modifier");
      }
      return uint32_t{1} << index;
    }

    void finish() {
      if (m_finished) {
        return;
      }
      wl_list_remove(&m_key.link);
      wl_list_remove(&m_modifiers.link);
      wlr_keyboard_finish(&m_keyboard);
      m_finished = true;
    }

  private:
    static void recordLeds(wlr_keyboard* keyboard, uint32_t leds) {
      static_cast<BackendKeyboard*>(keyboard->data)->m_writes.push_back(leds);
    }

    static void onKey(wl_listener* listener, void* /*data*/) {
      BackendKeyboard* self;
      self = wl_container_of(listener, self, m_key);
      self->update();
    }

    static void onModifiers(wl_listener* listener, void* /*data*/) {
      BackendKeyboard* self;
      self = wl_container_of(listener, self, m_modifiers);
      self->update();
    }

    void update() {
      if (m_leds != nullptr) {
        m_leds->update(m_changesSource ? &m_keyboard : nullptr);
      }
    }

    static constexpr wlr_keyboard_impl kWithLeds{.name = "recording LED backend", .led_update = recordLeds};
    static constexpr wlr_keyboard_impl kWithoutLeds{.name = "LED-less uinput backend", .led_update = nullptr};

    wlr_keyboard m_keyboard{};
    std::vector<uint32_t> m_writes;
    umbriel::KeyboardLeds* m_leds = nullptr;
    wl_listener m_key{};
    wl_listener m_modifiers{};
    uint32_t m_time = 0;
    bool m_changesSource = true;
    bool m_finished = false;
  };

} // namespace

UMBRIEL_TEST(ledlessSourcePublishesTheFirstCapsAndNumTransitions) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard receiver;
  BackendKeyboard proxy(false);
  laptop.observe(leds);
  receiver.observe(leds);
  proxy.observe(leds);
  loop.flush();

  proxy.tap(58);
  CHECK(laptop.writes().empty());
  loop.flush();
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK});
  CHECK_EQ(receiver.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK});
  CHECK_EQ(xkb_state_led_name_is_active(proxy.get()->xkb_state, XKB_LED_NAME_CAPS), 1);
  CHECK_EQ(xkb_state_led_name_is_active(laptop.get()->xkb_state, XKB_LED_NAME_CAPS), 0);

  proxy.tap(58);
  loop.flush();
  const std::vector<uint32_t> capsTransitions{WLR_LED_CAPS_LOCK, 0};
  CHECK_EQ(laptop.writes(), capsTransitions);
  CHECK_EQ(receiver.writes(), capsTransitions);

  proxy.tap(69);
  loop.flush();
  const std::vector<uint32_t> allTransitions{WLR_LED_CAPS_LOCK, 0, WLR_LED_NUM_LOCK};
  CHECK_EQ(laptop.writes(), allTransitions);
  CHECK_EQ(receiver.writes(), allTransitions);
}

UMBRIEL_TEST(postEventRefreshRestoresAnIndicatorOverwrittenByLocalKeyProcessing) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  // Infrastructure events can refresh LEDs without selecting a new source.
  laptop.observe(leds, false);
  proxy.observe(leds);
  loop.flush();
  proxy.tap(58);
  loop.flush();
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK});

  laptop.key(30, WL_KEYBOARD_KEY_STATE_PRESSED);
  const std::vector<uint32_t> overwritten{WLR_LED_CAPS_LOCK, 0};
  CHECK_EQ(laptop.writes(), overwritten);
  loop.flush();
  const std::vector<uint32_t> restored{WLR_LED_CAPS_LOCK, 0, WLR_LED_CAPS_LOCK};
  CHECK_EQ(laptop.writes(), restored);
  CHECK_EQ(laptop.get()->modifiers.locked, uint32_t{0});
  laptop.key(30, WL_KEYBOARD_KEY_STATE_RELEASED);
  loop.flush();
}

UMBRIEL_TEST(realInputSelectsItsOwnIndicatorsWithoutCopyingOtherKeyboardState) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop(true, "de,us");
  BackendKeyboard proxy(false, "us,de");
  const uint32_t shift = laptop.modifierMask(XKB_MOD_NAME_SHIFT);
  const uint32_t num = laptop.modifierMask(XKB_MOD_NAME_NUM);
  laptop.modifiers(shift, shift, num, 1);
  laptop.observe(leds);
  proxy.observe(leds);
  loop.flush();
  laptop.clearWrites();
  xkb_keymap* originalMap = laptop.get()->keymap;

  proxy.tap(58);
  loop.flush();
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK});
  CHECK_EQ(laptop.get()->keymap, originalMap);
  CHECK_EQ(laptop.get()->modifiers.depressed, shift);
  CHECK_EQ(laptop.get()->modifiers.latched, shift);
  CHECK_EQ(laptop.get()->modifiers.locked, num);
  CHECK_EQ(laptop.get()->modifiers.group, uint32_t{1});

  // A real event on this keyboard becomes authoritative using its own state.
  laptop.key(30, WL_KEYBOARD_KEY_STATE_PRESSED);
  loop.flush();
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_NUM_LOCK});
  CHECK_EQ(proxy.get()->leds, uint32_t{WLR_LED_NUM_LOCK});
  CHECK_EQ(proxy.get()->modifiers.locked, proxy.modifierMask(XKB_MOD_NAME_CAPS));
  laptop.key(30, WL_KEYBOARD_KEY_STATE_RELEASED);
  loop.flush();
}

UMBRIEL_TEST(hotplugAndKeymapChangesRetainTheCurrentSource) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  laptop.observe(leds, false);
  proxy.observe(leds);
  loop.flush();
  proxy.tap(58);
  loop.flush();

  BackendKeyboard hotplug;
  hotplug.observe(leds, false);
  loop.flush();
  CHECK_EQ(hotplug.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK});

  laptop.setKeymap("de");
  CHECK_EQ(xkb_state_led_name_is_active(laptop.get()->xkb_state, XKB_LED_NAME_CAPS), 0);
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});
  loop.flush();
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});
  CHECK_EQ(hotplug.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});

  // A source's keymap replacement also refreshes indicators without a key.
  proxy.setKeymap("fr");
  CHECK_EQ(proxy.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});
  loop.flush();
  CHECK_EQ(laptop.get()->leds, uint32_t{0});
  CHECK_EQ(hotplug.get()->leds, uint32_t{0});
}

UMBRIEL_TEST(sourceRemovalBeforeIdleSelectsASurvivingPhysicalKeyboard) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  laptop.observe(leds);
  proxy.observe(leds);
  loop.flush();
  proxy.tap(58);
  proxy.finish();
  loop.flush();
  CHECK(laptop.writes().empty());

  BackendKeyboard replacement(false);
  replacement.observe(leds);
  replacement.tap(69);
  loop.flush();
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{WLR_LED_NUM_LOCK});
  replacement.finish();
  loop.flush();
  const std::vector<uint32_t> fallbackTransitions{WLR_LED_NUM_LOCK, 0};
  CHECK_EQ(laptop.writes(), fallbackTransitions);
}

UMBRIEL_TEST(changedIndicatorNamesDoNotTurnInvalidIndexesIntoActiveLeds) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  laptop.observe(leds);
  proxy.observe(leds);
  loop.flush();
  proxy.tap(58);
  loop.flush();
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK});

  proxy.renameCapsIndicator();
  proxy.tap(58);
  loop.flush();
  CHECK_EQ(xkb_keymap_led_get_index(proxy.get()->keymap, XKB_LED_NAME_CAPS), XKB_LED_INVALID);
  CHECK_EQ(xkb_state_mod_name_is_active(proxy.get()->xkb_state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED), 1);
  const std::vector<uint32_t> transitions{WLR_LED_CAPS_LOCK, 0};
  CHECK_EQ(laptop.writes(), transitions);
}

UMBRIEL_TEST(losingTheFinalUsableSourceClearsSurvivingIndicators) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  laptop.observe(leds);
  proxy.observe(leds);
  loop.flush();
  proxy.tap(58);
  loop.flush();
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK});

  CHECK(wlr_keyboard_set_keymap(laptop.get(), nullptr));
  proxy.finish();
  loop.flush();
  const std::vector<uint32_t> transitions{WLR_LED_CAPS_LOCK, 0};
  CHECK_EQ(laptop.writes(), transitions);
}

UMBRIEL_TEST(virtualTargetsAndInputMethodEchoesDoNotReplaceTheRealSource) {
  EventLoop loop;
  wlr_keyboard* excluded = nullptr;
  umbriel::KeyboardLeds leds(loop.get(), [&excluded](wlr_keyboard* source) { return source != excluded; });
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  BackendKeyboard inputMethod;
  laptop.observe(leds);
  proxy.observe(leds);
  inputMethod.observe(leds, false, true);
  loop.flush();
  proxy.tap(58);
  loop.flush();
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK});
  CHECK(inputMethod.writes().empty());

  excluded = inputMethod.get();
  leds.update(inputMethod.get());
  loop.flush();
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});
  CHECK(inputMethod.writes().empty());

  // Ownership may be recognized after an idle update was queued.
  excluded = nullptr;
  leds.update(inputMethod.get());
  excluded = inputMethod.get();
  loop.flush();
  CHECK_EQ(laptop.get()->leds, uint32_t{0});
  const std::vector<uint32_t> fallbackTransitions{WLR_LED_CAPS_LOCK, 0};
  CHECK_EQ(laptop.writes(), fallbackTransitions);
  CHECK(inputMethod.writes().empty());
}

UMBRIEL_TEST(destroyingTheRelayCancelsPendingCallbacksAndRemovesListeners) {
  EventLoop loop;
  BackendKeyboard keyboard;
  {
    umbriel::KeyboardLeds leds(loop.get());
    keyboard.observe(leds);
  }
  keyboard.stopObserving();
  // These emit registered signals after the relay has gone away. No stale
  // keymap/destroy listener or pending idle may access the destroyed object.
  keyboard.setKeymap("de");
  keyboard.finish();
  loop.flush();
  CHECK(keyboard.writes().empty());
}

int main() { return RUN_TESTS(); }
