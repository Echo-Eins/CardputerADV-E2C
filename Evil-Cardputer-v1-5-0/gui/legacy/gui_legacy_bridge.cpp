/*
 * GUI Legacy Bridge - Implementation
 *
 * Compatibility layer that routes runtimeDisplay()-style calls through the
 * new async RenderQueue system, enabling gradual migration.
 */

#include "gui_legacy_bridge.h"

#if GUI_LEGACY_BRIDGE_MODE != GUI_LEGACY_BRIDGE_DISABLED

#include "../core/gui_display_lock.h"
#include "../core/gui_display_target.h"
#include <Arduino.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

// Spinlock protecting the shared measurement sprite (N6 fix)
static portMUX_TYPE s_measureLock = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<uint32_t> s_writeLockDepth{0};

namespace GUI {

// ============================================================================
// Auto-Scaling for External Displays
// ============================================================================
// Legacy code assumes a 240×135 base resolution. When the runtime display
// is larger (e.g. 480×320 ILI9488), coordinates and sizes are proportionally
// scaled so the UI fills the whole screen.
// Scaling is computed once at init() and cached.

static constexpr float kBaseW = 240.0f;
static constexpr float kBaseH = 135.0f;

static float s_scaleX = 1.0f;
static float s_scaleY = 1.0f;
static bool s_scalingActive = false;

static void updateScaling(uint16_t displayW, uint16_t displayH) {
  if (displayW > 0 && displayH > 0 &&
      (displayW != static_cast<uint16_t>(kBaseW) ||
       displayH != static_cast<uint16_t>(kBaseH))) {
    s_scaleX = static_cast<float>(displayW) / kBaseW;
    s_scaleY = static_cast<float>(displayH) / kBaseH;
    s_scalingActive = true;
  } else {
    s_scaleX = 1.0f;
    s_scaleY = 1.0f;
    s_scalingActive = false;
  }
}

static inline int16_t sx(int16_t x) {
  return s_scalingActive ? static_cast<int16_t>(x * s_scaleX) : x;
}
static inline int16_t sy(int16_t y) {
  return s_scalingActive ? static_cast<int16_t>(y * s_scaleY) : y;
}
static inline int16_t sw(int16_t w) {
  return s_scalingActive ? static_cast<int16_t>(w * s_scaleX) : w;
}
static inline int16_t sh(int16_t h) {
  return s_scalingActive ? static_cast<int16_t>(h * s_scaleY) : h;
}
static inline int16_t sr(int16_t r) {
  // Radius: scale by average of scaleX/scaleY
  return s_scalingActive
             ? static_cast<int16_t>(r * (s_scaleX + s_scaleY) * 0.5f)
             : r;
}
static inline float sTextSize(float size) {
  // Scale text size by the smaller factor to avoid overflow
  return s_scalingActive ? size * ((s_scaleX < s_scaleY) ? s_scaleX : s_scaleY)
                         : size;
}

static LGFX_Sprite &getMeasureSprite(const FontConfig &font);

template <typename ReturnType>
static inline
    typename std::enable_if<!std::is_void<ReturnType>::value, ReturnType>::type
    defaultLockResult() {
  return ReturnType();
}

template <typename ReturnType>
static inline
    typename std::enable_if<std::is_void<ReturnType>::value, void>::type
    defaultLockResult() {}

template <typename Fn>
static inline auto withDisplayLock(Fn &&fn) -> decltype(fn()) {
  using ReturnType = decltype(fn());
  DisplayLockGuard lockGuard;
  if (!lockGuard.locked()) {
    GUI_LOG_ERROR("LegacyBridge: display lock acquisition failed");
    return defaultLockResult<ReturnType>();
  }
  return fn();
}

// ============================================================================
// Static Member Initialization
// ============================================================================

LegacyBridgeState LegacyBridge::s_state;
bool LegacyBridge::s_initialized = false;
std::atomic<int> LegacyBridge::s_mode{GUI_LEGACY_BRIDGE_MODE};

// ============================================================================
// Lifecycle
// ============================================================================

bool LegacyBridge::init() {
  if (s_initialized) {
    return true;
  }

  // Reset state
  s_state.reset();

  // Cache display dimensions
  withDisplayLock([&]() {
    s_state.displayWidth = runtimeDisplay().width();
    s_state.displayHeight = runtimeDisplay().height();
  });

  // Compute auto-scaling factors for legacy 240x135 code on larger displays
  updateScaling(s_state.displayWidth, s_state.displayHeight);

  // Set default clip to full screen
  s_state.clipRect =
      Rect::make(0, 0, s_state.displayWidth, s_state.displayHeight);
  s_state.clipEnabled = false;

  s_initialized = true;

  GUI_LOG("LegacyBridge initialized (mode=%d, display=%dx%d)",
          s_mode.load(std::memory_order_relaxed), s_state.displayWidth,
          s_state.displayHeight);

  return true;
}

void LegacyBridge::shutdown() {
  if (!s_initialized) {
    return;
  }

  GUI_LOG("LegacyBridge shutdown (direct=%u, queued=%u, dropped=%u)",
          s_state.directCalls.load(std::memory_order_relaxed),
          s_state.queuedCalls.load(std::memory_order_relaxed),
          s_state.droppedCalls.load(std::memory_order_relaxed));

  s_initialized = false;
}

bool LegacyBridge::isInitialized() { return s_initialized; }

int LegacyBridge::getMode() { return s_mode.load(std::memory_order_relaxed); }

void LegacyBridge::setMode(int mode) {
  if (mode >= GUI_LEGACY_BRIDGE_DISABLED && mode <= GUI_LEGACY_BRIDGE_HYBRID) {
    s_mode.store(mode, std::memory_order_relaxed);
    GUI_LOG("LegacyBridge mode changed to %d", mode);
  }
}

const LegacyBridgeState &LegacyBridge::getState() { return s_state; }

void LegacyBridge::resetStats() {
  s_state.directCalls.store(0, std::memory_order_relaxed);
  s_state.queuedCalls.store(0, std::memory_order_relaxed);
  s_state.droppedCalls.store(0, std::memory_order_relaxed);
}

void LegacyBridge::syncWithTheme() {
  s_state.syncWithTheme();

  // Also update runtimeDisplay() if not in queued mode
  if (!shouldQueueCall()) {
    withDisplayLock([&]() {
      runtimeDisplay().setTextColor(s_state.textFgColor, s_state.textBgColor);
      runtimeDisplay().setTextSize(s_state.font.getSize());
      runtimeDisplay().setTextFont(s_state.font.font);
    });
  }
}

// ============================================================================
// Internal Helpers
// ============================================================================

bool LegacyBridge::shouldQueueCall() {
#if GUI_LEGACY_BRIDGE_MODE == GUI_LEGACY_BRIDGE_PASSTHROUGH
  return false;
#else
  if (!guiIsRunning()) {
    return false;
  }

  switch (s_mode.load(std::memory_order_relaxed)) {
  case GUI_LEGACY_BRIDGE_QUEUED:
  case GUI_LEGACY_BRIDGE_HYBRID:
    return true;
  case GUI_LEGACY_BRIDGE_DISABLED:
  case GUI_LEGACY_BRIDGE_PASSTHROUGH:
  default:
    return false;
  }
#endif
}

bool LegacyBridge::pushToQueue(const RenderOp &op) {
#if GUI_LEGACY_BRIDGE_MODE == GUI_LEGACY_BRIDGE_PASSTHROUGH
  (void)op;
  return false;
#else
  if (!guiIsRunning()) {
    return false;
  }

  bool success = renderQueue().pushWithBackpressure(op);
  if (success) {
    s_state.queuedCalls.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  s_state.droppedCalls.fetch_add(1, std::memory_order_relaxed);
  const RenderQueue::BackpressureStats bp =
      renderQueue().getBackpressureStats();
  GUI_LOG_ERROR("LegacyBridge: dropped op=%u pending=%lu overflow=%lu "
                "retries=%lu drops=%lu blockTimeouts=%lu policy=%u",
                static_cast<unsigned>(op.type),
                static_cast<unsigned long>(renderQueue().pending()),
                static_cast<unsigned long>(renderQueue().getOverflowCount()),
                static_cast<unsigned long>(bp.retries),
                static_cast<unsigned long>(bp.droppedCommands),
                static_cast<unsigned long>(bp.blockTimeouts),
                static_cast<unsigned>(Config::QUEUE_OVERFLOW_POLICY));
  return false;
#endif
}

void LegacyBridge::updateCursorAfterPrint(const char *text) {
  if (text == nullptr)
    return;

  // Use textWidth() for accurate pixel-based cursor tracking
  const char *nl = strrchr(text, '\n');
  if (nl) {
    // After last newline, cursor X is width of remaining text
    s_state.cursorX = textWidth(nl + 1);
    // Count newlines for Y advancement
    for (const char *p = text; *p; ++p) {
      if (*p == '\n') {
        s_state.cursorY += fontHeight();
      }
    }
  } else {
    s_state.cursorX += textWidth(text);
  }
}

void LegacyBridge::updateCursorNewline() {
  s_state.cursorX = 0;
  s_state.cursorY += fontHeight();
}

// ============================================================================
// Screen Operations
// ============================================================================

void LegacyBridge::clear() {
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::clear(Colors::Black));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() { runtimeDisplay().clear(); });
  }

  // Reset cursor
  s_state.cursorX = 0;
  s_state.cursorY = 0;
}

