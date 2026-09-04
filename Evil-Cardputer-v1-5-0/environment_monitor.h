#ifndef ENVIRONMENT_MONITOR_H
#define ENVIRONMENT_MONITOR_H

#include <Arduino.h>

namespace EnvironmentMonitor {

enum class DisplayTarget : uint8_t {
    Internal = 1,
    External = 2,
    Both = 3,
};

// Starts the low-priority sensor/power-policy task. Safe to call when I2C is
// disabled; the UI remains available and can request a later rescan.
bool begin();
void end();
void poll();

// Timestamp-only and safe to call from input paths on either core.
void notifyUserActivity();

// Main application entry and redraw handoff used after direct alert overlays.
void runMenu();
bool consumeUiRefreshRequest();

} // namespace EnvironmentMonitor

#endif // ENVIRONMENT_MONITOR_H
