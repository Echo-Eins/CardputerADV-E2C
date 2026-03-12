#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\legacy\\gui_legacy_bridge.h"
/*
 * GUI Legacy Bridge - Compatibility layer for M5.Display API
 *
 * This module provides a drop-in replacement for direct M5.Display calls,
 * routing them through the new async RenderQueue system.
 *
 * Usage modes (controlled by GUI_LEGACY_BRIDGE_MODE):
 *   0 = DISABLED: Legacy Bridge is completely disabled, direct M5.Display calls only
 *   1 = PASSTHROUGH: All calls go directly to M5.Display (no queue, for debugging)
 *   2 = QUEUED: All calls are routed through RenderQueue (full async rendering)
 *   3 = HYBRID: Urgent calls go direct, others go through queue
 *
 * To disable after migration is complete:
 *   1. Set GUI_LEGACY_BRIDGE_MODE to 0 in gui_config.h
 *   2. Remove #include "gui_legacy_bridge.h" from affected files
 *   3. Replace LegacyBridge:: calls with GUI::Draw:: calls
 *
 * Memory footprint: ~2KB code, ~256 bytes RAM (when enabled)
 */

#ifndef GUI_LEGACY_BRIDGE_H
#define GUI_LEGACY_BRIDGE_H

#include "../gui_config.h"
#include "../gui_types.h"
#include "../gui_theme.h"
#include <atomic>

// Legacy bridge mode configuration
#ifndef GUI_LEGACY_BRIDGE_MODE
#define GUI_LEGACY_BRIDGE_MODE 2  // Default: QUEUED
#endif

// Mode constants
#define GUI_LEGACY_BRIDGE_DISABLED    0
#define GUI_LEGACY_BRIDGE_PASSTHROUGH 1
#define GUI_LEGACY_BRIDGE_QUEUED      2
#define GUI_LEGACY_BRIDGE_HYBRID      3

// Only include implementation if bridge is enabled
#if GUI_LEGACY_BRIDGE_MODE != GUI_LEGACY_BRIDGE_DISABLED

#include <FS.h>
#include <M5Unified.h>
#include "../core/gui_render_queue.h"
#include "../core/gui_renderer.h"
#include <cstdarg>

namespace GUI {

// ============================================================================
// Legacy Bridge State
// ============================================================================

struct LegacyBridgeState {
    // Current cursor position
    int16_t cursorX;
    int16_t cursorY;

    // Current text settings
    Color textFgColor;
    Color textBgColor;
    FontConfig font;

    // Display dimensions cache
    uint16_t displayWidth;
    uint16_t displayHeight;

    // Clipping state
    Rect clipRect;
    bool clipEnabled;

    // Statistics (atomic — incremented from producer task, read from any task)
    std::atomic<uint32_t> directCalls;
    std::atomic<uint32_t> queuedCalls;
    std::atomic<uint32_t> droppedCalls;

    void reset() {
        cursorX = 0;
        cursorY = 0;
        // Use theme colors as defaults
        textFgColor = themeColors().foreground;
        textBgColor = themeColors().background;
        font = themeFonts().normal;
        clipEnabled = false;
        directCalls.store(0, std::memory_order_relaxed);
        queuedCalls.store(0, std::memory_order_relaxed);
        droppedCalls.store(0, std::memory_order_relaxed);
    }

    // Sync with current theme
    void syncWithTheme() {
        textFgColor = themeColors().foreground;
        textBgColor = themeColors().background;
        font = themeFonts().normal;
    }
};

// ============================================================================
// Legacy Bridge Class
// ============================================================================

class LegacyBridge {
public:
    // ========================================================================
    // Lifecycle
    // ========================================================================

    // Initialize the legacy bridge (call after GUI system init)
    static bool init();

    // Shutdown the bridge
    static void shutdown();

    // Check if initialized
    static bool isInitialized();

    // Get current mode
    static int getMode();

    // Set mode at runtime (for debugging)
    static void setMode(int mode);

    // Get bridge statistics
    static const LegacyBridgeState& getState();

    // Reset statistics
    static void resetStats();

    // Sync state with current theme (call after theme change)
    static void syncWithTheme();

    // Theme-aware color getters (for use when colors need to adapt to theme)
    static Color getMenuBackground() { return themeColors().background; }
    static Color getMenuSelectedBackground() { return themeColors().highlight; }
    static Color getMenuTextFocused() { return themeColors().foreground; }
    static Color getMenuTextUnfocused() { return themeColors().disabled; }
    static Color getTaskbarBackground() { return themeColors().taskbarBg; }
    static Color getTaskbarText() { return themeColors().taskbarText; }
    static Color getTaskbarDivider() { return themeColors().border; }
    static Color getSuccess() { return themeColors().success; }
    static Color getWarning() { return themeColors().warning; }
    static Color getError() { return themeColors().error; }
    static Color getInfo() { return themeColors().info; }