void LegacyBridge::fillScreen(Color color) {
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::fillScreen(color));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() { runtimeDisplay().fillScreen(color); });
  }

  // Reset cursor
  s_state.cursorX = 0;
  s_state.cursorY = 0;
}

void LegacyBridge::display() {
  if (shouldQueueCall()) {
    // In queued mode, this is a hint to flush
    pushToQueue(RenderOps::endFrame());
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() { runtimeDisplay().display(); });
  }
}

uint16_t LegacyBridge::width() { return s_state.displayWidth; }

uint16_t LegacyBridge::height() { return s_state.displayHeight; }

// ============================================================================
// Text Operations
// ============================================================================

void LegacyBridge::setCursor(int16_t x, int16_t y) {
  s_state.cursorX = x;
  s_state.cursorY = y;

  // Also update runtimeDisplay() for consistency if in passthrough
  if (!shouldQueueCall()) {
    const int16_t rx = sx(x), ry = sy(y);
    withDisplayLock([&]() { runtimeDisplay().setCursor(rx, ry); });
  }
}

int16_t LegacyBridge::getCursorX() { return s_state.cursorX; }

int16_t LegacyBridge::getCursorY() { return s_state.cursorY; }

void LegacyBridge::setTextColor(Color color) {
  s_state.textFgColor = color;
  s_state.textBgColor = Colors::Black;

  if (!shouldQueueCall()) {
    withDisplayLock([&]() { runtimeDisplay().setTextColor(color); });
  }
}

