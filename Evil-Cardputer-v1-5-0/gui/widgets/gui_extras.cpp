/**
 * @file gui_extras.cpp
 * @brief Additional widgets implementation
 */

#include "gui_extras.h"
#include "gui_draw.h"

namespace GUI {

//=============================================================================
// ProgressBar
//=============================================================================

ProgressBar::ProgressBar(WidgetId id)
    : Widget(WidgetType::ProgressBar, id)
    , m_value(0)
    , m_minValue(0)
    , m_maxValue(100)
    , m_barColor(ProgressColors::Bar)
    , m_trackColor(ProgressColors::Track)
    , m_showPercent(false)
    , m_indeterminate(false)
    , m_animOffset(0)
    , m_lastAnimTime(0)
{
    style().opaque = true;
    style().padding = Insets(1);
}

void ProgressBar::setValue(int value) {
    value = constrain(value, m_minValue, m_maxValue);
    if (m_value != value) {
        int oldValue = m_value;
        m_value = value;
        markDirty();
        emitValueChanged(oldValue, value, nullptr);
    }
}

void ProgressBar::setRange(int minVal, int maxVal) {
    if (minVal > maxVal) {
        int tmp = minVal;
        minVal = maxVal;
        maxVal = tmp;
    }
    m_minValue = minVal;
    m_maxValue = maxVal;
    m_value = constrain(m_value, m_minValue, m_maxValue);
    markDirty();
}

float ProgressBar::normalizedValue() const {
    if (m_maxValue == m_minValue) return 0.0f;
    return (float)(m_value - m_minValue) / (m_maxValue - m_minValue);
}

void ProgressBar::setIndeterminate(bool indeterminate) {
    if (m_indeterminate != indeterminate) {
        m_indeterminate = indeterminate;
        m_animOffset = 0;
        m_lastAnimTime = millis();
        markDirty();
    }
}

void ProgressBar::renderContent() {
    Rect abs = absoluteBounds();
    int16_t barX = abs.x + style().padding.left;
    int16_t barY = abs.y + style().padding.top;
    int16_t barW = abs.width - style().padding.horizontal();
    int16_t barH = abs.height - style().padding.vertical();

    // Draw track
    Draw::fillRect(barX, barY, barW, barH, m_trackColor);

    if (m_indeterminate) {
        // Animated indeterminate bar
        uint32_t now = millis();
        if (now - m_lastAnimTime > 30) {
            m_animOffset = (m_animOffset + 2) % barW;
            m_lastAnimTime = now;
            markDirty();  // Request next frame
        }

        int16_t segmentW = barW / 3;
        int16_t segmentX = barX + m_animOffset - segmentW;

        // Draw animated segment (wraps around)
        if (segmentX < barX) {
            // Split segment
            int16_t rightPart = barX - segmentX;
            Draw::fillRect(barX, barY, segmentW - rightPart, barH, m_barColor);
            Draw::fillRect(barX + barW - rightPart, barY, rightPart, barH, m_barColor);
        } else if (segmentX + segmentW > barX + barW) {
            // Split at right
            int16_t overflow = (segmentX + segmentW) - (barX + barW);
            Draw::fillRect(segmentX, barY, segmentW - overflow, barH, m_barColor);
            Draw::fillRect(barX, barY, overflow, barH, m_barColor);
        } else {
            Draw::fillRect(segmentX, barY, segmentW, barH, m_barColor);
        }
    } else {
        // Normal progress bar
        int16_t fillW = (int16_t)(barW * normalizedValue());
        if (fillW > 0) {
            Draw::fillRect(barX, barY, fillW, barH, m_barColor);
        }
    }

    // Draw percentage text
    if (m_showPercent && !m_indeterminate) {
        char buf[8];
        int percent = (int)(normalizedValue() * 100);
        snprintf(buf, sizeof(buf), "%d%%", percent);

        int16_t textW = strlen(buf) * 6;
        int16_t textX = abs.x + (abs.width - textW) / 2;
        int16_t textY = abs.y + (abs.height - 8) / 2;

        Draw::drawText(textX, textY, buf, ProgressColors::Text, 1);
    }
}

void ProgressBar::measure(int16_t availableWidth, int16_t availableHeight) {
    (void)availableHeight;

    int16_t w = availableWidth > 0 ? availableWidth : 100;
    int16_t h = m_showPercent ? 14 : 8;
    h += style().padding.vertical();

    setMeasuredSize(w, h);
}

//=============================================================================
// StatusIndicator
//=============================================================================

StatusIndicator::StatusIndicator(WidgetId id)
    : Widget(WidgetType::StatusIndicator, id)
    , m_status(IndicatorStatus::Off)
    , m_customColor(0)
    , m_round(true)
    , m_blinkInterval(500)
    , m_lastBlinkTime(0)
    , m_blinkState(true)
{
    style().opaque = false;
}

void StatusIndicator::setStatus(IndicatorStatus status) {
    if (m_status != status) {
        m_status = status;
        m_blinkState = true;
        m_lastBlinkTime = millis();
        markDirty();
    }
}

uint16_t StatusIndicator::getStatusColor() const {
    switch (m_status) {
        case IndicatorStatus::Off:     return IndicatorColors::Off;
        case IndicatorStatus::Ok:      return IndicatorColors::Ok;
        case IndicatorStatus::Warning: return IndicatorColors::Warning;
        case IndicatorStatus::Error:   return IndicatorColors::Error;
        case IndicatorStatus::Info:    return IndicatorColors::Info;
        case IndicatorStatus::Active:  return m_blinkState ? IndicatorColors::Ok : IndicatorColors::Off;
        case IndicatorStatus::Custom:  return m_customColor;
        default:                       return IndicatorColors::Off;
    }
}

void StatusIndicator::renderContent() {
    Rect abs = absoluteBounds();

    // Handle blinking
    if (m_status == IndicatorStatus::Active) {
        uint32_t now = millis();
        if (now - m_lastBlinkTime > m_blinkInterval) {
            m_blinkState = !m_blinkState;
            m_lastBlinkTime = now;
            markDirty();
        }
    }

    uint16_t color = getStatusColor();
    int16_t size = min(abs.width, abs.height);
    int16_t cx = abs.x + abs.width / 2;
    int16_t cy = abs.y + abs.height / 2;

    if (m_round) {
        int16_t radius = size / 2;
        Draw::fillCircle(cx, cy, radius, color);

        // Add highlight effect for "lit" indicators
        if (m_status != IndicatorStatus::Off) {
            // Brighter center
            uint16_t highlight = color | 0x8410;  // Brighten
            Draw::fillCircle(cx - radius/4, cy - radius/4, radius/3, highlight);
        }
    } else {
        int16_t x = abs.x + (abs.width - size) / 2;
        int16_t y = abs.y + (abs.height - size) / 2;
        Draw::fillRect(x, y, size, size, color);
    }
}

void StatusIndicator::measure(int16_t availableWidth, int16_t availableHeight) {
    (void)availableWidth;
    (void)availableHeight;

    // Default size: 8x8
    setMeasuredSize(8, 8);
}

//=============================================================================
// Divider
//=============================================================================

Divider::Divider(bool horizontal, WidgetId id)
    : Widget(WidgetType::Divider, id)
    , m_horizontal(horizontal)
    , m_lineColor(0x4208)  // Dark gray
    , m_thickness(1)
{
    style().opaque = false;
    style().focusable = false;
}

void Divider::renderContent() {
    Rect abs = absoluteBounds();

    if (m_horizontal) {
        int16_t y = abs.y + (abs.height - m_thickness) / 2;
        Draw::fillRect(abs.x, y, abs.width, m_thickness, m_lineColor);
    } else {
        int16_t x = abs.x + (abs.width - m_thickness) / 2;
        Draw::fillRect(x, abs.y, m_thickness, abs.height, m_lineColor);
    }
}

void Divider::measure(int16_t availableWidth, int16_t availableHeight) {
    if (m_horizontal) {
        setMeasuredSize(availableWidth > 0 ? availableWidth : 50, m_thickness + 2);
    } else {
        setMeasuredSize(m_thickness + 2, availableHeight > 0 ? availableHeight : 50);
    }
}

//=============================================================================
// MenuItem
//=============================================================================

MenuItem::MenuItem(const char* text, WidgetId id)
    : Widget(WidgetType::MenuItem, id)
    , m_text(nullptr)
    , m_shortcut(0)
    , m_icon(0)
    , m_hasSubmenu(false)
    , m_separator(false)
    , m_checked(false)
{
    style().focusable = true;
    style().opaque = false;
    style().padding = Insets(1, 4);

    if (text) {
        setText(text);
    }
}

MenuItem::~MenuItem() {
    if (m_text) {
        free(m_text);
    }
}

void MenuItem::setText(const char* text) {
    if (m_text) {
        free(m_text);
        m_text = nullptr;
    }
    if (text) {
        m_text = strdup(text);
    }
    markDirty();
}

bool MenuItem::onKeyPress(char key, uint8_t modifiers) {
    (void)modifiers;

    if (key == '\n' || key == '\r' || key == ' ') {
        if (m_separator) return false;  // Separators can't be activated

        emitClick();
        return true;
    }

    // Check shortcut
    if (m_shortcut && (key == m_shortcut ||
                       key == (m_shortcut - 32) ||
                       key == (m_shortcut + 32))) {
        emitClick();
        return true;
    }

    return Widget::onKeyPress(key, modifiers);
}

void MenuItem::onFocusGained() {
    Widget::onFocusGained();
}

void MenuItem::onFocusLost() {
    Widget::onFocusLost();
}

void MenuItem::renderContent() {
    if (m_separator) {
        // Draw as divider
        Rect abs = absoluteBounds();
        int16_t y = abs.y + abs.height / 2;
        Draw::drawLine(abs.x + 4, y, abs.right() - 4, y,
                      MenuItemColors::Separator);
        return;
    }

    Rect abs = absoluteBounds();
    const int16_t charWidth = 6 * style().textSize;
    const int16_t charHeight = 8 * style().textSize;

    // Background
    if (isFocused()) {
        Draw::fillRect(abs.x, abs.y, abs.width, abs.height,
                      MenuItemColors::Selected);
    }

    uint16_t textColor = isFocused() ? MenuItemColors::TextFocused :
                         (isEnabled() ? MenuItemColors::Text : LabelColors::Muted);

    int16_t x = abs.x + style().padding.left;
    int16_t y = abs.y + (abs.height - charHeight) / 2;

    // Checkmark
    if (m_checked) {
        Draw::drawText(x, y, ">", MenuItemColors::Checkmark, style().textSize);
        x += charWidth + 2;
    }

    // Icon
    if (m_icon) {
        char iconStr[2] = {m_icon, '\0'};
        Draw::drawText(x, y, iconStr, textColor, style().textSize);
        x += charWidth + 2;
    }

    // Text
    if (m_text) {
        Draw::drawText(x, y, m_text, textColor, style().textSize);
    }

    // Shortcut (right-aligned)
    if (m_shortcut) {
        char hint[4];
        snprintf(hint, sizeof(hint), "[%c]", m_shortcut);
        int16_t hintX = abs.right() - style().padding.right - (strlen(hint) * charWidth);
        Draw::drawText(hintX, y, hint, MenuItemColors::Shortcut, style().textSize);
    }

    // Submenu indicator
    if (m_hasSubmenu) {
        int16_t arrowX = abs.right() - style().padding.right - charWidth;
        Draw::drawText(arrowX, y, ">", textColor, style().textSize);
    }
}

void MenuItem::measure(int16_t availableWidth, int16_t availableHeight) {
    (void)availableHeight;

    if (m_separator) {
        setMeasuredSize(availableWidth > 0 ? availableWidth : 50, 5);
        return;
    }

    const int16_t charWidth = 6 * style().textSize;
    const int16_t charHeight = 8 * style().textSize;

    int16_t textWidth = 0;

    // Checkmark space
    if (m_checked) {
        textWidth += charWidth + 2;
    }

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
        textWidth += charWidth * 5;  // "[X]" + space
    }

    // Submenu arrow
    if (m_hasSubmenu) {
        textWidth += charWidth + 4;
    }

    int16_t w = availableWidth > 0 ? availableWidth :
                textWidth + style().padding.horizontal();
    int16_t h = charHeight + style().padding.vertical();

    setMeasuredSize(w, h);
}

} // namespace GUI
