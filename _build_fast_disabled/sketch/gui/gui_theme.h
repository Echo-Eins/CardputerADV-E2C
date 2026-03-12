#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\gui_theme.h"
/*
 * GUI Theme System - Centralized theming for the GUI framework
 *
 * Provides a complete theming system for the async GUI framework:
 * - Theme structure with colors, fonts, and spacing
 * - ThemeManager singleton for runtime theme switching
 * - Predefined themes (Default, Dark, Light, Hacker, HighContrast)
 * - Module-specific color overrides
 *
 * Stock driver color mappings (from Evil-Cardputer-v1-5-0.ino):
 * - taskbarBackgroundColor = TFT_NAVY (0x000F)
 * - taskbarTextColor = TFT_GREEN (0x07E0)
 * - taskbarDividerColor = TFT_PURPLE (0x780F)
 * - menuBackgroundColor = TFT_BLACK (0x0000)
 * - menuSelectedBackgroundColor = TFT_NAVY (0x000F)
 * - menuTextFocusedColor = TFT_GREEN (0x07E0)
 * - menuTextUnFocusedColor = TFT_WHITE (0xFFFF)
 */

#ifndef GUI_THEME_H
#define GUI_THEME_H

#include <cstdint>
#include <cstring>
#include "gui_types.h"

namespace GUI {

// ============================================================================
// Theme Color Palette Structure (28 bytes)
// ============================================================================

struct ThemeColors {
    // Primary colors
    Color background;       // Main background (menuBackgroundColor)
    Color foreground;       // Main text (menuTextFocusedColor)
    Color primary;          // Primary accent (selection, buttons)
    Color secondary;        // Secondary accent

    // Semantic colors
    Color success;          // Success/connected states (TFT_GREEN)
    Color warning;          // Warning states (TFT_YELLOW)
    Color error;            // Error states (TFT_RED)
    Color info;             // Information (TFT_CYAN)

    // UI element colors
    Color disabled;         // Disabled/unfocused items (menuTextUnFocusedColor)
    Color border;           // Borders and dividers (taskbarDividerColor)
    Color highlight;        // Highlighted/selected items (menuSelectedBackgroundColor)
    Color surface;          // Surface (cards, dialogs)

    // Taskbar specific (stock driver compatibility)
    Color taskbarBg;        // Taskbar background (taskbarBackgroundColor)
    Color taskbarText;      // Taskbar text (taskbarTextColor)

    // Factory method for POD initialization
    static ThemeColors make(
        Color bg, Color fg, Color prim, Color sec,
        Color succ, Color warn, Color err, Color inf,
        Color dis, Color bord, Color high, Color surf,
        Color tbBg, Color tbText
    ) {
        ThemeColors c;
        c.background = bg;
        c.foreground = fg;
        c.primary = prim;
        c.secondary = sec;
        c.success = succ;
        c.warning = warn;
        c.error = err;
        c.info = inf;
        c.disabled = dis;
        c.border = bord;
        c.highlight = high;
        c.surface = surf;
        c.taskbarBg = tbBg;
        c.taskbarText = tbText;
        return c;
    }
};

// ============================================================================
// Theme Font Configuration Structure (16 bytes)
// ============================================================================

struct ThemeFonts {
    FontConfig normal;      // Default UI text
    FontConfig header;      // Headers/titles
    FontConfig mono;        // Monospace (data, code)
    FontConfig small;       // Small text (status, hints)

    static ThemeFonts make(
        FontConfig norm, FontConfig head,
        FontConfig mon, FontConfig sm
    ) {
        ThemeFonts f;
        f.normal = norm;
        f.header = head;
        f.mono = mon;
        f.small = sm;
        return f;
    }
};

// ============================================================================
// Theme Spacing Configuration Structure (8 bytes)
// ============================================================================

struct ThemeSpacing {
    uint8_t padding;        // Inner padding (px)
    uint8_t margin;         // Outer margin (px)
    uint8_t borderRadius;   // Corner radius (px)
    uint8_t borderWidth;    // Border thickness (px)
    uint8_t lineHeight;     // Text line height (px)
    uint8_t itemHeight;     // List item height (px)
    uint8_t iconSize;       // Standard icon size (px)
    uint8_t taskbarHeight;  // Taskbar height (px)