void LegacyBridge::setTextColor(Color fg, Color bg) {
  s_state.textFgColor = fg;
  s_state.textBgColor = bg;

  if (!shouldQueueCall()) {
    withDisplayLock([&]() { runtimeDisplay().setTextColor(fg, bg); });
  }
}

void LegacyBridge::setTextSize(float size) {
  s_state.font.sizeInt = static_cast<uint8_t>(size);
  s_state.font.sizeFrac =
      static_cast<uint8_t>((size - s_state.font.sizeInt) * 100);

  if (!shouldQueueCall()) {
    const float scaledSize = sTextSize(size);
    withDisplayLock([&]() { runtimeDisplay().setTextSize(scaledSize); });
  }
}

void LegacyBridge::setTextFont(uint8_t font) {
  s_state.font.font = font;

  if (!shouldQueueCall()) {
    withDisplayLock([&]() { runtimeDisplay().setTextFont(font); });
  }
}

void LegacyBridge::setFont(const lgfx::IFont *font) {
  // Direct passthrough â€” font pointer can't be queued
  s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
  withDisplayLock([&]() { runtimeDisplay().setFont(font); });
}

void LegacyBridge::print(const char *text) {
  if (text == nullptr)
    return;

  if (shouldQueueCall()) {
    // Split text into queue-safe chunks, preserving newline behavior.
    const size_t len = strlen(text);
    size_t offset = 0;

    while (offset < len) {
      if (text[offset] == '\n') {
        updateCursorNewline();
        ++offset;
        continue;
      }

      size_t chunkLen = 0;
      while ((offset + chunkLen) < len && chunkLen < 11 &&
             text[offset + chunkLen] != '\n') {
        ++chunkLen;
      }

      if (chunkLen == 0) {
        ++offset;
        continue;
      }

      RenderOp op;
      memset(&op, 0, sizeof(op));
      op.type = RenderOpType::DrawText;
      op.priority = RenderPriority::Normal;
      op.target = DisplayTarget::Internal;
      op.data.text.pos = Point::make(s_state.cursorX, s_state.cursorY);
      op.data.text.fg = s_state.textFgColor;
      op.data.text.bg = s_state.textBgColor;
      op.data.text.font = s_state.font;
      op.data.text.textLen = static_cast<uint8_t>(chunkLen);
      memcpy(op.data.text.text, text + offset, chunkLen);
      op.data.text.text[chunkLen] = '\0';

      pushToQueue(op);

      // Cursor tracking uses the same sprite metrics as textWidth().
      portENTER_CRITICAL(&s_measureLock);
      const int chunkWidth =
          getMeasureSprite(s_state.font).textWidth(op.data.text.text);
      portEXIT_CRITICAL(&s_measureLock);
      s_state.cursorX += chunkWidth;

      offset += chunkLen;
    }
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    const int16_t rx = sx(s_state.cursorX), ry = sy(s_state.cursorY);
    const float scaledSize = sTextSize(s_state.font.getSize());
    withDisplayLock([&]() {
      runtimeDisplay().setCursor(rx, ry);
      runtimeDisplay().setTextColor(s_state.textFgColor, s_state.textBgColor);
      runtimeDisplay().setTextSize(scaledSize);
      runtimeDisplay().setTextFont(s_state.font.font);
      runtimeDisplay().print(text);

      // Update cursor from actual position
      s_state.cursorX = runtimeDisplay().getCursorX();
      s_state.cursorY = runtimeDisplay().getCursorY();
    });
  }
}

