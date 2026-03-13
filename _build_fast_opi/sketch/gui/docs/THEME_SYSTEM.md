#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\docs\\THEME_SYSTEM.md"
# GUI Theme System

Complete theming system for the Cardputer async GUI framework.

## Overview

The Theme System provides centralized theming capabilities for the GUI framework, allowing runtime theme switching while maintaining full compatibility with the stock driver's color scheme.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      ThemeManager                            │
│  (Singleton - manages current theme and overrides)          │
├─────────────────────────────────────────────────────────────┤
│  - currentTheme_: Theme pointer                              │
│  - customTheme_: User-defined theme                          │
│  - overrides_[]: Module-specific color overrides             │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                         Theme                                │
├─────────────────────────────────────────────────────────────┤
│  name: const char*                                           │
│  colors: ThemeColors (28 bytes)                              │
│  fonts: ThemeFonts (16 bytes)                                │
│  spacing: ThemeSpacing (8 bytes)                             │
│  colorful: bool                                              │
└─────────────────────────────────────────────────────────────┘
```

## Stock Driver Color Mapping

The theme system preserves exact compatibility with the original color variables:

| Stock Variable               | Theme Color              | Default Value      |
|------------------------------|--------------------------|---------------------|
| `taskbarBackgroundColor`     | `colors.taskbarBg`       | TFT_NAVY (0x000F)   |
| `taskbarTextColor`           | `colors.taskbarText`     | TFT_GREEN (0x07E0)  |
| `taskbarDividerColor`        | `colors.border`          | TFT_PURPLE (0x780F) |
| `menuBackgroundColor`        | `colors.background`      | TFT_BLACK (0x0000)  |
| `menuSelectedBackgroundColor`| `colors.highlight`       | TFT_NAVY (0x000F)   |
| `menuTextFocusedColor`       | `colors.foreground`      | TFT_GREEN (0x07E0)  |
| `menuTextUnFocusedColor`     | `colors.disabled`        | TFT_WHITE (0xFFFF)  |
| `Colorful`                   | `colorful`               | true                |

## Predefined Themes

### 1. Default Theme (`THEME_DEFAULT`)
The standard Cardputer theme matching the stock driver:
- Navy/Black background
- Green text
- Purple dividers
- Matches original Evil-Cardputer-v1-5-0.ino settings exactly

### 2. Dark Theme (`THEME_DARK`)
Low-light optimized theme:
- Pure black background
- Light grey text
- Cyan accents
- Reduced eye strain

### 3. Light Theme (`THEME_LIGHT`)
High-brightness environments:
- White background
- Black text
- Blue accents
- Good outdoor visibility

### 4. Hacker Theme (`THEME_HACKER`)
Matrix-style terminal look:
- Black background
- Green text (monospace font by default)
- Dark green accents
- Classic hacker aesthetic

### 5. High Contrast Theme (`THEME_HIGH_CONTRAST`)
Maximum visibility for accessibility:
- Black background
- White text
- Yellow accents
- Larger fonts, thicker borders

## Usage

### Basic Theme Switching

```cpp
#include "gui/gui.h"

void setup() {
    M5.begin();
    GUI::begin();

    // Use default theme
    GUI::setTheme(GUI::ThemeId::Default);

    // Or switch to hacker theme
    GUI::setTheme(GUI::ThemeId::Hacker);
}
```

### Accessing Theme Colors

```cpp
// Method 1: Direct access via ThemeManager
GUI::Color bg = GUI::theme().menuBackgroundColor();
GUI::Color text = GUI::theme().menuTextFocusedColor();

// Method 2: Using themeColors() helper
const GUI::ThemeColors& colors = GUI::themeColors();
M5.Display.fillScreen(colors.background);
M5.Display.setTextColor(colors.foreground, colors.background);

// Method 3: Named color access
GUI::Color primary = GUI::theme().getColor("primary");
GUI::Color error = GUI::theme().getColor("error");

// Method 4: Macros (stock driver compatible)
M5.Display.fillScreen(THEME_MENU_BG);
M5.Display.setTextColor(THEME_MENU_TEXT);
```

### Module-Specific Overrides

Modules can temporarily override theme colors:

```cpp
void myModuleInit() {
    // Create custom color palette for this module
    GUI::ThemeColors customColors = GUI::themeColors();
    customColors.background = GUI::TFT::DARKGREEN;
    customColors.foreground = GUI::TFT::WHITE;

    // Push override (active until popped)
    GUI::theme().pushOverride("my_module", customColors);
}

void myModuleCleanup() {
    // Restore previous theme colors
    GUI::theme().popOverride();
}
```

### Custom Theme Creation

```cpp
// Create a completely custom theme
GUI::Theme myTheme = {
    "MyTheme",
    GUI::ThemeColors::make(
        GUI::TFT::NAVY,      // background
        GUI::TFT::CYAN,      // foreground
        GUI::TFT::CYAN,      // primary
        GUI::TFT::BLUE,      // secondary
        GUI::TFT::GREEN,     // success
        GUI::TFT::YELLOW,    // warning
        GUI::TFT::RED,       // error
        GUI::TFT::CYAN,      // info
        GUI::TFT::DARKGREY,  // disabled
        GUI::TFT::BLUE,      // border
        GUI::TFT::BLUE,      // highlight
        GUI::TFT::NAVY,      // surface
        GUI::TFT::NAVY,      // taskbarBg
        GUI::TFT::CYAN       // taskbarText
    ),
    GUI::ThemeFonts::make(
        GUI::FontConfig::make(1, 1.0f, false),  // normal
        GUI::FontConfig::make(1, 1.5f, true),   // header
        GUI::FontConfig::make(4, 1.0f, false),  // mono
        GUI::FontConfig::make(6, 1.0f, false)   // small
    ),
    GUI::ThemeSpacing::make(4, 2, 2, 1, 12, 14, 10, 12),
    true  // colorful
};