    // ========================================================================
    // Screen Operations
    // ========================================================================

    // Clear screen with black
    static void clear();

    // Clear screen with specified color
    static void clear(Color color) { fillScreen(color); }
    static void clear(int color) { fillScreen(static_cast<Color>(static_cast<uint16_t>(color))); }

    // Fill entire screen with color
    static void fillScreen(Color color);

    // Flush display buffer (for compatibility - may be no-op in queued mode)
    static void display();

    // Get display dimensions
    static uint16_t width();
    static uint16_t height();

    // ========================================================================
    // Text Operations
    // ========================================================================

    // Set cursor position
    static void setCursor(int16_t x, int16_t y);

    // Get cursor position
    static int16_t getCursorX();
    static int16_t getCursorY();

    // Set text color (foreground only, black background)
    static void setTextColor(Color color);

    // Set text color (foreground and background)
    static void setTextColor(Color fg, Color bg);

    // Set text size
    static void setTextSize(float size);
    static void setTextSize(double size) { setTextSize(static_cast<float>(size)); }
    static void setTextSize(int size) { setTextSize(static_cast<float>(size)); }

    // Set text font
    static void setTextFont(uint8_t font);

    // Set text font (pointer variant, e.g. &fonts::Font0)
    static void setFont(const lgfx::IFont* font);

    // Print text (advances cursor)
    static void print(const char* text);
    static void print(const String& text);
    static void print(int value);
    static void print(unsigned int value);
    static void print(long value);
    static void print(unsigned long value);
    static void print(float value, int decimals = 2);
    static void print(double value, int decimals = 2) { print(static_cast<float>(value), decimals); }
    static void print(char c);

    // Print text with newline
    static void println(const char* text);
    static void println(const String& text);
    static void println(int value);
    static void println(unsigned int value);
    static void println(long value);
    static void println(unsigned long value);
    static void println(float value, int decimals = 2);
    static void println(double value, int decimals = 2) { println(static_cast<float>(value), decimals); }
    static void println();  // Just newline

    // Formatted print
    static void printf(const char* format, ...);

    // Get text width in pixels
    static int textWidth(const char* text);
    static int textWidth(const String& text);

    // Get font height
    static int fontHeight();

    // Draw string at position (does not advance cursor)
    static void drawString(const char* text, int16_t x, int16_t y);
    static void drawString(const String& text, int16_t x, int16_t y);

    // Draw centered string
    static void drawCentreString(const char* text, int16_t x, int16_t y);
    static void drawCentreString(const String& text, int16_t x, int16_t y);

    // Draw right-aligned string
    static void drawRightString(const char* text, int16_t x, int16_t y);
    static void drawRightString(const String& text, int16_t x, int16_t y);

    // Write single byte to display (raw character output)
    static void write(uint8_t c);

    // Draw single character
    static void drawChar(char c, int16_t x, int16_t y);

    // ========================================================================
    // Graphics Primitives
    // ========================================================================

    // Draw pixel
    static void drawPixel(int16_t x, int16_t y, Color color);

    // Draw line
    static void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color color);

    // Draw horizontal line (optimized)
    static void drawFastHLine(int16_t x, int16_t y, int16_t w, Color color);

    // Draw vertical line (optimized)
    static void drawFastVLine(int16_t x, int16_t y, int16_t h, Color color);

    // Draw rectangle outline
    static void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, Color color);

    // Draw filled rectangle
    static void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, Color color);

    // Draw rounded rectangle outline
    static void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, Color color);

    // Draw filled rounded rectangle
    static void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, Color color);

    // Draw circle outline
    static void drawCircle(int16_t x, int16_t y, int16_t r, Color color);

    // Draw filled circle
    static void fillCircle(int16_t x, int16_t y, int16_t r, Color color);

    // Draw triangle outline
    static void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                             int16_t x2, int16_t y2, Color color);

    // Draw filled triangle
    static void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                             int16_t x2, int16_t y2, Color color);

    // ========================================================================
    // Image Operations
    // ========================================================================

    // Draw raw RGB565 image
    static void pushImage(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t* data);

    // Draw JPEG from memory
    static void drawJpg(const uint8_t* data, uint32_t len, int16_t x = 0, int16_t y = 0,
                        uint16_t maxWidth = 0, uint16_t maxHeight = 0);

    // Draw JPEG from file
    static void drawJpgFile(fs::FS& fs, const char* path, int16_t x = 0, int16_t y = 0);

    // ========================================================================
    // Display Control
    // ========================================================================

    // Set brightness (0-255)
    static void setBrightness(uint8_t brightness);

    // Get current brightness
    static uint8_t getBrightness();

    // Set rotation (0-3)
    static void setRotation(uint8_t rotation);

    // Get current rotation
    static uint8_t getRotation();

    // Convert RGB to RGB565 color
    static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }

    // ========================================================================
    // Clipping
    // ========================================================================

    // Set clipping rectangle
    static void setClipRect(int16_t x, int16_t y, uint16_t w, uint16_t h);

    // Clear clipping (restore full screen)
    static void clearClipRect();

    // ========================================================================
    // Advanced / Write Transactions
    // ========================================================================

    // Start write transaction (for DMA/batch optimization)
    static void startWrite();

    // End write transaction
    static void endWrite();

    // Scroll display
    static void scroll(int16_t dx, int16_t dy);

    // ========================================================================
    // Synchronization
    // ========================================================================

    // Wait for all queued operations to complete
    static void sync();

    // Check if queue is empty
    static bool isIdle();