void LegacyBridge::print(const String &text) { print(text.c_str()); }

void LegacyBridge::print(int value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", value);
  print(buf);
}

void LegacyBridge::print(unsigned int value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u", value);
  print(buf);
}

void LegacyBridge::print(long value) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%ld", value);
  print(buf);
}

void LegacyBridge::print(unsigned long value) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%lu", value);
  print(buf);
}

void LegacyBridge::print(float value, int decimals) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  print(buf);
}

void LegacyBridge::print(char c) {
  char buf[2] = {c, '\0'};
  print(buf);
}

void LegacyBridge::println(const char *text) {
  print(text);
  updateCursorNewline();
}

void LegacyBridge::println(const String &text) {
  print(text);
  updateCursorNewline();
}

void LegacyBridge::println(int value) {
  print(value);
  updateCursorNewline();
}

void LegacyBridge::println(unsigned int value) {
  print(value);
  updateCursorNewline();
}

void LegacyBridge::println(long value) {
  print(value);
  updateCursorNewline();
}

void LegacyBridge::println(unsigned long value) {
  print(value);
  updateCursorNewline();
}

void LegacyBridge::println(float value, int decimals) {
  print(value, decimals);
  updateCursorNewline();
}

void LegacyBridge::println() { updateCursorNewline(); }

