#pragma once

#include <M5Cardputer.h>
#include "environment_monitor.h"

namespace InputCompat {

// Cardputer ADV exposes the physical DEL key through KeysState::del. Keep
// compatibility with legacy KEY_BACKSPACE checks while making FN+DEL the
// universal navigation-back gesture.
inline bool isBackPressed() {
  const Keyboard_Class::KeysState &state = M5Cardputer.Keyboard.keysState();
  const bool pressed = M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) ||
                       (state.fn && state.del);
  if (pressed) EnvironmentMonitor::notifyUserActivity();
  return pressed;
}

// Read the physical Enter state as well as the legacy key lookup.  The
// physical state remains reliable when an I2C Scroll Unit is active.
inline bool isEnterPressed() {
  const Keyboard_Class::KeysState &state = M5Cardputer.Keyboard.keysState();
  const bool pressed = M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER) || state.enter;
  if (pressed) EnvironmentMonitor::notifyUserActivity();
  return pressed;
}

} // namespace InputCompat
