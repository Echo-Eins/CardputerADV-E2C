/**
 * @file gui_draw.h
 * @brief Drawing API wrapper for widget rendering
 *
 * Provides a clean drawing interface for widgets that maps to
 * the async renderer's GUI::Draw functions.
 *
 * Widgets use these functions instead of direct M5.Display calls
 * to ensure all rendering goes through the async queue.
 */

#ifndef GUI_WIDGET_DRAW_H
#define GUI_WIDGET_DRAW_H

#include "../gui.h"
#include "../gui_types.h"
#include "../gui_theme.h"

namespace GUI {

/**
 * @brief Drawing functions for widgets
 *
 * These functions wrap GUI::Draw to provide:
 * - Consistent API for widgets
 * - Text size support
 * - Clipping region support
 * - Theme color integration
 */
namespace WidgetDraw {

//=============================================================================
// Basic Primitives
//=============================================================================

/**
 * @brief Fill a rectangle
 */
inline bool fillRect(int16_t x, int16_t y, int16_t w, int16_t h, Color color) {
    return Draw::fillRect(x, y, w, h, color);
}

/**
 * @brief Draw a rectangle outline
 */
inline bool drawRect(int16_t x, int16_t y, int16_t w, int16_t h, Color color) {
    return Draw::drawRect(x, y, w, h, color);
}

/**
 * @brief Draw a line
 */
inline bool drawLine(int16_t x1, int16_t y1, int16_t x2, int16_t y2, Color color) {
    return Draw::drawLine(x1, y1, x2, y2, color);
}

/**
 * @brief Draw a single pixel
 */
inline bool drawPixel(int16_t x, int16_t y, Color color) {
    return Draw::drawPixel(x, y, color);
}

//=============================================================================
// Rounded Rectangles
//=============================================================================

/**
 * @brief Fill a rounded rectangle
 */
inline bool fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                          int16_t r, Color color) {
    return Draw::fillRoundRect(x, y, w, h, r, color);
}

/**
 * @brief Draw a rounded rectangle outline
 */
inline bool drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                          int16_t r, Color color) {
    return Draw::drawRoundRect(x, y, w, h, r, color);
}

//=============================================================================
// Circles
//=============================================================================

/**
 * @brief Fill a circle
 */
inline bool fillCircle(int16_t x, int16_t y, int16_t r, Color color) {
    return Draw::fillCircle(x, y, r, color);
}

/**
 * @brief Draw a circle outline
 */
inline bool drawCircle(int16_t x, int16_t y, int16_t r, Color color) {
    return Draw::drawCircle(x, y, r, color);
}

//=============================================================================
// Triangles
//=============================================================================

/**
 * @brief Fill a triangle
 */
inline bool fillTriangle(int16_t x0, int16_t y0,
                         int16_t x1, int16_t y1,
                         int16_t x2, int16_t y2,
                         Color color) {
    return Draw::fillTriangle(x0, y0, x1, y1, x2, y2, color);
}

/**
 * @brief Draw a triangle outline
 */
inline bool drawTriangle(int16_t x0, int16_t y0,
                         int16_t x1, int16_t y1,
                         int16_t x2, int16_t y2,
                         Color color) {
    return Draw::drawTriangle(x0, y0, x1, y1, x2, y2, color);
}

//=============================================================================
// Text
//=============================================================================

/**
 * @brief Draw text with size support
 *
 * @param x X position
 * @param y Y position
 * @param text Text string
 * @param color Text color
 * @param size Text size multiplier (1-3)
 * @param bg Background color (optional, default black)
 */
inline bool drawText(int16_t x, int16_t y, const char* text,
                     Color color, uint8_t size = 1, Color bg = Colors::Black) {
    // Note: The async renderer's drawText doesn't have size parameter
    // We need to handle this differently - for now use standard drawText
    // Text size would be handled by the M5GFX font system
    (void)size;  // TODO: Implement proper text size handling
    return Draw::drawText(x, y, text, color, bg);
}

//=============================================================================
// Screen Operations
//=============================================================================

/**
 * @brief Clear screen with color
 */
inline bool clear(Color color = Colors::Black) {
    return Draw::clear(color);
}

/**
 * @brief Fill entire screen
 */
inline bool fillScreen(Color color) {
    return Draw::fillScreen(color);
}

//=============================================================================
// Clipping
//=============================================================================

/**
 * @brief Set clipping rectangle
 *
 * All subsequent drawing will be clipped to this region.
 */
inline bool setClip(int16_t x, int16_t y, int16_t w, int16_t h) {
    // Push clip command to queue
    return renderQueue().push(RenderOps::setClip(x, y, w, h));
}

/**
 * @brief Clear clipping rectangle
 */
inline bool clearClip() {
    return renderQueue().push(RenderOps::clearClip());
}

//=============================================================================
// Synchronization
//=============================================================================

/**
 * @brief Signal end of frame
 */
inline bool endFrame() {
    return Draw::endFrame();
}

/**
 * @brief Wait for all commands to complete
 */
inline void sync() {
    Draw::sync();
}

//=============================================================================
// Theme-Aware Colors
//=============================================================================

/**
 * @brief Get background color from theme
 */
inline Color backgroundColor() {
    return ThemeManager::instance().theme().menuBackgroundColor();
}

/**
 * @brief Get foreground/text color from theme
 */
inline Color foregroundColor() {
    return ThemeManager::instance().theme().menuTextUnFocusedColor();
}

/**
 * @brief Get focused text color from theme
 */
inline Color focusedColor() {
    return ThemeManager::instance().theme().menuTextFocusedColor();
}

/**
 * @brief Get selection/highlight color from theme
 */
inline Color selectionColor() {
    return ThemeManager::instance().theme().menuSelectedBackgroundColor();
}

/**
 * @brief Get divider color from theme
 */
inline Color dividerColor() {
    return ThemeManager::instance().theme().taskbarDividerColor();
}

} // namespace WidgetDraw

// Alias for shorter access
namespace Draw = WidgetDraw;

} // namespace GUI

#endif // GUI_WIDGET_DRAW_H