void LegacyBridge::printf(const char *format, ...) {
  char buf[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  print(buf);
}

// Shared measurement sprite â€” protected by s_measureLock for cross-core
// safety
static LGFX_Sprite s_measureSprite(&runtimeDisplay());
static std::atomic<bool> s_measureSpriteCreated{false};

static LGFX_Sprite &getMeasureSprite(const FontConfig &font) {
  if (!s_measureSpriteCreated) {
    s_measureSprite.createSprite(1, 1);
    s_measureSpriteCreated = true;
  }
  s_measureSprite.setTextSize(font.getSize());
  s_measureSprite.setTextFont(font.font);
  return s_measureSprite;
}

int LegacyBridge::textWidth(const char *text) {
  portENTER_CRITICAL(&s_measureLock);
  int w = getMeasureSprite(s_state.font).textWidth(text);
  portEXIT_CRITICAL(&s_measureLock);
  return w;
}

int LegacyBridge::textWidth(const String &text) {
  return textWidth(text.c_str());
}

int LegacyBridge::fontHeight() {
  portENTER_CRITICAL(&s_measureLock);
  int h = getMeasureSprite(s_state.font).fontHeight();
  portEXIT_CRITICAL(&s_measureLock);
  return h;
}

void LegacyBridge::drawString(const char *text, int16_t x, int16_t y) {
  int16_t savedX = s_state.cursorX;
  int16_t savedY = s_state.cursorY;

  setCursor(x, y);
  print(text);

  // Restore cursor (drawString doesn't advance cursor)
  s_state.cursorX = savedX;
  s_state.cursorY = savedY;
}

void LegacyBridge::drawString(const String &text, int16_t x, int16_t y) {
  drawString(text.c_str(), x, y);
}

void LegacyBridge::drawCentreString(const char *text, int16_t x, int16_t y) {
  int w = textWidth(text);
  drawString(text, x - w / 2, y);
}

void LegacyBridge::drawCentreString(const String &text, int16_t x, int16_t y) {
  drawCentreString(text.c_str(), x, y);
}

void LegacyBridge::drawRightString(const char *text, int16_t x, int16_t y) {
  int w = textWidth(text);
  drawString(text, x - w, y);
}

void LegacyBridge::drawRightString(const String &text, int16_t x, int16_t y) {
  drawRightString(text.c_str(), x, y);
}

void LegacyBridge::drawChar(char c, int16_t x, int16_t y) {
  if (shouldQueueCall()) {
    RenderOp op;
    memset(&op, 0, sizeof(op));
    op.type = RenderOpType::DrawChar;
    op.priority = RenderPriority::Normal;
    op.target = DisplayTarget::Internal;
    op.data.chr.pos = Point::make(x, y);
    op.data.chr.fg = s_state.textFgColor;
    op.data.chr.bg = s_state.textBgColor;
    op.data.chr.font = s_state.font;
    op.data.chr.ch = c;

    pushToQueue(op);
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() {
      runtimeDisplay().setTextColor(s_state.textFgColor, s_state.textBgColor);
      runtimeDisplay().setTextSize(s_state.font.getSize());
      runtimeDisplay().setTextFont(s_state.font.font);
      runtimeDisplay().setCursor(x, y);
      runtimeDisplay().print(c);
    });
  }
}

void LegacyBridge::write(uint8_t c) {
  // Direct passthrough â€” write() advances cursor internally
  s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
  withDisplayLock([&]() {
    runtimeDisplay().write(c);
    s_state.cursorX = runtimeDisplay().getCursorX();
    s_state.cursorY = runtimeDisplay().getCursorY();
  });
}

// ============================================================================
// Graphics Primitives
// ============================================================================

void LegacyBridge::drawPixel(int16_t x, int16_t y, Color color) {
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::drawPixel(sx(x), sy(y), color));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    const int16_t rx = sx(x), ry = sy(y);
    withDisplayLock([&]() { runtimeDisplay().drawPixel(rx, ry, color); });
  }
}

