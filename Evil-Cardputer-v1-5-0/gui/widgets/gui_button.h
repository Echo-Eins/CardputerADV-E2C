/**
 * @file gui_button.h
 * @brief Button widget for clickable actions
 *
 * Supports:
 * - Text buttons with labels
 * - Icon buttons
 * - Toggle buttons (checkbox-like)
 * - Keyboard activation (Enter/Space)
 * - Visual feedback on press
 */

#ifndef GUI_BUTTON_H
#define GUI_BUTTON_H

#include "gui_widget.h"

namespace GUI {

/**
 * @brief Button visual style
 */
enum class ButtonStyle : uint8_t {
    Standard,           // Normal filled button
    Outline,            // Outline only
    Flat,               // No border/background until hover/focus
    Link                // Text link style (underlined)
};

/**
 * @brief Button widget
 */
class Button : public Widget {
public:
    /**
     * @brief Constructor
     * @param text Button label
     * @param id Widget ID
     */
    explicit Button(const char* text = nullptr, WidgetId id = INVALID_WIDGET_ID);

    /**
     * @brief Destructor
     */
    virtual ~Button();

    //=========================================================================
    // Text
    //=========================================================================

    const char* text() const { return m_text; }
    void setText(const char* text);

    //=========================================================================
    // Button Properties
    //=========================================================================

    /**
     * @brief Set button visual style
     */
    void setButtonStyle(ButtonStyle bstyle) {
        m_buttonStyle = bstyle;
        markDirty();
    }

    ButtonStyle buttonStyle() const { return m_buttonStyle; }

    /**
     * @brief Set as toggle button
     */
    void setToggleable(bool toggleable) {
        m_toggleable = toggleable;
    }

    bool isToggleable() const { return m_toggleable; }

    /**
     * @brief Get/set toggled state (for toggle buttons)
     */
    bool isToggled() const { return m_toggled; }
    void setToggled(bool toggled);

    /**
     * @brief Set pressed visual state
     */
    void setPressed(bool pressed);
    bool isPressed() const { return hasState(WidgetState::Pressed); }

    /**
     * @brief Set shortcut key (shown in button)
     */
    void setShortcut(char key) {
        m_shortcut = key;
        markDirty();
    }

    char shortcut() const { return m_shortcut; }

    /**
     * @brief Set icon character (prefix)
     */
    void setIcon(char icon) {
        m_icon = icon;
        markDirty();
    }

    char icon() const { return m_icon; }

    //=========================================================================
    // Colors (convenience)
    //=========================================================================

    void setPressedColor(uint16_t color) { m_pressedColor = color; }
    uint16_t pressedColor() const { return m_pressedColor; }

    void setToggledColor(uint16_t color) { m_toggledColor = color; }
    uint16_t toggledColor() const { return m_toggledColor; }

    //=========================================================================
    // Event Handling
    //=========================================================================

    bool onKeyPress(char key, uint8_t modifiers) override;
    void onFocusGained() override;
    void onFocusLost() override;

    /**
     * @brief Programmatically click the button
     */
    void click();

    //=========================================================================
    // Signals (convenience)
    //=========================================================================

    /**
     * @brief Connect to toggle changed signal
     */
    SlotId onToggleChanged(SlotFunction handler);

    //=========================================================================
    // Rendering
    //=========================================================================

    void renderContent() override;
    void measure(int16_t availableWidth, int16_t availableHeight) override;

protected:
    /**
     * @brief Get current background color based on state
     */
    uint16_t currentBackgroundColor() const;

    /**
     * @brief Get current text color based on state
     */
    uint16_t currentTextColor() const;

private:
    char* m_text;
    ButtonStyle m_buttonStyle;
    bool m_toggleable;
    bool m_toggled;
    char m_shortcut;
    char m_icon;
    uint16_t m_pressedColor;
    uint16_t m_toggledColor;
};

//=============================================================================
// Button Colors
//=============================================================================

namespace ButtonColors {
    // Standard button colors (RGB565)
    constexpr uint16_t DefaultBg     = 0x2104;  // Dark gray
    constexpr uint16_t DefaultFg     = 0xFFFF;  // White
    constexpr uint16_t PressedBg     = 0x4208;  // Lighter gray
    constexpr uint16_t FocusBorder   = 0x07E0;  // Green
    constexpr uint16_t ToggledBg     = 0x001F;  // Blue
    constexpr uint16_t DisabledBg    = 0x1082;  // Very dark gray
    constexpr uint16_t DisabledFg    = 0x7BEF;  // Gray text
}

} // namespace GUI

#endif // GUI_BUTTON_H
