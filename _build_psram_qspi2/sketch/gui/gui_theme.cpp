#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\gui_theme.cpp"
/*
 * GUI Theme System - Implementation
 *
 * ThemeManager singleton and predefined themes implementation.
 * Color values match stock driver settings from Evil-Cardputer-v1-5-0.ino
 */

#include "gui_theme.h"
#include <cstring>

namespace GUI {

// ============================================================================
// Default Font Configurations
// ============================================================================

// Stock driver typically uses font 1 (Roboto) with size 1.0
static const FontConfig FONT_NORMAL = FontConfig::make(1, 1.0f, false);
// Headers use font 1 with size 1.5 or 2.0
static const FontConfig FONT_HEADER = FontConfig::make(1, 1.5f, true);
// Monospace uses font 4 (Courier)
static const FontConfig FONT_MONO = FontConfig::make(4, 1.0f, false);
// Small text uses font 6 (Tiny)
static const FontConfig FONT_SMALL = FontConfig::make(6, 1.0f, false);

// ============================================================================
// Default Theme Fonts
// ============================================================================

static const ThemeFonts DEFAULT_FONTS = ThemeFonts::make(
    FONT_NORMAL,    // normal
    FONT_HEADER,    // header
    FONT_MONO,      // mono
    FONT_SMALL      // small
);

// ============================================================================
// Default Theme Spacing
// ============================================================================

static const ThemeSpacing DEFAULT_SPACING = ThemeSpacing::make(
    4,      // padding
    2,      // margin
    2,      // borderRadius
    1,      // borderWidth
    12,     // lineHeight
    14,     // itemHeight
    10,     // iconSize
    12      // taskbarHeight
);

// ============================================================================
// Predefined Theme: DEFAULT (Stock Driver)
// ============================================================================
// Matches exactly: Evil-Cardputer-v1-5-0.ino variables
// taskbarBackgroundColor = TFT_NAVY (0x000F)
// taskbarTextColor = TFT_GREEN (0x07E0)
// taskbarDividerColor = TFT_PURPLE (0x780F)
// menuBackgroundColor = TFT_BLACK (0x0000)
// menuSelectedBackgroundColor = TFT_NAVY (0x000F)
// menuTextFocusedColor = TFT_GREEN (0x07E0)
// menuTextUnFocusedColor = TFT_WHITE (0xFFFF)
// Colorful = true

const Theme THEME_DEFAULT = {
    "Default",      // name
    ThemeColors::make(
        TFT::BLACK,         // background (menuBackgroundColor)
        TFT::GREEN,         // foreground (menuTextFocusedColor)
        TFT::GREEN,         // primary
        TFT::CYAN,          // secondary
        TFT::GREEN,         // success
        TFT::YELLOW,        // warning
        TFT::RED,           // error
        TFT::CYAN,          // info
        TFT::WHITE,         // disabled (menuTextUnFocusedColor)
        TFT::PURPLE,        // border (taskbarDividerColor)
        TFT::NAVY,          // highlight (menuSelectedBackgroundColor)
        TFT::NAVY,          // surface
        TFT::NAVY,          // taskbarBg (taskbarBackgroundColor)
        TFT::GREEN          // taskbarText (taskbarTextColor)
    ),
    DEFAULT_FONTS,
    DEFAULT_SPACING,
    true            // colorful
};

// ============================================================================
// Predefined Theme: DARK
// ============================================================================
// Darker variant with less blue tint, easier on the eyes

const Theme THEME_DARK = {
    "Dark",         // name
    ThemeColors::make(
        TFT::BLACK,         // background
        TFT::LIGHTGREY,     // foreground
        TFT::CYAN,          // primary
        TFT::DARKCYAN,      // secondary
        TFT::GREEN,         // success
        TFT::YELLOW,        // warning
        TFT::RED,           // error
        TFT::CYAN,          // info
        TFT::DARKGREY,      // disabled
        TFT::DARKGREY,      // border
        TFT::DARKGREY,      // highlight
        Colors::fromRGB(32, 32, 32),  // surface (dark gray)
        Colors::fromRGB(16, 16, 16),  // taskbarBg
        TFT::LIGHTGREY      // taskbarText
    ),
    DEFAULT_FONTS,
    DEFAULT_SPACING,
    false           // colorful
};

// ============================================================================
// Predefined Theme: LIGHT
// ============================================================================
// Light background for bright environments

const Theme THEME_LIGHT = {
    "Light",        // name
    ThemeColors::make(
        TFT::WHITE,         // background
        TFT::BLACK,         // foreground
        TFT::BLUE,          // primary
        TFT::NAVY,          // secondary
        TFT::DARKGREEN,     // success
        TFT::ORANGE,        // warning
        TFT::RED,           // error
        TFT::BLUE,          // info
        TFT::DARKGREY,      // disabled
        TFT::LIGHTGREY,     // border
        TFT::LIGHTGREY,     // highlight
        Colors::fromRGB(240, 240, 240),  // surface
        TFT::LIGHTGREY,     // taskbarBg
        TFT::BLACK          // taskbarText
    ),
    DEFAULT_FONTS,
    DEFAULT_SPACING,
    false           // colorful
};

// ============================================================================
// Predefined Theme: HACKER
// ============================================================================
// Classic green-on-black terminal style (Matrix look)

const Theme THEME_HACKER = {
    "Hacker",       // name
    ThemeColors::make(
        TFT::BLACK,         // background
        TFT::GREEN,         // foreground
        TFT::GREEN,         // primary
        TFT::DARKGREEN,     // secondary
        TFT::GREEN,         // success
        TFT::GREENYELLOW,   // warning
        Colors::fromRGB(255, 64, 64),  // error (brighter red)
        TFT::GREEN,         // info
        TFT::DARKGREEN,     // disabled
        TFT::DARKGREEN,     // border
        Colors::fromRGB(0, 48, 0),  // highlight (very dark green)
        Colors::fromRGB(0, 24, 0),  // surface
        TFT::BLACK,         // taskbarBg
        TFT::GREEN          // taskbarText
    ),
    ThemeFonts::make(
        FontConfig::make(4, 1.0f, false),   // normal (monospace)
        FontConfig::make(4, 1.5f, true),    // header (monospace bold)
        FontConfig::make(4, 1.0f, false),   // mono
        FontConfig::make(4, 1.0f, false)    // small (monospace)
    ),
    DEFAULT_SPACING,
    false           // colorful
};

// ============================================================================
// Predefined Theme: HIGH CONTRAST
// ============================================================================
// Maximum visibility for accessibility

const Theme THEME_HIGH_CONTRAST = {
    "HighContrast", // name
    ThemeColors::make(
        TFT::BLACK,         // background
        TFT::WHITE,         // foreground
        TFT::YELLOW,        // primary
        TFT::CYAN,          // secondary
        TFT::GREEN,         // success
        TFT::YELLOW,        // warning
        TFT::RED,           // error
        TFT::CYAN,          // info
        TFT::LIGHTGREY,     // disabled
        TFT::WHITE,         // border
        TFT::YELLOW,        // highlight
        Colors::fromRGB(32, 32, 32),  // surface
        TFT::BLACK,         // taskbarBg
        TFT::WHITE          // taskbarText
    ),
    ThemeFonts::make(
        FontConfig::make(1, 1.2f, true),    // normal (slightly larger, bold)
        FontConfig::make(1, 1.8f, true),    // header
        FontConfig::make(4, 1.2f, true),    // mono
        FontConfig::make(1, 1.0f, true)     // small (bold)
    ),
    ThemeSpacing::make(
        6,      // padding (larger)
        4,      // margin (larger)
        0,      // borderRadius (sharp corners)
        2,      // borderWidth (thicker)
        14,     // lineHeight
        16,     // itemHeight
        12,     // iconSize
        14      // taskbarHeight
    ),
    true            // colorful
};

// ============================================================================
// Theme Array for ID lookup
// ============================================================================

static const Theme* const THEME_TABLE[] = {
    &THEME_DEFAULT,
    &THEME_DARK,
    &THEME_LIGHT,
    &THEME_HACKER,
    &THEME_HIGH_CONTRAST
};

constexpr size_t THEME_TABLE_SIZE = sizeof(THEME_TABLE) / sizeof(THEME_TABLE[0]);

// ============================================================================
// ThemeManager Implementation
// ============================================================================

ThemeManager& ThemeManager::instance() {
    static ThemeManager manager;
    return manager;
}

ThemeManager::ThemeManager()
    : currentId_(ThemeId::Default)
    , currentTheme_(&THEME_DEFAULT)
    , overrideStackTop_(0)
{
    // Initialize custom theme with defaults
    customTheme_ = THEME_DEFAULT;
    customTheme_.name = "Custom";

    // Clear override stack
    memset(overrides_, 0, sizeof(overrides_));
}

void ThemeManager::setTheme(ThemeId id) {
    if (id == ThemeId::Custom) {
        currentId_ = ThemeId::Custom;
        currentTheme_ = &customTheme_;
        return;
    }

    size_t index = static_cast<size_t>(id);
    if (index < THEME_TABLE_SIZE) {
        currentId_ = id;
        currentTheme_ = THEME_TABLE[index];
    }
}

void ThemeManager::setTheme(const Theme& theme) {
    // Copy to custom theme
    customTheme_ = theme;
    customTheme_.name = "Custom";  // Always mark as custom
    currentId_ = ThemeId::Custom;
    currentTheme_ = &customTheme_;
}

const Theme& ThemeManager::getThemeById(ThemeId id) {
    if (id == ThemeId::Custom) {
        return instance().customTheme_;
    }

    size_t index = static_cast<size_t>(id);
    if (index < THEME_TABLE_SIZE) {
        return *THEME_TABLE[index];
    }
    return THEME_DEFAULT;
}

void ThemeManager::setCustomTheme(const Theme& theme) {
    customTheme_ = theme;
    customTheme_.name = "Custom";
}

Color ThemeManager::getColor(const char* name, bool disabled) const {
    const ThemeColors& c = effectiveColors();

    // Fast string comparison using switch on first char
    if (name == nullptr || name[0] == '\0') {
        return c.foreground;
    }

    switch (name[0]) {
        case 'b':
            if (strcmp(name, "background") == 0) return c.background;
            if (strcmp(name, "border") == 0) return c.border;
            break;
        case 'd':
            if (strcmp(name, "disabled") == 0) return c.disabled;
            break;
        case 'e':
            if (strcmp(name, "error") == 0) return c.error;
            break;
        case 'f':
            if (strcmp(name, "foreground") == 0) return disabled ? c.disabled : c.foreground;
            break;
        case 'h':
            if (strcmp(name, "highlight") == 0) return c.highlight;
            break;
        case 'i':
            if (strcmp(name, "info") == 0) return c.info;
            break;
        case 'p':
            if (strcmp(name, "primary") == 0) return disabled ? c.disabled : c.primary;
            break;
        case 's':
            if (strcmp(name, "secondary") == 0) return c.secondary;
            if (strcmp(name, "success") == 0) return c.success;
            if (strcmp(name, "surface") == 0) return c.surface;
            break;
        case 't':
            if (strcmp(name, "taskbarBg") == 0) return c.taskbarBg;
            if (strcmp(name, "taskbarText") == 0) return c.taskbarText;
            break;
        case 'w':
            if (strcmp(name, "warning") == 0) return c.warning;
            break;
    }

    // Default fallback
    return disabled ? c.disabled : c.foreground;
}

void ThemeManager::pushOverride(const char* moduleName, const ThemeColors& colors) {
    if (overrideStackTop_ >= MAX_THEME_OVERRIDES) {
        // Stack full, drop oldest
        for (size_t i = 0; i < MAX_THEME_OVERRIDES - 1; i++) {
            overrides_[i] = overrides_[i + 1];
        }
        overrideStackTop_ = MAX_THEME_OVERRIDES - 1;
    }

    overrides_[overrideStackTop_].moduleName = moduleName;
    overrides_[overrideStackTop_].colors = colors;
    overrides_[overrideStackTop_].active = true;
    overrideStackTop_++;
}

void ThemeManager::popOverride() {
    if (overrideStackTop_ > 0) {
        overrideStackTop_--;
        overrides_[overrideStackTop_].active = false;
    }
}

void ThemeManager::clearOverrides() {
    for (size_t i = 0; i < MAX_THEME_OVERRIDES; i++) {
        overrides_[i].active = false;
    }
    overrideStackTop_ = 0;
}

const ThemeColors& ThemeManager::effectiveColors() const {
    // Return top of override stack if any active
    if (overrideStackTop_ > 0 && overrides_[overrideStackTop_ - 1].active) {
        return overrides_[overrideStackTop_ - 1].colors;
    }
    return currentTheme_->colors;
}

// ============================================================================
// Serialization Support
// ============================================================================

uint8_t ThemeManager::serialize(uint8_t* buffer, size_t bufferSize) const {
    // Format: [1 byte themeId] [28 bytes customColors if Custom]
    if (bufferSize < 1) return 0;

    buffer[0] = static_cast<uint8_t>(currentId_);

    if (currentId_ == ThemeId::Custom) {
        if (bufferSize < 1 + sizeof(ThemeColors)) return 1;
        memcpy(buffer + 1, &customTheme_.colors, sizeof(ThemeColors));
        return 1 + sizeof(ThemeColors);
    }

    return 1;
}

bool ThemeManager::deserialize(const uint8_t* buffer, size_t size) {
    if (size < 1) return false;

    ThemeId id = static_cast<ThemeId>(buffer[0]);

    if (id == ThemeId::Custom) {
        if (size < 1 + sizeof(ThemeColors)) return false;
        memcpy(&customTheme_.colors, buffer + 1, sizeof(ThemeColors));
        setTheme(ThemeId::Custom);
    } else if (static_cast<size_t>(id) < THEME_TABLE_SIZE) {
        setTheme(id);
    } else {
        return false;
    }

    return true;
}

} // namespace GUI
