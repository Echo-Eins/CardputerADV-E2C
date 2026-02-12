/**
 * @file gui_extras.h
 * @brief Additional widgets: ProgressBar, StatusIndicator, Divider, MenuItem
 */

#ifndef GUI_EXTRAS_H
#define GUI_EXTRAS_H

#include "gui_widget.h"

namespace GUI {

//=============================================================================
// ProgressBar
//=============================================================================

/**
 * @brief Progress bar widget
 */
class ProgressBar : public Widget {
public:
    explicit ProgressBar(WidgetId id = INVALID_WIDGET_ID);
    virtual ~ProgressBar() = default;

    //=========================================================================
    // Value
    //=========================================================================

    /**
     * @brief Get current value (0-100)
     */
    int value() const { return m_value; }

    /**
     * @brief Set value (0-100)
     */
    void setValue(int value);

    /**
     * @brief Get minimum value
     */
    int minValue() const { return m_minValue; }

    /**
     * @brief Get maximum value
     */
    int maxValue() const { return m_maxValue; }

    /**
     * @brief Set range
     */
    void setRange(int minVal, int maxVal);

    /**
     * @brief Get normalized value (0.0 - 1.0)
     */
    float normalizedValue() const;

    //=========================================================================
    // Appearance
    //=========================================================================

    /**
     * @brief Set progress bar color
     */
    void setBarColor(uint16_t color) {
        m_barColor = color;
        markDirty();
    }

    uint16_t barColor() const { return m_barColor; }

    /**
     * @brief Set track color
     */
    void setTrackColor(uint16_t color) {
        m_trackColor = color;
        markDirty();
    }

    /**
     * @brief Show percentage text
     */
    void setShowPercent(bool show) {
        m_showPercent = show;
        markDirty();
    }

    bool showPercent() const { return m_showPercent; }

    /**
     * @brief Set indeterminate mode (animated)
     */
    void setIndeterminate(bool indeterminate);
    bool isIndeterminate() const { return m_indeterminate; }

    //=========================================================================
    // Rendering
    //=========================================================================

    void renderContent() override;
    void measure(int16_t availableWidth, int16_t availableHeight) override;

private:
    int m_value;
    int m_minValue;
    int m_maxValue;
    uint16_t m_barColor;
    uint16_t m_trackColor;
    bool m_showPercent;
    bool m_indeterminate;
    int16_t m_animOffset;       // For indeterminate animation
    uint32_t m_lastAnimTime;
};

//=============================================================================
// StatusIndicator
//=============================================================================

/**
 * @brief Status indicator (LED-like)
 */
enum class IndicatorStatus : uint8_t {
    Off,                // Not lit
    Ok,                 // Green
    Warning,            // Yellow
    Error,              // Red
    Info,               // Cyan/Blue
    Active,             // Blinking
    Custom              // Custom color
};

class StatusIndicator : public Widget {
public:
    explicit StatusIndicator(WidgetId id = INVALID_WIDGET_ID);
    virtual ~StatusIndicator() = default;

    //=========================================================================
    // Status
    //=========================================================================

    /**
     * @brief Get status
     */
    IndicatorStatus status() const { return m_status; }

    /**
     * @brief Set status
     */
    void setStatus(IndicatorStatus status);

    /**
     * @brief Set to OK (green)
     */
    void setOk() { setStatus(IndicatorStatus::Ok); }

    /**
     * @brief Set to error (red)
     */
    void setError() { setStatus(IndicatorStatus::Error); }

    /**
     * @brief Set to warning (yellow)
     */
    void setWarning() { setStatus(IndicatorStatus::Warning); }

    /**
     * @brief Set to info (cyan)
     */
    void setInfo() { setStatus(IndicatorStatus::Info); }

    /**
     * @brief Set to off
     */
    void setOff() { setStatus(IndicatorStatus::Off); }

    /**
     * @brief Set custom color
     */
    void setCustomColor(uint16_t color) {
        m_customColor = color;
        setStatus(IndicatorStatus::Custom);
    }

    //=========================================================================
    // Appearance
    //=========================================================================

    /**
     * @brief Set indicator shape (circle or square)
     */
    void setRound(bool round) {
        m_round = round;
        markDirty();
    }

    bool isRound() const { return m_round; }

    /**
     * @brief Set blink interval for Active status
     */
    void setBlinkInterval(uint32_t ms) {
        m_blinkInterval = ms;
    }

    //=========================================================================
    // Rendering
    //=========================================================================

