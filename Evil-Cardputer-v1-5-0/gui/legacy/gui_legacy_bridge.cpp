/*
 * GUI Legacy Bridge - Implementation
 *
 * Compatibility layer that routes M5.Display-style calls through the
 * new async RenderQueue system, enabling gradual migration.
 */

#include "gui_legacy_bridge.h"

#if GUI_LEGACY_BRIDGE_MODE != GUI_LEGACY_BRIDGE_DISABLED

#include <cstdio>
#include <cstring>
#include <Arduino.h>

namespace GUI {

// ============================================================================
// Static Member Initialization
// ============================================================================

LegacyBridgeState LegacyBridge::s_state;
bool LegacyBridge::s_initialized = false;
int LegacyBridge::s_mode = GUI_LEGACY_BRIDGE_MODE;

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
    s_state.displayWidth = M5.Display.width();
    s_state.displayHeight = M5.Display.height();

    // Set default clip to full screen
    s_state.clipRect = Rect::make(0, 0, s_state.displayWidth, s_state.displayHeight);
    s_state.clipEnabled = false;

    s_initialized = true;

    GUI_LOG("LegacyBridge initialized (mode=%d, display=%dx%d)",
            s_mode, s_state.displayWidth, s_state.displayHeight);

    return true;
}

void LegacyBridge::shutdown() {
    if (!s_initialized) {
        return;
    }

    GUI_LOG("LegacyBridge shutdown (direct=%u, queued=%u, dropped=%u)",
            s_state.directCalls, s_state.queuedCalls, s_state.droppedCalls);

    s_initialized = false;
}

bool LegacyBridge::isInitialized() {
    return s_initialized;
}

int LegacyBridge::getMode() {
    return s_mode;
}

void LegacyBridge::setMode(int mode) {
    if (mode >= GUI_LEGACY_BRIDGE_DISABLED && mode <= GUI_LEGACY_BRIDGE_HYBRID) {
        s_mode = mode;
        GUI_LOG("LegacyBridge mode changed to %d", mode);
    }
}

const LegacyBridgeState& LegacyBridge::getState() {
    return s_state;
}

void LegacyBridge::resetStats() {
    s_state.directCalls = 0;
    s_state.queuedCalls = 0;
    s_state.droppedCalls = 0;
}

void LegacyBridge::syncWithTheme() {
    s_state.syncWithTheme();

    // Also update M5.Display if not in queued mode
    if (!shouldQueueCall()) {
        M5.Display.setTextColor(s_state.textFgColor, s_state.textBgColor);
        M5.Display.setTextSize(s_state.font.getSize());
        M5.Display.setTextFont(s_state.font.font);
    }
}

// ============================================================================
// Internal Helpers
// ============================================================================

bool LegacyBridge::shouldQueueCall() {
#if GUI_LEGACY_BRIDGE_MODE == GUI_LEGACY_BRIDGE_PASSTHROUGH
    return false;
#elif GUI_LEGACY_BRIDGE_MODE == GUI_LEGACY_BRIDGE_QUEUED
    return guiIsRunning();
#elif GUI_LEGACY_BRIDGE_MODE == GUI_LEGACY_BRIDGE_HYBRID
    return guiIsRunning();
#else
    return false;
#endif
}

bool LegacyBridge::pushToQueue(const RenderOp& op) {
    if (!guiIsRunning()) {
        return false;
    }

    bool success = renderQueue().push(op);
    if (success) {
        s_state.queuedCalls++;
    } else {
        s_state.droppedCalls++;
        GUI_LOG_ERROR("LegacyBridge: queue full, dropped call");
    }
    return success;
}

void LegacyBridge::updateCursorAfterPrint(const char* text) {
    if (text == nullptr) return;

    // Estimate cursor movement (simplified - actual depends on font metrics)
    int len = strlen(text);
    int charWidth = 6 * s_state.font.getSize();  // Approximate char width

    // Check for newlines
    const char* nl = strchr(text, '\n');
    if (nl) {
        // Move to start of next line
        s_state.cursorX = 0;
        s_state.cursorY += s_state.font.getSize() * 10;  // Approximate line height
    } else {
        s_state.cursorX += len * charWidth;
    }
}