    static ThemeSpacing make(
        uint8_t pad = 4, uint8_t marg = 2,
        uint8_t rad = 2, uint8_t bw = 1,
        uint8_t lh = 12, uint8_t ih = 14,
        uint8_t icon = 10, uint8_t tb = 12
    ) {
        ThemeSpacing s;
        s.padding = pad;
        s.margin = marg;
        s.borderRadius = rad;
        s.borderWidth = bw;
        s.lineHeight = lh;
        s.itemHeight = ih;
        s.iconSize = icon;
        s.taskbarHeight = tb;
        return s;
    }
};

// ============================================================================
// Complete Theme Structure (~56 bytes)
// ============================================================================

struct Theme {
    const char* name;       // Theme name (pointer to static string)
    ThemeColors colors;     // Color palette
    ThemeFonts fonts;       // Font configuration
    ThemeSpacing spacing;   // Spacing configuration
    bool colorful;          // Enable colorful mode (stock: Colorful variable)
};

// ============================================================================
// Theme ID Enumeration
// ============================================================================

enum class ThemeId : uint8_t {
    Default = 0,        // Stock driver theme (Navy/Green/Black)
    Dark,               // Dark theme (darker colors)
    Light,              // Light theme (light background)
    Hacker,             // Hacker theme (green on black, matrix-style)
    HighContrast,       // High contrast (accessibility)
    Custom,             // User-defined theme

    Count               // Number of built-in themes
};

// ============================================================================
// Predefined Themes (extern declarations)
// ============================================================================

// Default theme - matches stock driver exactly
extern const Theme THEME_DEFAULT;

// Dark theme - darker, less eye strain
extern const Theme THEME_DARK;

// Light theme - light background for bright environments
extern const Theme THEME_LIGHT;

// Hacker theme - classic green on black terminal look
extern const Theme THEME_HACKER;

// High contrast theme - maximum visibility
extern const Theme THEME_HIGH_CONTRAST;

// ============================================================================
// Theme Override for Module-Specific Colors
// ============================================================================

struct ThemeOverride {
    const char* moduleName;     // Module identifier (e.g., "ble_attacks")
    ThemeColors colors;         // Override colors
    bool active;                // Is this override active?
};

// Maximum number of stacked overrides
constexpr size_t MAX_THEME_OVERRIDES = 4;

// ============================================================================
// ThemeManager - Singleton for Theme Management
// ============================================================================

class ThemeManager {
public:
    // Singleton access
    static ThemeManager& instance();

    // Theme selection
    void setTheme(ThemeId id);
    void setTheme(const Theme& theme);
    ThemeId currentThemeId() const { return currentId_; }

    // Current theme access
    const Theme& current() const { return *currentTheme_; }
    const ThemeColors& colors() const { return currentTheme_->colors; }
    const ThemeFonts& fonts() const { return currentTheme_->fonts; }
    const ThemeSpacing& spacing() const { return currentTheme_->spacing; }

    // Backward-compatible shim for legacy call sites:
    // ThemeManager::instance().theme().menuBackgroundColor()
    ThemeManager& theme() { return *this; }
    const ThemeManager& theme() const { return *this; }

    // Stock driver compatibility getters
    Color menuBackgroundColor() const { return effectiveColors().background; }
    Color menuSelectedBackgroundColor() const { return effectiveColors().highlight; }
    Color menuTextFocusedColor() const { return effectiveColors().foreground; }
    Color menuTextUnFocusedColor() const { return effectiveColors().disabled; }
    Color taskbarBackgroundColor() const { return effectiveColors().taskbarBg; }
    Color taskbarTextColor() const { return effectiveColors().taskbarText; }
    Color taskbarDividerColor() const { return effectiveColors().border; }
    bool isColorful() const { return currentTheme_->colorful; }

    // Named color access with fallback
    Color getColor(const char* name, bool disabled = false) const;

