/*
 * GUI Types - Basic type definitions for the GUI framework
 *
 * This file defines fundamental types used throughout the async GUI system:
 * - Color (RGB565 format)
 * - Geometry (Point, Size, Rect)
 * - Rendering priority levels
 * - Display target identifiers
 *
 * Note: Geometry types are POD (Plain Old Data) for efficient use in unions.
 * Use static factory methods (e.g., Point::make(x, y)) for initialization.
 */

#ifndef GUI_TYPES_H
#define GUI_TYPES_H

#include <cstdint>
#include <cstring>
#include <algorithm>

namespace GUI {

// ============================================================================
// Color Type (RGB565, 16-bit)
// ============================================================================

using Color = uint16_t;

// Color conversion helpers
namespace Colors {
    // Convert RGB888 to RGB565
    constexpr Color fromRGB(uint8_t r, uint8_t g, uint8_t b) {
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }

    // Common colors (RGB565)
    constexpr Color Black       = 0x0000;
    constexpr Color White       = 0xFFFF;
    constexpr Color Red         = 0xF800;
    constexpr Color Green       = 0x07E0;
    constexpr Color Blue        = 0x001F;
    constexpr Color Yellow      = 0xFFE0;
    constexpr Color Cyan        = 0x07FF;
    constexpr Color Magenta     = 0xF81F;
    constexpr Color Orange      = 0xFD20;
    constexpr Color Navy        = 0x000F;
    constexpr Color DarkGreen   = 0x03E0;
    constexpr Color DarkCyan    = 0x03EF;
    constexpr Color Maroon      = 0x7800;
    constexpr Color Purple      = 0x780F;
    constexpr Color Olive       = 0x7BE0;
    constexpr Color LightGrey   = 0xC618;
    constexpr Color DarkGrey    = 0x7BEF;

    // Cardputer theme colors (static fallback defaults)
    // For dynamic theming, use GUI::themeColors() from gui_theme.h
    constexpr Color Background  = Navy;      // 0x000F
    constexpr Color Foreground  = Green;     // 0x07E0
    constexpr Color Highlight   = Yellow;    // 0xFFE0
    constexpr Color Error       = Red;       // 0xF800
    constexpr Color Success     = Green;     // 0x07E0
    constexpr Color Warning     = Yellow;    // 0xFFE0
    constexpr Color Info        = Cyan;      // 0x07FF
    constexpr Color Disabled    = DarkGrey;  // 0x7BEF

    // Extract RGB components from RGB565 color
    inline uint8_t getRed(Color c)   { return (c >> 8) & 0xF8; }
    inline uint8_t getGreen(Color c) { return (c >> 3) & 0xFC; }
    inline uint8_t getBlue(Color c)  { return (c << 3) & 0xF8; }

    // Blend two colors (alpha 0-255, 0=c1, 255=c2)
    inline Color blend(Color c1, Color c2, uint8_t alpha) {
        uint8_t r1 = getRed(c1), g1 = getGreen(c1), b1 = getBlue(c1);
        uint8_t r2 = getRed(c2), g2 = getGreen(c2), b2 = getBlue(c2);
        uint8_t r = r1 + (((r2 - r1) * alpha) >> 8);
        uint8_t g = g1 + (((g2 - g1) * alpha) >> 8);
        uint8_t b = b1 + (((b2 - b1) * alpha) >> 8);
        return fromRGB(r, g, b);
    }

    // Darken color by factor (0-255, 0=black, 255=unchanged)
    inline Color darken(Color c, uint8_t factor) {
        return blend(Black, c, factor);
    }

    // Lighten color by factor (0-255, 0=unchanged, 255=white)
    inline Color lighten(Color c, uint8_t factor) {
        return blend(c, White, factor);
    }
}

// ============================================================================
// Geometry Types (POD for union compatibility)
// ============================================================================

// Point structure (4 bytes) - POD type
struct Point {
    int16_t x;
    int16_t y;

    // Static factory method
    static Point make(int16_t _x, int16_t _y) {
        Point p = {_x, _y};
        return p;
    }

    bool operator==(const Point& other) const {
        return x == other.x && y == other.y;
    }
    bool operator!=(const Point& other) const {
        return !(*this == other);
    }
};

// Size structure (4 bytes) - POD type
struct Size {
    uint16_t width;
    uint16_t height;