void LegacyBridge::updateCursorNewline() {
    s_state.cursorX = 0;
    s_state.cursorY += s_state.font.getSize() * 10;  // Approximate line height
}

// ============================================================================
// Screen Operations
// ============================================================================

void LegacyBridge::clear() {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::clear(Colors::Black));
    } else {
        s_state.directCalls++;
        M5.Display.clear();
    }

    // Reset cursor
    s_state.cursorX = 0;
    s_state.cursorY = 0;
}

void LegacyBridge::fillScreen(Color color) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::fillScreen(color));
    } else {
        s_state.directCalls++;
        M5.Display.fillScreen(color);
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
        s_state.directCalls++;
        M5.Display.display();
    }
}

uint16_t LegacyBridge::width() {
    return s_state.displayWidth;
}

uint16_t LegacyBridge::height() {
    return s_state.displayHeight;
}

// ============================================================================
// Text Operations
// ============================================================================

void LegacyBridge::setCursor(int16_t x, int16_t y) {
    s_state.cursorX = x;
    s_state.cursorY = y;

    // Also update M5.Display for consistency if in passthrough
    if (!shouldQueueCall()) {
        M5.Display.setCursor(x, y);
    }
}

int16_t LegacyBridge::getCursorX() {
    return s_state.cursorX;
}

int16_t LegacyBridge::getCursorY() {
    return s_state.cursorY;
}

void LegacyBridge::setTextColor(Color color) {
    s_state.textFgColor = color;
    s_state.textBgColor = Colors::Black;

    if (!shouldQueueCall()) {
        M5.Display.setTextColor(color);
    }
}

void LegacyBridge::setTextColor(Color fg, Color bg) {
    s_state.textFgColor = fg;
    s_state.textBgColor = bg;

    if (!shouldQueueCall()) {
        M5.Display.setTextColor(fg, bg);
    }
}

void LegacyBridge::setTextSize(float size) {
    s_state.font.sizeInt = static_cast<uint8_t>(size);
    s_state.font.sizeFrac = static_cast<uint8_t>((size - s_state.font.sizeInt) * 100);

    if (!shouldQueueCall()) {
        M5.Display.setTextSize(size);
    }
}

void LegacyBridge::setTextFont(uint8_t font) {
    s_state.font.font = font;

    if (!shouldQueueCall()) {
        M5.Display.setTextFont(font);
    }
}

void LegacyBridge::print(const char* text) {
    if (text == nullptr) return;

    if (shouldQueueCall()) {
        // For longer text, split into chunks
        size_t len = strlen(text);
        size_t offset = 0;

        while (offset < len) {
            size_t chunkLen = (len - offset > 11) ? 11 : (len - offset);

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
            strncpy(op.data.text.text, text + offset, 12);
            op.data.text.text[11] = '\0';

            pushToQueue(op);

            // Estimate cursor advancement
            int charWidth = 6 * s_state.font.getSize();
            s_state.cursorX += chunkLen * charWidth;

            offset += chunkLen;
        }
    } else {
        s_state.directCalls++;
        M5.Display.setCursor(s_state.cursorX, s_state.cursorY);
        M5.Display.setTextColor(s_state.textFgColor, s_state.textBgColor);
        M5.Display.setTextSize(s_state.font.getSize());
        M5.Display.setTextFont(s_state.font.font);
        M5.Display.print(text);

        // Update cursor from actual position
        s_state.cursorX = M5.Display.getCursorX();
        s_state.cursorY = M5.Display.getCursorY();
    }
}

void LegacyBridge::print(const String& text) {
    print(text.c_str());
}

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

void LegacyBridge::println(const char* text) {
    print(text);
    updateCursorNewline();
}