void LegacyBridge::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            Color color) {
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::drawLine(sx(x0), sy(y0), sx(x1), sy(y1), color));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    const int16_t rx0 = sx(x0), ry0 = sy(y0), rx1 = sx(x1), ry1 = sy(y1);
    withDisplayLock(
        [&]() { runtimeDisplay().drawLine(rx0, ry0, rx1, ry1, color); });
  }
}

void LegacyBridge::drawFastHLine(int16_t x, int16_t y, int16_t w, Color color) {
  // Optimize as fillRect with height=1
  fillRect(x, y, w, 1, color);
}

void LegacyBridge::drawFastVLine(int16_t x, int16_t y, int16_t h, Color color) {
  // Optimize as fillRect with width=1
  fillRect(x, y, 1, h, color);
}

void LegacyBridge::drawRect(int16_t x, int16_t y, int16_t w, int16_t h,
                            Color color) {
  if (w <= 0 || h <= 0)
    return;
  const int16_t rx = sx(x), ry = sy(y), rw = sw(w), rh = sh(h);
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::drawRect(rx, ry, static_cast<uint16_t>(rw),
                                    static_cast<uint16_t>(rh), color));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock(
        [&]() { runtimeDisplay().drawRect(rx, ry, rw, rh, color); });
  }
}

void LegacyBridge::fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                            Color color) {
  if (w <= 0 || h <= 0)
    return;
  const int16_t rx = sx(x), ry = sy(y), rw = sw(w), rh = sh(h);
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::fillRect(rx, ry, static_cast<uint16_t>(rw),
                                    static_cast<uint16_t>(rh), color));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock(
        [&]() { runtimeDisplay().fillRect(rx, ry, rw, rh, color); });
  }
}

void LegacyBridge::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                                 int16_t r, Color color) {
  if (w <= 0 || h <= 0)
    return;
  const int16_t rx = sx(x), ry = sy(y), rw = sw(w), rh = sh(h), rr = sr(r);
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::drawRoundRect(rx, ry, static_cast<uint16_t>(rw),
                                         static_cast<uint16_t>(rh), rr, color));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock(
        [&]() { runtimeDisplay().drawRoundRect(rx, ry, rw, rh, rr, color); });
  }
}

void LegacyBridge::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                                 int16_t r, Color color) {
  if (w <= 0 || h <= 0)
    return;
  const int16_t rx = sx(x), ry = sy(y), rw = sw(w), rh = sh(h), rr = sr(r);
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::fillRoundRect(rx, ry, static_cast<uint16_t>(rw),
                                         static_cast<uint16_t>(rh), rr, color));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock(
        [&]() { runtimeDisplay().fillRoundRect(rx, ry, rw, rh, rr, color); });
  }
}

void LegacyBridge::drawCircle(int16_t x, int16_t y, int16_t r, Color color) {
  const int16_t rx = sx(x), ry = sy(y), rr = sr(r);
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::drawCircle(rx, ry, rr, color));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() { runtimeDisplay().drawCircle(rx, ry, rr, color); });
  }
}

void LegacyBridge::fillCircle(int16_t x, int16_t y, int16_t r, Color color) {
  const int16_t rx = sx(x), ry = sy(y), rr = sr(r);
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::fillCircle(rx, ry, rr, color));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() { runtimeDisplay().fillCircle(rx, ry, rr, color); });
  }
}

void LegacyBridge::drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                int16_t x2, int16_t y2, Color color) {
  const int16_t rx0 = sx(x0), ry0 = sy(y0), rx1 = sx(x1), ry1 = sy(y1),
                rx2 = sx(x2), ry2 = sy(y2);
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::drawTriangle(rx0, ry0, rx1, ry1, rx2, ry2, color));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() {
      runtimeDisplay().drawTriangle(rx0, ry0, rx1, ry1, rx2, ry2, color);
    });
  }
}

void LegacyBridge::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                int16_t x2, int16_t y2, Color color) {
  const int16_t rx0 = sx(x0), ry0 = sy(y0), rx1 = sx(x1), ry1 = sy(y1),
                rx2 = sx(x2), ry2 = sy(y2);
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::fillTriangle(rx0, ry0, rx1, ry1, rx2, ry2, color));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() {
      runtimeDisplay().fillTriangle(rx0, ry0, rx1, ry1, rx2, ry2, color);
    });
  }
}

