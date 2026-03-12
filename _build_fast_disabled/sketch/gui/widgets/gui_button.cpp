#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\widgets\\gui_button.cpp"
/**
 * @file gui_button.cpp
 * @brief Button widget implementation
 */

#include "gui_button.h"
#include "gui_draw.h"
#include <cstring>
#include <cstdlib>

namespace GUI {

//=============================================================================
// Constructor / Destructor
//=============================================================================

Button::Button(const char* text, WidgetId id)
    : Widget(WidgetType::Button, id)
    , m_text(nullptr)
    , m_buttonStyle(ButtonStyle::Standard)
    , m_toggleable(false)
    , m_toggled(false)
    , m_shortcut(0)
    , m_icon(0)
    , m_pressedColor(ButtonColors::PressedBg)
    , m_toggledColor(ButtonColors::ToggledBg)
{
    // Buttons are focusable
    style().focusable = true;
    style().opaque = true;
    style().padding = Insets(2, 4);  // Vertical, horizontal padding
    style().borderRadius = 2;
    style().backgroundColor = ButtonColors::DefaultBg;
    style().foregroundColor = ButtonColors::DefaultFg;

    if (text) {
        setText(text);
    }
}

Button::~Button() {
    if (m_text) {
        free(m_text);
    }
}

//=============================================================================
// Text
//=============================================================================

void Button::setText(const char* text) {
    if (m_text) {
        free(m_text);
        m_text = nullptr;
    }

    if (text) {
        m_text = strdup(text);
    }
    markDirty();
}

//=============================================================================
// Toggle
//=============================================================================

void Button::setToggled(bool toggled) {
    if (!m_toggleable) return;

    if (m_toggled != toggled) {
        m_toggled = toggled;
        markDirty();

        // Emit value changed
        emitValueChanged(!toggled, toggled, nullptr);
    }
}

void Button::setPressed(bool pressed) {
    if (pressed) {
        addState(WidgetState::Pressed);
    } else {
        removeState(WidgetState::Pressed);
    }
}

//=============================================================================
// Event Handling
//=============================================================================

bool Button::onKeyPress(char key, uint8_t modifiers) {
    (void)modifiers;

    // Enter or space activates button
    if (key == '\n' || key == '\r' || key == ' ') {
        click();
        return true;
    }

    // Check shortcut
    if (m_shortcut && (key == m_shortcut || key == (m_shortcut - 32) || key == (m_shortcut + 32))) {
        click();
        return true;
    }

    return false;
}

void Button::onFocusGained() {
    Widget::onFocusGained();
}

void Button::onFocusLost() {
    Widget::onFocusLost();
    // Clear pressed state when losing focus
    setPressed(false);
}

void Button::click() {
    if (!isEnabled()) return;

    // Visual feedback
    setPressed(true);

    // Handle toggle
    if (m_toggleable) {
        setToggled(!m_toggled);
    }

    // Emit click signal
    emitClick();

    // Clear pressed state after short delay (visual feedback)
    // In real implementation, this would use a timer
    setPressed(false);
}

SlotId Button::onToggleChanged(SlotFunction handler) {
    return signal().connect(SignalType::ValueChanged, handler, this);
}

//=============================================================================
// Colors
//=============================================================================

uint16_t Button::currentBackgroundColor() const {
    if (!isEnabled()) {
        return ButtonColors::DisabledBg;
    }

    if (isPressed()) {
        return m_pressedColor;
    }

    if (m_toggleable && m_toggled) {
        return m_toggledColor;
    }

    if (isFocused()) {
        // Slightly lighter when focused
        uint16_t bg = style().backgroundColor;
        // Simple brightness increase
        uint8_t r = ((bg >> 11) & 0x1F);
        uint8_t g = ((bg >> 5) & 0x3F);
        uint8_t b = (bg & 0x1F);
        r = min(31, r + 4);
        g = min(63, g + 8);
        b = min(31, b + 4);
        return (r << 11) | (g << 5) | b;
    }

    return style().backgroundColor;
}

uint16_t Button::currentTextColor() const {
    if (!isEnabled()) {
        return ButtonColors::DisabledFg;
    }
    return style().foregroundColor;
}

//=============================================================================
// Rendering
//=============================================================================

void Button::renderContent() {
    Rect abs = absoluteBounds();
    uint16_t bg = currentBackgroundColor();
    uint16_t fg = currentTextColor();

    // Draw background based on style
    switch (m_buttonStyle) {
        case ButtonStyle::Standard:
            // Filled background
            if (style().borderRadius > 0) {
                Draw::fillRoundRect(abs.x, abs.y, abs.width, abs.height,
                                   style().borderRadius, bg);
            } else {
                Draw::fillRect(abs.x, abs.y, abs.width, abs.height, bg);
            }
            break;

        case ButtonStyle::Outline:
            // Just border
            if (style().borderRadius > 0) {
                Draw::drawRoundRect(abs.x, abs.y, abs.width, abs.height,
                                   style().borderRadius, fg);
            } else {
                Draw::drawRect(abs.x, abs.y, abs.width, abs.height, fg);
            }
            break;

        case ButtonStyle::Flat:
            // Only show background when focused/pressed
            if (isFocused() || isPressed()) {
                Draw::fillRect(abs.x, abs.y, abs.width, abs.height, bg);
            }
            break;

        case ButtonStyle::Link:
            // No background
            break;
    }

    // Calculate text position
    const int16_t charWidth = 6 * style().textSize;
    const int16_t charHeight = 8 * style().textSize;

    int16_t textWidth = 0;

    // Icon
    if (m_icon) {
        textWidth += charWidth + 2;  // Icon + space
    }

    // Main text
    if (m_text) {
        textWidth += strlen(m_text) * charWidth;
    }

    // Shortcut hint
    int16_t shortcutWidth = 0;
    if (m_shortcut) {
        shortcutWidth = (charWidth * 3);  // "[X]"
        textWidth += shortcutWidth + charWidth;  // + space
    }

    // Center text
    int16_t x = abs.x + (abs.width - textWidth) / 2;
    int16_t y = abs.y + (abs.height - charHeight) / 2;

    // Draw icon
    if (m_icon) {
        char iconStr[2] = {m_icon, '\0'};
        Draw::drawText(x, y, iconStr, fg, style().textSize);
        x += charWidth + 2;
    }

    // Draw text
    if (m_text) {
        Draw::drawText(x, y, m_text, fg, style().textSize);
        x += strlen(m_text) * charWidth;
    }

    // Draw shortcut hint (smaller, dimmed)
    if (m_shortcut) {
        char hint[4];
        snprintf(hint, sizeof(hint), "[%c]", m_shortcut);
        uint16_t hintColor = isFocused() ? fg :
            ((fg >> 1) & 0x7BEF);  // Dimmed
        x += charWidth;  // Space
        Draw::drawText(x, y, hint, hintColor, style().textSize);
    }

    // Draw underline for link style
    if (m_buttonStyle == ButtonStyle::Link && m_text) {
        int16_t lineY = abs.y + abs.height - style().padding.bottom - 1;
        int16_t lineX = abs.x + style().padding.left;
        int16_t lineW = strlen(m_text) * charWidth;
        Draw::drawLine(lineX, lineY, lineX + lineW, lineY, fg);
    }

    // Draw focus indicator
    if (isFocused() && m_buttonStyle != ButtonStyle::Flat) {
        uint16_t focusColor = ButtonColors::FocusBorder;
        if (style().borderRadius > 0) {
            Draw::drawRoundRect(abs.x, abs.y, abs.width, abs.height,
                               style().borderRadius, focusColor);
        } else {
            Draw::drawRect(abs.x, abs.y, abs.width, abs.height, focusColor);
        }
    }
}

void Button::measure(int16_t availableWidth, int16_t availableHeight) {
    (void)availableWidth;
    (void)availableHeight;

    const int16_t charWidth = 6 * style().textSize;
    const int16_t charHeight = 8 * style().textSize;

    int16_t textWidth = 0;

    // Icon
    if (m_icon) {
        textWidth += charWidth + 2;
    }

    // Text
    if (m_text) {
        textWidth += strlen(m_text) * charWidth;
    }

    // Shortcut
    if (m_shortcut) {
        textWidth += charWidth * 4;  // Space + "[X]"
    }

    // Add padding
    int16_t w = textWidth + style().padding.horizontal();
    int16_t h = charHeight + style().padding.vertical();

    // Minimum size
    w = max(w, (int16_t)20);
    h = max(h, (int16_t)12);

    setMeasuredSize(w, h);
}

} // namespace GUI
