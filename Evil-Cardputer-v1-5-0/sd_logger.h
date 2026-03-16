/**
 * @file sd_logger.h
 * @brief SD card logging system with boot-screen output.
 *
 * Usage:
 *   SdLogger::init();          // After SD.begin()
 *   ELOG_I("Boot phase: %s", "wifi");
 *   ELOG_W("Low heap: %u", freeHeap);
 *   ELOG_E("SD write failed");
 *
 * All output goes to both Serial and SD card log file.
 * Before SdLogger::init(), messages are buffered in a small ring buffer
 * and flushed to SD once init() completes.
 *
 * Boot screen mode: when enabled, log lines are also drawn to the display
 * in a scrolling terminal fashion until post-boot.
 */

#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include <Arduino.h>

namespace SdLogger {

/// Initialize logger. Must be called AFTER SD.begin().
/// Deletes previous log file, creates new one with timestamp.
void init();

/// Core log function. Level is a single char: 'I', 'W', 'E', 'D'.
void log(char level, const char *fmt, ...);

/// Flush any buffered writes to SD (called automatically every N lines).
void flush();

/// Enable/disable logging to SD file. Serial always active.
void setEnabled(bool en);
bool isEnabled();

/// Enable/disable log output on display during boot.
void setScreenLog(bool en);
bool isScreenLogEnabled();

/// Signal that boot is complete — stop screen logging.
void endBootScreen();

/// Toggle setting persisted in config.
void saveEnabledState();
void restoreEnabledState();

} // namespace SdLogger

// Convenience macros — compile to nothing if you #define ELOG_DISABLE
#ifndef ELOG_DISABLE
#define ELOG_I(fmt, ...) SdLogger::log('I', fmt, ##__VA_ARGS__)
#define ELOG_W(fmt, ...) SdLogger::log('W', fmt, ##__VA_ARGS__)
#define ELOG_E(fmt, ...) SdLogger::log('E', fmt, ##__VA_ARGS__)
#define ELOG_D(fmt, ...) SdLogger::log('D', fmt, ##__VA_ARGS__)
#else
#define ELOG_I(fmt, ...) ((void)0)
#define ELOG_W(fmt, ...) ((void)0)
#define ELOG_E(fmt, ...) ((void)0)
#define ELOG_D(fmt, ...) ((void)0)
#endif

#endif // SD_LOGGER_H