// ============================================================================
// Image Operations
// ============================================================================

void LegacyBridge::pushImage(int16_t x, int16_t y, uint16_t w, uint16_t h,
                             const uint16_t *data) {
  if (data == nullptr || w == 0 || h == 0)
    return;

  if (shouldQueueCall()) {
    const size_t bytes = static_cast<size_t>(w) * h * sizeof(uint16_t);
    uint16_t *owned = static_cast<uint16_t *>(std::malloc(bytes));
    if (!owned) {
      s_state.droppedCalls.fetch_add(1, std::memory_order_relaxed);
      GUI_LOG_ERROR("LegacyBridge: pushImage alloc failed (%u x %u)", w, h);
      s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
      withDisplayLock([&]() { runtimeDisplay().pushImage(x, y, w, h, data); });
      return;
    }
    memcpy(owned, data, bytes);

    RenderOp op;
    memset(&op, 0, sizeof(op));
    op.type = RenderOpType::DrawBitmap;
    op.priority = RenderPriority::Normal;
    op.target = DisplayTarget::Internal;
    op.data.bitmap.rect = Rect::make(x, y, w, h);
    op.data.bitmap.data = owned;
    op.data.bitmap.ownsData = 1;

    if (!pushToQueue(op)) {
      std::free(owned);
      s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
      withDisplayLock([&]() { runtimeDisplay().pushImage(x, y, w, h, data); });
    }
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() { runtimeDisplay().pushImage(x, y, w, h, data); });
  }
}

void LegacyBridge::drawJpg(const uint8_t *data, uint32_t len, int16_t x,
                           int16_t y, uint16_t maxWidth, uint16_t maxHeight) {
  if (data == nullptr || len == 0)
    return;

  if (shouldQueueCall()) {
    uint8_t *owned = static_cast<uint8_t *>(std::malloc(len));
    if (!owned) {
      s_state.droppedCalls.fetch_add(1, std::memory_order_relaxed);
      GUI_LOG_ERROR("LegacyBridge: drawJpg alloc failed (len=%lu)", len);
      s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
      if (maxWidth > 0 && maxHeight > 0) {
        withDisplayLock([&]() {
          runtimeDisplay().drawJpg(data, len, x, y, maxWidth, maxHeight);
        });
      } else {
        withDisplayLock([&]() { runtimeDisplay().drawJpg(data, len, x, y); });
      }
      return;
    }
    memcpy(owned, data, len);

    RenderOp op;
    memset(&op, 0, sizeof(op));
    op.type = RenderOpType::DrawJpeg;
    op.priority = RenderPriority::Normal;
    op.target = DisplayTarget::Internal;
    op.data.jpeg.pos = Point::make(x, y);
    op.data.jpeg.data = owned;
    op.data.jpeg.len = len;
    op.data.jpeg.maxWidth = maxWidth;
    op.data.jpeg.maxHeight = maxHeight;
    op.data.jpeg.ownsData = 1;

    if (!pushToQueue(op)) {
      std::free(owned);
      s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
      if (maxWidth > 0 && maxHeight > 0) {
        withDisplayLock([&]() {
          runtimeDisplay().drawJpg(data, len, x, y, maxWidth, maxHeight);
        });
      } else {
        withDisplayLock([&]() { runtimeDisplay().drawJpg(data, len, x, y); });
      }
    }
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    if (maxWidth > 0 && maxHeight > 0) {
      withDisplayLock([&]() {
        runtimeDisplay().drawJpg(data, len, x, y, maxWidth, maxHeight);
      });
    } else {
      withDisplayLock([&]() { runtimeDisplay().drawJpg(data, len, x, y); });
    }
  }
}

void LegacyBridge::drawJpgFile(fs::FS &fs, const char *path, int16_t x,
                               int16_t y) {
  // File operations always go direct (can't queue file handles safely)
  s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
  withDisplayLock([&]() { runtimeDisplay().drawJpgFile(fs, path, x, y); });
}