// Apply custom theme
GUI::theme().setTheme(myTheme);
```

### Legacy Bridge Integration

When using LegacyBridge, theme colors are automatically applied:

```cpp
#include "gui/legacy/gui_legacy_bridge.h"

void drawWithTheme() {
    // These use current theme colors automatically
    GUI::LegacyBridge::fillScreen(GUI::LegacyBridge::getMenuBackground());
    GUI::LegacyBridge::setTextColor(
        GUI::LegacyBridge::getMenuTextFocused(),
        GUI::LegacyBridge::getMenuBackground()
    );

    // After theme change, sync LegacyBridge state
    GUI::theme().setTheme(GUI::ThemeId::Hacker);
    GUI::LegacyBridge::syncWithTheme();
}
```

## Theme Structure Reference

### ThemeColors (28 bytes)

| Field      | Type   | Description                       |
|------------|--------|-----------------------------------|
| background | Color  | Main background color             |
| foreground | Color  | Main text color                   |
| primary    | Color  | Primary accent color              |
| secondary  | Color  | Secondary accent color            |
| success    | Color  | Success state (green)             |
| warning    | Color  | Warning state (yellow)            |
| error      | Color  | Error state (red)                 |
| info       | Color  | Information (cyan)                |
| disabled   | Color  | Disabled/unfocused elements       |
| border     | Color  | Borders and dividers              |
| highlight  | Color  | Selected/highlighted items        |
| surface    | Color  | Card/dialog surfaces              |
| taskbarBg  | Color  | Taskbar background                |
| taskbarText| Color  | Taskbar text                      |

### ThemeFonts (16 bytes)

| Field   | Type       | Description            |
|---------|------------|------------------------|
| normal  | FontConfig | Default UI text        |
| header  | FontConfig | Headers/titles         |
| mono    | FontConfig | Monospace (code/data)  |
| small   | FontConfig | Small text (status)    |

### ThemeSpacing (8 bytes)

| Field         | Type   | Description              |
|---------------|--------|--------------------------|
| padding       | uint8_t| Inner padding (px)       |
| margin        | uint8_t| Outer margin (px)        |
| borderRadius  | uint8_t| Corner radius (px)       |
| borderWidth   | uint8_t| Border thickness (px)    |
| lineHeight    | uint8_t| Text line height (px)    |
| itemHeight    | uint8_t| List item height (px)    |
| iconSize      | uint8_t| Standard icon size (px)  |
| taskbarHeight | uint8_t| Taskbar height (px)      |

## Color Utilities

### Color Conversion

```cpp
// Create RGB565 color from RGB components
GUI::Color myColor = GUI::Colors::fromRGB(255, 128, 0);  // Orange

// Extract components
uint8_t r = GUI::Colors::getRed(myColor);
uint8_t g = GUI::Colors::getGreen(myColor);
uint8_t b = GUI::Colors::getBlue(myColor);
```

### Color Manipulation

```cpp
// Blend two colors (alpha: 0=c1, 255=c2)
GUI::Color blended = GUI::Colors::blend(
    GUI::TFT::RED,
    GUI::TFT::BLUE,
    128  // 50% blend
);

// Darken color (factor: 0=black, 255=unchanged)
GUI::Color darker = GUI::Colors::darken(GUI::TFT::GREEN, 128);

// Lighten color (factor: 0=unchanged, 255=white)
GUI::Color lighter = GUI::Colors::lighten(GUI::TFT::BLUE, 64);
```

## Persistence

Theme settings can be saved/restored:

```cpp
// Save current theme to buffer
uint8_t buffer[32];
uint8_t size = GUI::theme().serialize(buffer, sizeof(buffer));

// Later, restore from buffer
GUI::theme().deserialize(buffer, size);
```

## Migration from Stock Driver

### Before (stock driver)
```cpp
int menuBackgroundColor = TFT_BLACK;
int menuTextFocusedColor = TFT_GREEN;

M5.Display.fillScreen(menuBackgroundColor);
M5.Display.setTextColor(menuTextFocusedColor);
```

### After (theme system)
```cpp
// Option 1: Direct replacement with macros
M5.Display.fillScreen(THEME_MENU_BG);
M5.Display.setTextColor(THEME_MENU_TEXT);

// Option 2: Using ThemeManager
M5.Display.fillScreen(GUI::theme().menuBackgroundColor());
M5.Display.setTextColor(GUI::theme().menuTextFocusedColor());

// Option 3: Full theme-aware code
const auto& c = GUI::themeColors();
M5.Display.fillScreen(c.background);
M5.Display.setTextColor(c.foreground, c.background);
```

## Performance Considerations

- ThemeManager is a singleton with minimal overhead
- Color lookups are O(1) via direct struct access
- Named color lookups use switch-case for efficiency
- Override stack has fixed size (4 levels max)
- All theme data fits in ~56 bytes per theme
- Predefined themes are in ROM (const)

## Memory Usage

| Component           | Size      |
|---------------------|-----------|
| Theme struct        | ~56 bytes |
| ThemeManager        | ~300 bytes|
| Override stack      | ~120 bytes|
| Predefined themes   | ~280 bytes (ROM) |

Total RAM usage: ~420 bytes
