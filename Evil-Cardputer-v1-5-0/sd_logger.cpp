/**
 * @file sd_logger.cpp
 * @brief SD card logging system implementation.
 */

#include "sd_logger.h"
#include "display_runtime.h"
#include <SD.h>
#include <cstdarg>
#include <cstdio>
#include <new>


// Forward-declare runtimeDisplay for boot screen drawing
namespace GUI {
lgfx::LGFX_Device &runtimeDisplay();
}

namespace SdLogger {
namespace {

// ============================================================================
// Configuration
// ============================================================================
constexpr const char *kLogDir = "/evil/logs";
constexpr int kFlushInterval = 10;  // flush every N lines
constexpr int kPreInitBufSize = 30; // ring buffer for pre-SD lines
constexpr int kLineBufSize = 256;   // max formatted line length

// Boot screen config
constexpr int kScreenMarginX = 4;
constexpr int kScreenLineH = 10;
constexpr float kScreenTextSz = 1.0f;

// ============================================================================
// State
// ============================================================================
static bool s_enabled = true;
static bool s_screenLog = true;
static bool s_initialized = false;
static bool s_bootScreenOn = true;
static File s_logFile;
static int s_linesSinceFlush = 0;
static char s_logPath[64] = "";

// Boot screen cursor
static int16_t s_screenY = 2;
static int16_t s_screenMaxY = 135;

// Pre-init ring buffer (before SD is available)
struct PreLine {
  char text[kLineBufSize];
};
static PreLine *s_preBuf = nullptr;
static int s_preCount = 0;
static bool s_preAllocAttempted = false;

// ============================================================================
// Helpers
// ============================================================================
static void writeToFile(const char *line) {
  if (!s_logFile)
    return;
  s_logFile.println(line);
  s_linesSinceFlush++;
  if (s_linesSinceFlush >= kFlushInterval) {
    s_logFile.flush();
    s_linesSinceFlush = 0;
  }
}

static void drawToScreen(const char *line) {
  if (!s_bootScreenOn || !s_screenLog)
    return;

  auto &disp = GUI::runtimeDisplay();
  int16_t dispH = static_cast<int16_t>(disp.height());
  if (dispH > 0)
    s_screenMaxY = dispH;

  // Scroll: if we hit the bottom, shift up
  if (s_screenY + kScreenLineH > s_screenMaxY) {
    // Scroll by copying screen up (crude but effective on small displays)
    disp.scroll(0, -kScreenLineH);
    s_screenY -= kScreenLineH;
    // Clear last line
    disp.fillRect(0, s_screenY, static_cast<int16_t>(disp.width()),
                  kScreenLineH, 0x0000);
  }

  disp.setTextSize(kScreenTextSz);
  disp.setTextColor(0x07E0, 0x0000); // green on black
  disp.setCursor(kScreenMarginX, s_screenY);
  disp.print(line);
  s_screenY += kScreenLineH;
}

static unsigned long getUptimeMs() { return millis(); }

static void releasePreInitBuffer() {
  delete[] s_preBuf;
  s_preBuf = nullptr;
  s_preCount = 0;
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

void init() {
  if (s_initialized)
    return;

  // Create log directory
  if (!SD.exists(kLogDir)) {
    SD.mkdir(kLogDir);
  }

  // Delete old log files (keep only current session)
  File dir = SD.open(kLogDir);
  if (dir && dir.isDirectory()) {
    File entry;
    // Collect names first to avoid modifying dir while iterating
    String toDelete[16];
    int delCount = 0;
    while ((entry = dir.openNextFile()) && delCount < 16) {
      if (!entry.isDirectory()) {
        toDelete[delCount++] = String(kLogDir) + "/" + entry.name();
      }
      entry.close();
    }
    dir.close();
    for (int i = 0; i < delCount; i++) {
      SD.remove(toDelete[i].c_str());
    }
  }

  // Create new log file with uptime-based name (no RTC on Cardputer)
  unsigned long ms = getUptimeMs();
  snprintf(s_logPath, sizeof(s_logPath), "%s/boot_%lu.log", kLogDir, ms);
  s_logFile = SD.open(s_logPath, FILE_WRITE);
  if (!s_logFile) {
    Serial.printf("[SdLogger] FAILED to create %s\n", s_logPath);
    s_initialized = true;
    releasePreInitBuffer();
    return;
  }

  s_initialized = true;
  Serial.printf("[SdLogger] Logging to %s\n", s_logPath);

  // Flush pre-init buffer to file
  if (s_preBuf) {
    for (int i = 0; i < s_preCount; i++) {
      writeToFile(s_preBuf[i].text);
    }
  }
  releasePreInitBuffer();

  if (s_logFile) {
    s_logFile.flush();
  }
}

void log(char level, const char *fmt, ...) {
  char line[kLineBufSize];
  unsigned long ms = getUptimeMs();

  // Format: [T+12345][I] message
  int prefix = snprintf(line, sizeof(line), "[T+%lu][%c] ", ms, level);
  if (prefix < 0)
    prefix = 0;
  if (prefix >= (int)sizeof(line))
    prefix = sizeof(line) - 1;

  va_list args;
  va_start(args, fmt);
  vsnprintf(line + prefix, sizeof(line) - prefix, fmt, args);
  va_end(args);

  // Always to Serial
  Serial.println(line);

  // To SD or pre-buffer
  if (s_enabled) {
    if (s_initialized && s_logFile) {
      writeToFile(line);
    } else if (s_preCount < kPreInitBufSize) {
      if (!s_preAllocAttempted) {
        s_preAllocAttempted = true;
        s_preBuf = new (std::nothrow) PreLine[kPreInitBufSize];
      }
      if (s_preBuf) {
        strncpy(s_preBuf[s_preCount].text, line, kLineBufSize - 1);
        s_preBuf[s_preCount].text[kLineBufSize - 1] = '\0';
        s_preCount++;
      }
    }
  }

  // To boot screen
  drawToScreen(line);
}

void flush() {
  if (s_logFile) {
    s_logFile.flush();
    s_linesSinceFlush = 0;
  }
}

void setEnabled(bool en) { s_enabled = en; }

bool isEnabled() { return s_enabled; }

void setScreenLog(bool en) { s_screenLog = en; }

bool isScreenLogEnabled() { return s_screenLog; }

void endBootScreen() { s_bootScreenOn = false; }

void saveEnabledState() {
  // Persist via config_manager — caller should invoke saveConfigParameter
}

void restoreEnabledState() {
  // Restore via config_manager — caller should invoke restoreConfigParameter
}

} // namespace SdLogger