void LegacyBridge::println(const String& text) {
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

void LegacyBridge::println() {
    updateCursorNewline();
}

void LegacyBridge::printf(const char* format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    print(buf);
}

int LegacyBridge::textWidth(const char* text) {
    // Always use M5.Display for measurement (sync operation)
    M5.Display.setTextSize(s_state.font.getSize());
    M5.Display.setTextFont(s_state.font.font);
    return M5.Display.textWidth(text);
}

int LegacyBridge::textWidth(const String& text) {
    return textWidth(text.c_str());
}

int LegacyBridge::fontHeight() {
    M5.Display.setTextSize(s_state.font.getSize());
    M5.Display.setTextFont(s_state.font.font);
    return M5.Display.fontHeight();
}

void LegacyBridge::drawString(const char* text, int16_t x, int16_t y) {
    int16_t savedX = s_state.cursorX;
    int16_t savedY = s_state.cursorY;

    setCursor(x, y);
    print(text);

    // Restore cursor (drawString doesn't advance cursor)
    s_state.cursorX = savedX;
    s_state.cursorY = savedY;
}

void LegacyBridge::drawString(const String& text, int16_t x, int16_t y) {
    drawString(text.c_str(), x, y);
}

void LegacyBridge::drawCentreString(const char* text, int16_t x, int16_t y) {
    int w = textWidth(text);
    drawString(text, x - w / 2, y);
}

void LegacyBridge::drawCentreString(const String& text, int16_t x, int16_t y) {
    drawCentreString(text.c_str(), x, y);
}

void LegacyBridge::drawRightString(const char* text, int16_t x, int16_t y) {
    int w = textWidth(text);
    drawString(text, x - w, y);
}

void LegacyBridge::drawRightString(const String& text, int16_t x, int16_t y) {
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
        s_state.directCalls++;
        M5.Display.setTextColor(s_state.textFgColor, s_state.textBgColor);
        M5.Display.setTextSize(s_state.font.getSize());
        M5.Display.setTextFont(s_state.font.font);
        M5.Display.setCursor(x, y);
        M5.Display.print(c);
    }
}

// ============================================================================
// Graphics Primitives
// ============================================================================

void LegacyBridge::drawPixel(int16_t x, int16_t y, Color color) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::drawPixel(x, y, color));
    } else {
        s_state.directCalls++;
        M5.Display.drawPixel(x, y, color);
    }
}

void LegacyBridge::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color color) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::drawLine(x0, y0, x1, y1, color));
    } else {
        s_state.directCalls++;
        M5.Display.drawLine(x0, y0, x1, y1, color);
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

void LegacyBridge::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, Color color) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::drawRect(x, y, static_cast<uint16_t>(w),
                                        static_cast<uint16_t>(h), color));
    } else {
        s_state.directCalls++;
        M5.Display.drawRect(x, y, w, h, color);
    }
}

void LegacyBridge::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, Color color) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::fillRect(x, y, static_cast<uint16_t>(w),
                                        static_cast<uint16_t>(h), color));
    } else {
        s_state.directCalls++;
        M5.Display.fillRect(x, y, w, h, color);
    }
}

void LegacyBridge::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, Color color) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::drawRoundRect(x, y, static_cast<uint16_t>(w),
                                             static_cast<uint16_t>(h), r, color));
    } else {
        s_state.directCalls++;
        M5.Display.drawRoundRect(x, y, w, h, r, color);
    }
}

void LegacyBridge::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, Color color) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::fillRoundRect(x, y, static_cast<uint16_t>(w),
                                             static_cast<uint16_t>(h), r, color));
    } else {
        s_state.directCalls++;
        M5.Display.fillRoundRect(x, y, w, h, r, color);
    }
}

void LegacyBridge::drawCircle(int16_t x, int16_t y, int16_t r, Color color) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::drawCircle(x, y, r, color));
    } else {
        s_state.directCalls++;
        M5.Display.drawCircle(x, y, r, color);
    }
}

void LegacyBridge::fillCircle(int16_t x, int16_t y, int16_t r, Color color) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::fillCircle(x, y, r, color));
    } else {
        s_state.directCalls++;
        M5.Display.fillCircle(x, y, r, color);
    }
}

void LegacyBridge::drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                 int16_t x2, int16_t y2, Color color) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::drawTriangle(x0, y0, x1, y1, x2, y2, color));
    } else {
        s_state.directCalls++;
        M5.Display.drawTriangle(x0, y0, x1, y1, x2, y2, color);
    }
}

