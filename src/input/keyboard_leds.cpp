#include "input/keyboard_leds.h"

#include <algorithm>
#include <cstdint>
#include <utility>

extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

namespace umbriel {

  KeyboardLeds::KeyboardLeds(wl_event_loop* loop, std::function<bool(wlr_keyboard*)> canUseSource)
      : m_loop(loop), m_canUseSource(std::move(canUseSource)) {}

  KeyboardLeds::~KeyboardLeds() {
    if (m_idle != nullptr) {
      wl_event_source_remove(m_idle);
    }
    for (Entry& entry : m_entries) {
      wl_list_remove(&entry.keymap.link);
      wl_list_remove(&entry.destroy.link);
    }
  }

  bool KeyboardLeds::usableSource(wlr_keyboard* keyboard) const {
    return keyboard != nullptr
        && std::ranges::any_of(m_entries, [keyboard](const Entry& entry) { return entry.keyboard == keyboard; })
        && keyboard->xkb_state != nullptr
        && (!m_canUseSource || m_canUseSource(keyboard));
  }

  wlr_keyboard* KeyboardLeds::fallbackSource() const {
    for (const Entry& entry : m_entries) {
      if (!entry.virtualDevice && usableSource(entry.keyboard)) {
        return entry.keyboard;
      }
    }
    return nullptr;
  }

  void KeyboardLeds::add(wlr_keyboard* keyboard, bool virtualDevice) {
    if (keyboard == nullptr
        || std::ranges::any_of(m_entries, [keyboard](const Entry& entry) { return entry.keyboard == keyboard; })) {
      return;
    }
    Entry& entry = m_entries.emplace_back();
    entry.owner = this;
    entry.keyboard = keyboard;
    entry.virtualDevice = virtualDevice;
    entry.keymap.notify = onKeymap;
    wl_signal_add(&keyboard->events.keymap, &entry.keymap);
    entry.destroy.notify = onDestroy;
    wl_signal_add(&keyboard->base.events.destroy, &entry.destroy);
    if (m_source == nullptr && !virtualDevice && usableSource(keyboard)) {
      m_source = keyboard;
    }
    update();
  }

  void KeyboardLeds::remove(wlr_keyboard* keyboard) {
    const auto entry =
        std::ranges::find_if(m_entries, [keyboard](const Entry& candidate) { return candidate.keyboard == keyboard; });
    if (entry == m_entries.end()) {
      return;
    }
    wl_list_remove(&entry->keymap.link);
    wl_list_remove(&entry->destroy.link);
    m_entries.erase(entry);
    if (m_source == keyboard) {
      m_source = fallbackSource();
    }
    update();
  }

  void KeyboardLeds::update(wlr_keyboard* source) {
    if (usableSource(source)) {
      m_source = source;
    }
    if (m_idle == nullptr) {
      m_idle = wl_event_loop_add_idle(m_loop, onIdle, this);
    }
  }

  void KeyboardLeds::onKeymap(wl_listener* listener, void* /*data*/) {
    Entry* entry;
    entry = wl_container_of(listener, entry, keymap);
    entry->owner->update();
  }

  void KeyboardLeds::onDestroy(wl_listener* listener, void* /*data*/) {
    Entry* entry;
    entry = wl_container_of(listener, entry, destroy);
    entry->owner->remove(entry->keyboard);
  }

  void KeyboardLeds::onIdle(void* data) {
    auto* self = static_cast<KeyboardLeds*>(data);
    self->m_idle = nullptr;
    self->flush();
  }

  void KeyboardLeds::flush() {
    if (!usableSource(m_source)) {
      m_source = fallbackSource();
    }
    // The modifiers signal precedes wlroots' own LED refresh. Reading XKB
    // here also handles keymap replacement and deferred input-method ownership.
    uint32_t leds = 0;
    if (m_source != nullptr) {
      for (uint32_t i = 0; i < WLR_LED_COUNT; ++i) {
        const xkb_led_index_t index = m_source->led_indexes[i];
        if (index != XKB_LED_INVALID && xkb_state_led_index_is_active(m_source->xkb_state, index) > 0) {
          leds |= uint32_t{1} << i;
        }
      }
    }
    for (const Entry& entry : m_entries) {
      if (!entry.virtualDevice) {
        wlr_keyboard_led_update(entry.keyboard, leds);
      }
    }
  }

} // namespace umbriel