    // Module-specific overrides
    void pushOverride(const char* moduleName, const ThemeColors& colors);
    void popOverride();
    void clearOverrides();
    bool hasOverride() const { return overrideStackTop_ > 0; }

    // Get effective colors (with overrides applied)
    const ThemeColors& effectiveColors() const;

    // Custom theme support
    void setCustomTheme(const Theme& theme);
    const Theme& getCustomTheme() const { return customTheme_; }

    // Theme by ID
    static const Theme& getThemeById(ThemeId id);

    // Serialization support (for settings persistence)
    uint8_t serialize(uint8_t* buffer, size_t bufferSize) const;
    bool deserialize(const uint8_t* buffer, size_t size);

private:
    ThemeManager();
    ~ThemeManager() = default;
    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;

    ThemeId currentId_;
    const Theme* currentTheme_;
    Theme customTheme_;

    // Override stack
    ThemeOverride overrides_[MAX_THEME_OVERRIDES];
    uint8_t overrideStackTop_;
};

// ============================================================================
// Convenience Functions
// ============================================================================

// Get current theme manager
inline ThemeManager& theme() { return ThemeManager::instance(); }

// Quick access to current colors
inline const ThemeColors& themeColors() { return theme().effectiveColors(); }

// Quick access to current fonts
inline const ThemeFonts& themeFonts() { return theme().fonts(); }

// Quick access to current spacing
inline const ThemeSpacing& themeSpacing() { return theme().spacing(); }

// ============================================================================
// Color Constants for Stock Driver Compatibility (TFT_* mappings)
// ============================================================================

namespace TFT {
    // Standard TFT_LIB colors (RGB565)
    constexpr Color BLACK       = 0x0000;
    constexpr Color NAVY        = 0x000F;
    constexpr Color DARKGREEN   = 0x03E0;
    constexpr Color DARKCYAN    = 0x03EF;
    constexpr Color MAROON      = 0x7800;
    constexpr Color PURPLE      = 0x780F;
    constexpr Color OLIVE       = 0x7BE0;
    constexpr Color LIGHTGREY   = 0xC618;
    constexpr Color DARKGREY    = 0x7BEF;
    constexpr Color BLUE        = 0x001F;
    constexpr Color GREEN       = 0x07E0;
    constexpr Color CYAN        = 0x07FF;
    constexpr Color RED         = 0xF800;
    constexpr Color MAGENTA     = 0xF81F;
    constexpr Color YELLOW      = 0xFFE0;
    constexpr Color WHITE       = 0xFFFF;
    constexpr Color ORANGE      = 0xFD20;
    constexpr Color GREENYELLOW = 0xAFE5;
    constexpr Color PINK        = 0xFC18;
    constexpr Color BROWN       = 0x79E0;
    constexpr Color GOLD        = 0xFEA0;
    constexpr Color SILVER      = 0xC618;
    constexpr Color SKYBLUE     = 0x867D;
    constexpr Color VIOLET      = 0x915C;
}

// ============================================================================
// Semantic Color Helper Macros (for easy migration from stock driver)
// ============================================================================

// These can be used as drop-in replacements for stock variables
#define THEME_MENU_BG           GUI::theme().menuBackgroundColor()
#define THEME_MENU_SELECTED_BG  GUI::theme().menuSelectedBackgroundColor()
#define THEME_MENU_TEXT         GUI::theme().menuTextFocusedColor()
#define THEME_MENU_TEXT_DIM     GUI::theme().menuTextUnFocusedColor()
#define THEME_TASKBAR_BG        GUI::theme().taskbarBackgroundColor()
#define THEME_TASKBAR_TEXT      GUI::theme().taskbarTextColor()
#define THEME_TASKBAR_DIVIDER   GUI::theme().taskbarDividerColor()

// Semantic colors
#define THEME_SUCCESS           GUI::themeColors().success
#define THEME_WARNING           GUI::themeColors().warning
#define THEME_ERROR             GUI::themeColors().error
#define THEME_INFO              GUI::themeColors().info

} // namespace GUI

#endif // GUI_THEME_H