void LegacyBridge::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                 int16_t x2, int16_t y2, Color color) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::fillTriangle(x0, y0, x1, y1, x2, y2, color));
    } else {
        s_state.directCalls++;
        M5.Display.fillTriangle(x0, y0, x1, y1, x2, y2, color);
    }
}

// ============================================================================
// Image Operations
// ============================================================================

void LegacyBridge::pushImage(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t* data) {
    if (shouldQueueCall()) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::DrawBitmap;
        op.priority = RenderPriority::Normal;
        op.target = DisplayTarget::Internal;
        op.data.bitmap.rect = Rect::make(x, y, w, h);
        op.data.bitmap.data = data;

        pushToQueue(op);
    } else {
        s_state.directCalls++;
        M5.Display.pushImage(x, y, w, h, data);
    }
}

void LegacyBridge::drawJpg(const uint8_t* data, uint32_t len, int16_t x, int16_t y,
                           uint16_t maxWidth, uint16_t maxHeight) {
    if (shouldQueueCall()) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::DrawJpeg;
        op.priority = RenderPriority::Normal;
        op.target = DisplayTarget::Internal;
        op.data.jpeg.pos = Point::make(x, y);
        op.data.jpeg.data = data;
        op.data.jpeg.len = len;
        op.data.jpeg.maxWidth = maxWidth;
        op.data.jpeg.maxHeight = maxHeight;

        pushToQueue(op);
    } else {
        s_state.directCalls++;
        if (maxWidth > 0 && maxHeight > 0) {
            M5.Display.drawJpg(data, len, x, y, maxWidth, maxHeight);
        } else {
            M5.Display.drawJpg(data, len, x, y);
        }
    }
}

void LegacyBridge::drawJpgFile(fs::FS& fs, const char* path, int16_t x, int16_t y) {
    // File operations always go direct (can't queue file handles safely)
    s_state.directCalls++;
    M5.Display.drawJpgFile(fs, path, x, y);
}

// ============================================================================
// Display Control
// ============================================================================

void LegacyBridge::setBrightness(uint8_t brightness) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::setBrightness(brightness));
    } else {
        s_state.directCalls++;
        M5.Display.setBrightness(brightness);
    }
}

uint8_t LegacyBridge::getBrightness() {
    // Always direct - read operation
    return M5.Display.getBrightness();
}

void LegacyBridge::setRotation(uint8_t rotation) {
    // Always direct - affects display state
    s_state.directCalls++;
    M5.Display.setRotation(rotation);

    // Update cached dimensions
    s_state.displayWidth = M5.Display.width();
    s_state.displayHeight = M5.Display.height();
}

uint8_t LegacyBridge::getRotation() {
    return M5.Display.getRotation();
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
        s_state.directCalls++;
        M5.Display.setClipRect(x, y, w, h);
    }
}

void LegacyBridge::clearClipRect() {
    s_state.clipEnabled = false;
    s_state.clipRect = Rect::make(0, 0, s_state.displayWidth, s_state.displayHeight);

    if (shouldQueueCall()) {
        pushToQueue(RenderOps::clearClip());
    } else {
        s_state.directCalls++;
        M5.Display.clearClipRect();
    }
}

// ============================================================================
// Advanced / Write Transactions
// ============================================================================

void LegacyBridge::startWrite() {
    // Write transactions are M5GFX optimization - always go direct
    s_state.directCalls++;
    M5.Display.startWrite();
}

void LegacyBridge::endWrite() {
    s_state.directCalls++;
    M5.Display.endWrite();
}

void LegacyBridge::scroll(int16_t dx, int16_t dy) {
    if (shouldQueueCall()) {
        pushToQueue(RenderOps::scroll(dx, dy));
    } else {
        s_state.directCalls++;
        M5.Display.scroll(dx, dy);
    }
}

// ============================================================================
// Synchronization
// ============================================================================

void LegacyBridge::sync() {
    if (guiIsRunning()) {
        renderQueue().sync();
    }
}

bool LegacyBridge::isIdle() {
    if (guiIsRunning()) {
        return renderQueue().isEmpty();
    }
    return true;
}

} // namespace GUI

#endif // GUI_LEGACY_BRIDGE_MODE != GUI_LEGACY_BRIDGE_DISABLED
