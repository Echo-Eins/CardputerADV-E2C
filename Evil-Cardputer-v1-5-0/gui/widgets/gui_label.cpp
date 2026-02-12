/**
 * @file gui_label.cpp
 * @brief Label widget implementation
 */

#include "gui_label.h"
#include "gui_draw.h"
#include <cstdarg>

namespace GUI {

//=============================================================================
// Constructor / Destructor
//=============================================================================

Label::Label(const char* text, WidgetId id)
    : Widget(WidgetType::Label, id)
    , m_text(nullptr)
    , m_textLen(0)
    , m_textCapacity(0)
    , m_textDirty(true)
    , m_prefix(nullptr)
    , m_suffix(nullptr)
    , m_overflow(TextOverflow::Ellipsis)
    , m_maxLines(1)
    , m_statusColor(0)
    , m_useStatusColor(false)
    , m_scrollOffset(0)
    , m_lastScrollTime(0)
{
    // Labels are not focusable by default
    style().focusable = false;
    style().opaque = false;  // Transparent background by default

    if (text) {
        setText(text);
    }
}

Label::~Label() {
    if (m_text) free(m_text);
    if (m_prefix) free(m_prefix);
    if (m_suffix) free(m_suffix);
}

//=============================================================================
// Text Management
//=============================================================================

bool Label::ensureCapacity(size_t required) {
    if (required <= m_textCapacity) {
        return true;
    }

    // Allocate with some extra space to reduce reallocations
    size_t newCapacity = required + 16;
    char* newText = (char*)realloc(m_text, newCapacity);
    if (!newText) {
        return false;  // Allocation failed
    }

    m_text = newText;
    m_textCapacity = newCapacity;
    return true;
}

void Label::setText(const char* text) {
    if (text == nullptr) {
        if (m_text) {
            m_text[0] = '\0';
            m_textLen = 0;
        }
        m_textDirty = true;
        markDirty();
        return;
    }

    size_t len = strlen(text);

    // Check if text actually changed
    if (m_text && m_textLen == len && strcmp(m_text, text) == 0) {
        return;  // No change
    }

    if (!ensureCapacity(len + 1)) {
        return;  // Allocation failed
    }

    memcpy(m_text, text, len + 1);
    m_textLen = len;
    m_textDirty = true;
    markDirty();

    // Emit value changed signal
    emitValueChanged(0, 0, m_text);
}

void Label::setTextF(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    setText(buffer);
}

void Label::setText_P(const char* text_p) {
    // Copy from PROGMEM
    size_t len = strlen_P(text_p);

    if (!ensureCapacity(len + 1)) {
        return;
    }

    strcpy_P(m_text, text_p);
    m_textLen = len;
    m_textDirty = true;
    markDirty();
}

void Label::setPrefix(const char* prefix) {
    if (m_prefix) {
        free(m_prefix);
        m_prefix = nullptr;
    }
    if (prefix) {
        m_prefix = strdup(prefix);
    }
    markDirty();
}

void Label::setSuffix(const char* suffix) {
    if (m_suffix) {
        free(m_suffix);
        m_suffix = nullptr;
    }
    if (suffix) {
        m_suffix = strdup(suffix);
    }
    markDirty();
}

//=============================================================================
// Status Colors
//=============================================================================

void Label::setStatusOk() {
    setStatusColor(LabelColors::Ok);
}

void Label::setStatusError() {
    setStatusColor(LabelColors::Error);
}

void Label::setStatusWarning() {
    setStatusColor(LabelColors::Warning);
}

void Label::setStatusInfo() {
    setStatusColor(LabelColors::Info);
}

uint16_t Label::effectiveTextColor() const {
    if (m_useStatusColor) {
        return m_statusColor;
    }
    if (hasState(WidgetState::Disabled)) {
        return style().disabledColor != 0 ? style().disabledColor : LabelColors::Muted;
    }
    return effectiveForegroundColor();
}

//=============================================================================
// Text Rendering Helpers
//=============================================================================

int16_t Label::calculateTextWidth(const char* text, uint8_t textSize) const {
    if (!text || !text[0]) return 0;

    // Approximate: 6 pixels per character at size 1
    // This should match the font metrics
    const int16_t charWidth = 6 * textSize;
    return strlen(text) * charWidth;
}

void Label::renderEllipsisText(int16_t x, int16_t y, int16_t maxWidth) {
    if (!m_text || !m_text[0]) return;

    const int16_t charWidth = 6 * style().textSize;
    const int16_t ellipsisWidth = 3 * charWidth;  // "..."

    int16_t totalWidth = calculateTextWidth(m_text, style().textSize);

    if (totalWidth <= maxWidth) {
        // Fits, draw normally
        Draw::drawText(x, y, m_text, effectiveTextColor(), style().textSize);
        return;
    }

    // Need to truncate
    int16_t availWidth = maxWidth - ellipsisWidth;
    int maxChars = availWidth / charWidth;
    if (maxChars < 1) maxChars = 1;

    // Draw truncated text
    char buffer[128];
    size_t copyLen = min((size_t)maxChars, sizeof(buffer) - 4);
    strncpy(buffer, m_text, copyLen);
    buffer[copyLen] = '\0';
    strcat(buffer, "...");

    Draw::drawText(x, y, buffer, effectiveTextColor(), style().textSize);
}

void Label::renderWrappedText(int16_t x, int16_t y, int16_t maxWidth, int16_t maxHeight) {
    if (!m_text || !m_text[0]) return;

    const int16_t charWidth = 6 * style().textSize;
    const int16_t lineHeight = 8 * style().textSize + 2;  // 8px char + spacing
    const int maxCharsPerLine = maxWidth / charWidth;

    if (maxCharsPerLine < 1) return;

    int16_t currentY = y;
    int lineCount = 0;
    const char* ptr = m_text;

    while (*ptr && lineCount < m_maxLines && (currentY + lineHeight) <= (y + maxHeight)) {
        // Find end of this line
        const char* lineStart = ptr;
        const char* lineEnd = ptr;
        const char* lastSpace = nullptr;
        int charCount = 0;

        while (*ptr && charCount < maxCharsPerLine) {
            if (*ptr == ' ') {
                lastSpace = ptr;
            }
            if (*ptr == '\n') {
                lineEnd = ptr;
                ptr++;
                break;
            }
            ptr++;
            charCount++;
            lineEnd = ptr;
        }

        // If we didn't reach end of string and there's a space, break at space
        if (*ptr && *ptr != '\n' && lastSpace && lastSpace > lineStart) {
            lineEnd = lastSpace;
            ptr = lastSpace + 1;  // Skip the space
        }

        // Draw this line
        size_t lineLen = lineEnd - lineStart;
        char lineBuf[128];
        if (lineLen > sizeof(lineBuf) - 1) {
            lineLen = sizeof(lineBuf) - 1;
        }
        strncpy(lineBuf, lineStart, lineLen);
        lineBuf[lineLen] = '\0';

        Draw::drawText(x, currentY, lineBuf, effectiveTextColor(), style().textSize);

        currentY += lineHeight;
        lineCount++;

        // Skip newline if that's why we stopped
        while (*ptr == '\n' || *ptr == '\r') ptr++;
    }
}

void Label::renderText(int16_t x, int16_t y, int16_t maxWidth, int16_t maxHeight) {
    switch (m_overflow) {
        case TextOverflow::Clip:
            // Simple clip - just draw and let clipping handle it
            if (m_text) {
                Draw::drawText(x, y, m_text, effectiveTextColor(), style().textSize);
            }
            break;

        case TextOverflow::Ellipsis:
            renderEllipsisText(x, y, maxWidth);
            break;

        case TextOverflow::Wrap:
            renderWrappedText(x, y, maxWidth, maxHeight);
            break;

        case TextOverflow::Scroll:
            // TODO: Implement auto-scroll for long text
            renderEllipsisText(x, y, maxWidth);
            break;
    }
}

//=============================================================================
// Rendering
//=============================================================================

void Label::renderContent() {
    Rect content = contentRect();
    Rect abs = absoluteBounds();

    int16_t x = abs.x + style().padding.left;
    int16_t y = abs.y + style().padding.top;
    int16_t maxWidth = content.width;
    int16_t maxHeight = content.height;

    const int16_t charWidth = 6 * style().textSize;

    // Calculate total text width for alignment
    int16_t prefixWidth = m_prefix ? calculateTextWidth(m_prefix, style().textSize) : 0;
    int16_t textWidth = m_text ? calculateTextWidth(m_text, style().textSize) : 0;
    int16_t suffixWidth = m_suffix ? calculateTextWidth(m_suffix, style().textSize) : 0;
    int16_t totalWidth = prefixWidth + textWidth + suffixWidth;

    // Apply text alignment
    switch (style().textAlign) {
        case 1:  // Center
            x += (maxWidth - totalWidth) / 2;
            break;
        case 2:  // Right
            x += maxWidth - totalWidth;
            break;
        default:  // Left
            break;
    }

    // Draw prefix (if any)
    if (m_prefix && m_prefix[0]) {
        Draw::drawText(x, y, m_prefix, effectiveTextColor(), style().textSize);
        x += prefixWidth;
    }

    // Draw main text
    if (m_text && m_text[0]) {
        // Adjust maxWidth for remaining space
        int16_t remainingWidth = maxWidth - (x - (abs.x + style().padding.left)) - suffixWidth;
        renderText(x, y, remainingWidth, maxHeight);
        x += textWidth;
    }

    // Draw suffix (if any)
    if (m_suffix && m_suffix[0]) {
        Draw::drawText(x, y, m_suffix, effectiveTextColor(), style().textSize);
    }

    m_textDirty = false;
}

void Label::measure(int16_t availableWidth, int16_t availableHeight) {
    const int16_t charWidth = 6 * style().textSize;
    const int16_t charHeight = 8 * style().textSize;

    int16_t textWidth = 0;
    int16_t textHeight = charHeight;

    // Calculate prefix width
    if (m_prefix) {
        textWidth += calculateTextWidth(m_prefix, style().textSize);
    }

    // Calculate main text width
    if (m_text) {
        if (m_overflow == TextOverflow::Wrap && m_maxLines > 1) {
            // Multi-line: width is constrained, height expands
            textWidth += min(calculateTextWidth(m_text, style().textSize),
                           (int16_t)(availableWidth - style().padding.horizontal()));
            textHeight = charHeight * m_maxLines;
        } else {
            textWidth += calculateTextWidth(m_text, style().textSize);
        }
    }

    // Calculate suffix width
    if (m_suffix) {
        textWidth += calculateTextWidth(m_suffix, style().textSize);
    }

    // Add padding
    int16_t w = textWidth + style().padding.horizontal();
    int16_t h = textHeight + style().padding.vertical();

    // Clamp to constraints
    w = sizeConstraint().clampWidth(w);
    h = sizeConstraint().clampHeight(h);

    setMeasuredSize(w, h);
}

//=============================================================================
// Display Adaptation
//=============================================================================

WidgetVariant Label::selectVariant(int16_t displayWidth, int16_t displayHeight) {
    // Labels adapt by changing text size
    if (displayWidth >= 240) {
        return WidgetVariant::Full;
    } else if (displayWidth >= 128) {
        return WidgetVariant::Compact;
    }
    return WidgetVariant::Minimal;
}

} // namespace GUI