private:
    static LegacyBridgeState s_state;
    static bool s_initialized;
    static std::atomic<int> s_mode;

    // Internal helpers
    static bool shouldQueueCall();
    static bool pushToQueue(const RenderOp& op);
    static void updateCursorAfterPrint(const char* text);
    static void updateCursorNewline();
};

// ============================================================================
// Convenience Macros for Migration
// ============================================================================

// These macros can be used to gradually migrate code from M5.Display to LegacyBridge
// Define GUI_LEGACY_BRIDGE_MACROS to enable them

#ifdef GUI_LEGACY_BRIDGE_MACROS

// Redirect M5.Display calls to LegacyBridge
#define M5_Display_clear()                   GUI::LegacyBridge::clear()
#define M5_Display_fillScreen(c)             GUI::LegacyBridge::fillScreen(c)
#define M5_Display_display()                 GUI::LegacyBridge::display()
#define M5_Display_setCursor(x,y)            GUI::LegacyBridge::setCursor(x,y)
#define M5_Display_setTextColor(...)         GUI::LegacyBridge::setTextColor(__VA_ARGS__)
#define M5_Display_setTextSize(s)            GUI::LegacyBridge::setTextSize(s)
#define M5_Display_setTextFont(f)            GUI::LegacyBridge::setTextFont(f)
#define M5_Display_print(...)                GUI::LegacyBridge::print(__VA_ARGS__)
#define M5_Display_println(...)              GUI::LegacyBridge::println(__VA_ARGS__)
#define M5_Display_printf(...)               GUI::LegacyBridge::printf(__VA_ARGS__)
#define M5_Display_drawPixel(x,y,c)          GUI::LegacyBridge::drawPixel(x,y,c)
#define M5_Display_drawLine(x0,y0,x1,y1,c)   GUI::LegacyBridge::drawLine(x0,y0,x1,y1,c)
#define M5_Display_drawRect(x,y,w,h,c)       GUI::LegacyBridge::drawRect(x,y,w,h,c)
#define M5_Display_fillRect(x,y,w,h,c)       GUI::LegacyBridge::fillRect(x,y,w,h,c)
#define M5_Display_drawCircle(x,y,r,c)       GUI::LegacyBridge::drawCircle(x,y,r,c)
#define M5_Display_fillCircle(x,y,r,c)       GUI::LegacyBridge::fillCircle(x,y,r,c)
#define M5_Display_drawRoundRect(x,y,w,h,r,c) GUI::LegacyBridge::drawRoundRect(x,y,w,h,r,c)
#define M5_Display_setBrightness(b)          GUI::LegacyBridge::setBrightness(b)
#define M5_Display_width()                   GUI::LegacyBridge::width()
#define M5_Display_height()                  GUI::LegacyBridge::height()

#endif // GUI_LEGACY_BRIDGE_MACROS

} // namespace GUI

#endif // GUI_LEGACY_BRIDGE_MODE != GUI_LEGACY_BRIDGE_DISABLED

// ============================================================================
// Stub Macros When Bridge is Disabled
// ============================================================================

#if GUI_LEGACY_BRIDGE_MODE == GUI_LEGACY_BRIDGE_DISABLED

// When bridge is disabled, these become direct M5.Display calls
// This allows code to compile but uses legacy path

namespace GUI {
namespace LegacyBridge {
    inline bool init() { return true; }
    inline void shutdown() {}
    inline bool isInitialized() { return false; }
}
}

#endif // GUI_LEGACY_BRIDGE_MODE == GUI_LEGACY_BRIDGE_DISABLED

#endif // GUI_LEGACY_BRIDGE_H