// ============================================================================
// Display Control
// ============================================================================

void LegacyBridge::setBrightness(uint8_t brightness) {
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::setBrightness(brightness));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() { runtimeDisplay().setBrightness(brightness); });
  }
}

uint8_t LegacyBridge::getBrightness() {
  // Always direct - read operation
  return withDisplayLock(
      [&]() -> uint8_t { return runtimeDisplay().getBrightness(); });
}

void LegacyBridge::setRotation(uint8_t rotation) {
  // Always direct - affects display state
  s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
  withDisplayLock([&]() {
    runtimeDisplay().setRotation(rotation);

    // Update cached dimensions
    s_state.displayWidth = runtimeDisplay().width();
    s_state.displayHeight = runtimeDisplay().height();
  });
}

uint8_t LegacyBridge::getRotation() {
  return withDisplayLock(
      [&]() -> uint8_t { return runtimeDisplay().getRotation(); });
}

// ============================================================================
// Clipping
// ============================================================================

void LegacyBridge::setClipRect(int16_t x, int16_t y, uint16_t w, uint16_t h) {
  s_state.clipRect = Rect::make(x, y, w, h);
  s_state.clipEnabled = true;

  if (shouldQueueCall()) {
    pushToQueue(RenderOps::setClip(x, y, w, h));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() { runtimeDisplay().setClipRect(x, y, w, h); });
  }
}

void LegacyBridge::clearClipRect() {
  s_state.clipEnabled = false;
  s_state.clipRect =
      Rect::make(0, 0, s_state.displayWidth, s_state.displayHeight);

  if (shouldQueueCall()) {
    pushToQueue(RenderOps::clearClip());
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() { runtimeDisplay().clearClipRect(); });
  }
}

// ============================================================================
// Advanced / Write Transactions
// ============================================================================

void LegacyBridge::startWrite() {
  // Write transactions are M5GFX optimization - always go direct
  s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
  if (shouldQueueCall()) {
    withDisplayLock([&]() { runtimeDisplay().startWrite(); });
    return;
  }

  if (lockDisplay()) {
    s_writeLockDepth.fetch_add(1, std::memory_order_relaxed);
  }
  runtimeDisplay().startWrite();
}

void LegacyBridge::endWrite() {
  s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
  if (shouldQueueCall()) {
    withDisplayLock([&]() { runtimeDisplay().endWrite(); });
    return;
  }

  runtimeDisplay().endWrite();
  uint32_t depth = s_writeLockDepth.load(std::memory_order_relaxed);
  if (depth > 0) {
    s_writeLockDepth.fetch_sub(1, std::memory_order_relaxed);
    unlockDisplay();
  }
}

void LegacyBridge::scroll(int16_t dx, int16_t dy) {
  if (shouldQueueCall()) {
    pushToQueue(RenderOps::scroll(dx, dy));
  } else {
    s_state.directCalls.fetch_add(1, std::memory_order_relaxed);
    withDisplayLock([&]() { runtimeDisplay().scroll(dx, dy); });
  }
}

// ============================================================================
// Synchronization
// ============================================================================

void LegacyBridge::sync() {
#if GUI_LEGACY_BRIDGE_MODE == GUI_LEGACY_BRIDGE_PASSTHROUGH
  return;
#else
  if (guiIsRunning()) {
    if (!renderQueue().sync()) {
      GUI_LOG_ERROR("LegacyBridge: sync timeout");
    }
  }
#endif
}

bool LegacyBridge::isIdle() {
#if GUI_LEGACY_BRIDGE_MODE == GUI_LEGACY_BRIDGE_PASSTHROUGH
  return true;
#else
  if (guiIsRunning()) {
    return renderQueue().isEmpty();
  }
  return true;
#endif
}

} // namespace GUI

#endif // GUI_LEGACY_BRIDGE_MODE != GUI_LEGACY_BRIDGE_DISABLED