    static Size make(uint16_t w, uint16_t h) {
        Size s = {w, h};
        return s;
    }

    bool operator==(const Size& other) const {
        return width == other.width && height == other.height;
    }
    bool operator!=(const Size& other) const {
        return !(*this == other);
    }

    uint32_t area() const {
        return static_cast<uint32_t>(width) * height;
    }
};

// Rect structure (8 bytes) - POD type
struct Rect {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;

    static Rect make(int16_t _x, int16_t _y, uint16_t w, uint16_t h) {
        Rect r = {_x, _y, w, h};
        return r;
    }

    // Right edge (exclusive)
    int16_t right() const { return x + static_cast<int16_t>(width); }

    // Bottom edge (exclusive)
    int16_t bottom() const { return y + static_cast<int16_t>(height); }

    // Check if point coordinates are inside rectangle
    bool contains(int16_t px, int16_t py) const {
        return px >= x && px < right() &&
               py >= y && py < bottom();
    }

    // Check if rectangles overlap
    bool intersects(const Rect& other) const {
        return !(other.x >= right() || other.right() <= x ||
                 other.y >= bottom() || other.bottom() <= y);
    }

    // Get intersection of two rectangles (may have zero size)
    Rect intersection(const Rect& other) const {
        int16_t ix = std::max(x, other.x);
        int16_t iy = std::max(y, other.y);
        int16_t ir = std::min(right(), other.right());
        int16_t ib = std::min(bottom(), other.bottom());

        if (ir <= ix || ib <= iy) {
            return Rect::make(0, 0, 0, 0);  // No intersection
        }
        return Rect::make(ix, iy,
                    static_cast<uint16_t>(ir - ix),
                    static_cast<uint16_t>(ib - iy));
    }

    // Check if rectangle has valid dimensions
    bool isValid() const {
        return width > 0 && height > 0;
    }

    // Check if rectangle is empty
    bool isEmpty() const {
        return width == 0 || height == 0;
    }

    uint32_t area() const {
        return static_cast<uint32_t>(width) * height;
    }

    bool operator==(const Rect& other) const {
        return x == other.x && y == other.y &&
               width == other.width && height == other.height;
    }
    bool operator!=(const Rect& other) const {
        return !(*this == other);
    }
};

// ============================================================================
// Render Priority
// ============================================================================

enum class RenderPriority : uint8_t {
    Urgent   = 0,   // Immediate: input feedback, cursor blink
    Normal   = 1,   // Standard: most UI updates
    Deferred = 2    // Low priority: animations, background updates
};

// ============================================================================
// Display Target
// ============================================================================

enum class DisplayTarget : uint8_t {
    Internal = 0,   // Built-in ST7789V (240x135)
    External = 1,   // Optional I2C/SPI display
    All      = 255  // Both displays
};

// ============================================================================
// Text Alignment
// ============================================================================

enum class HAlign : uint8_t {
    Left   = 0,
    Center = 1,
    Right  = 2
};

enum class VAlign : uint8_t {
    Top    = 0,
    Middle = 1,
    Bottom = 2
};

// ============================================================================
// Font Configuration (POD, 4 bytes)
// ============================================================================

struct FontConfig {
    uint8_t font;       // M5GFX font index (1-8)
    uint8_t sizeInt;    // Size integer part (1-3)
    uint8_t sizeFrac;   // Size fractional part (0-99, /100)
    uint8_t flags;      // Bit 0: bold

    static FontConfig make(uint8_t f = 1, float size = 1.0f, bool bold = false) {
        FontConfig fc;
        fc.font = f;
        fc.sizeInt = static_cast<uint8_t>(size);
        fc.sizeFrac = static_cast<uint8_t>((size - fc.sizeInt) * 100);
        fc.flags = bold ? 1 : 0;
        return fc;
    }

    float getSize() const {
        return sizeInt + sizeFrac / 100.0f;
    }

    bool isBold() const {
        return (flags & 1) != 0;
    }
};

// Verify POD types sizes
static_assert(sizeof(Point) == 4, "Point must be 4 bytes");
static_assert(sizeof(Size) == 4, "Size must be 4 bytes");
static_assert(sizeof(Rect) == 8, "Rect must be 8 bytes");
static_assert(sizeof(FontConfig) == 4, "FontConfig must be 4 bytes");

} // namespace GUI

#endif // GUI_TYPES_H
