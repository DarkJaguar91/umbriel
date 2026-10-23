#pragma once

#include <functional>
#include <list>
#include <wayland-server-core.h>

struct wlr_keyboard;

namespace umbriel {

  // Device-local XKB state can belong to a keyboard with no LEDs, for example
  // a uinput hotkey proxy. Publish the active source's indicators to every
  // backend keyboard after wlroots finishes its device-local LED updates.
  class KeyboardLeds {
  public:
    explicit KeyboardLeds(wl_event_loop* loop, std::function<bool(wlr_keyboard*)> canUseSource = {});
    ~KeyboardLeds();

    KeyboardLeds(const KeyboardLeds&) = delete;
    KeyboardLeds& operator=(const KeyboardLeds&) = delete;

    void add(wlr_keyboard* keyboard, bool virtualDevice = false);
    void remove(wlr_keyboard* keyboard);
    // A usable explicit source becomes authoritative. A null source only
    // schedules a refresh of the current source, including IME echo events.
    void update(wlr_keyboard* source = nullptr);

  private:
    struct Entry {
      KeyboardLeds* owner = nullptr;
      wlr_keyboard* keyboard = nullptr;
      bool virtualDevice = false;
      wl_listener keymap{};
      wl_listener destroy{};
    };

    static void onKeymap(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static void onIdle(void* data);

    [[nodiscard]] bool usableSource(wlr_keyboard* keyboard) const;
    [[nodiscard]] wlr_keyboard* fallbackSource() const;
    void flush();

    wl_event_loop* m_loop = nullptr;
    wl_event_source* m_idle = nullptr;
    std::function<bool(wlr_keyboard*)> m_canUseSource;
    std::list<Entry> m_entries;
    wlr_keyboard* m_source = nullptr;
  };

} // namespace umbriel