    void renderContent() override;
    void measure(int16_t availableWidth, int16_t availableHeight) override;

private:
    IndicatorStatus m_status;
    uint16_t m_customColor;
    bool m_round;
    uint32_t m_blinkInterval;
    uint32_t m_lastBlinkTime;
    bool m_blinkState;

    uint16_t getStatusColor() const;
};

//=============================================================================
// Divider
//=============================================================================

/**
 * @brief Horizontal or vertical divider line
 */
class Divider : public Widget {
public:
    explicit Divider(bool horizontal = true, WidgetId id = INVALID_WIDGET_ID);
    virtual ~Divider() = default;

    //=========================================================================
    // Properties
    //=========================================================================

    void setHorizontal(bool horizontal) {
        m_horizontal = horizontal;
        markDirty();
    }

    bool isHorizontal() const { return m_horizontal; }

    void setLineColor(uint16_t color) {
        m_lineColor = color;
        markDirty();
    }

    uint16_t lineColor() const { return m_lineColor; }

    void setThickness(uint8_t thickness) {
        m_thickness = thickness;
        markDirty();
    }

    uint8_t thickness() const { return m_thickness; }

    //=========================================================================
    // Rendering
    //=========================================================================

    void renderContent() override;
    void measure(int16_t availableWidth, int16_t availableHeight) override;

private:
    bool m_horizontal;
    uint16_t m_lineColor;
    uint8_t m_thickness;
};

//=============================================================================
// MenuItem
//=============================================================================

/**
 * @brief Menu item with optional shortcut and submenu indicator
 */
class MenuItem : public Widget {
public:
    explicit MenuItem(const char* text = nullptr, WidgetId id = INVALID_WIDGET_ID);
    virtual ~MenuItem();

    //=========================================================================
    // Text
    //=========================================================================

    const char* text() const { return m_text; }
    void setText(const char* text);

    //=========================================================================
    // Properties
    //=========================================================================

    /**
     * @brief Set shortcut key
     */
    void setShortcut(char key) {
        m_shortcut = key;
        markDirty();
    }

    char shortcut() const { return m_shortcut; }

    /**
     * @brief Set as having submenu
     */
    void setHasSubmenu(bool has) {
        m_hasSubmenu = has;
        markDirty();
    }

    bool hasSubmenu() const { return m_hasSubmenu; }

    /**
     * @brief Set as separator (divider line instead of item)
     */
    void setSeparator(bool sep) {
        m_separator = sep;
        markDirty();
    }

    bool isSeparator() const { return m_separator; }

    /**
     * @brief Set checked state (for toggle menus)
     */
    void setChecked(bool checked) {
        m_checked = checked;
        markDirty();
    }

    bool isChecked() const { return m_checked; }

    /**
     * @brief Set icon character
     */
    void setIcon(char icon) {
        m_icon = icon;
        markDirty();
    }

    char icon() const { return m_icon; }

    //=========================================================================
    // Event Handling
    //=========================================================================

    bool onKeyPress(char key, uint8_t modifiers) override;
    void onFocusGained() override;
    void onFocusLost() override;

    //=========================================================================
    // Rendering
    //=========================================================================

    void renderContent() override;
    void measure(int16_t availableWidth, int16_t availableHeight) override;

private:
    char* m_text;
    char m_shortcut;
    char m_icon;
    bool m_hasSubmenu;
    bool m_separator;
    bool m_checked;
};

//=============================================================================
// Colors
//=============================================================================

namespace ProgressColors {
    constexpr uint16_t Bar      = 0x07E0;  // Green
    constexpr uint16_t Track    = 0x2104;  // Dark gray
    constexpr uint16_t Text     = 0xFFFF;  // White
}

namespace IndicatorColors {
    constexpr uint16_t Off      = 0x2104;  // Dark gray
    constexpr uint16_t Ok       = 0x07E0;  // Green
    constexpr uint16_t Warning  = 0xFFE0;  // Yellow
    constexpr uint16_t Error    = 0xF800;  // Red
    constexpr uint16_t Info     = 0x07FF;  // Cyan
}

namespace MenuItemColors {
    constexpr uint16_t Background   = 0x0000;
    constexpr uint16_t Selected     = 0x000F;
    constexpr uint16_t Text         = 0xFFFF;
    constexpr uint16_t TextFocused  = 0x07E0;
    constexpr uint16_t Shortcut     = 0x7BEF;
    constexpr uint16_t Separator    = 0x4208;
    constexpr uint16_t Checkmark    = 0x07E0;
}

} // namespace GUI

#endif // GUI_EXTRAS_H
