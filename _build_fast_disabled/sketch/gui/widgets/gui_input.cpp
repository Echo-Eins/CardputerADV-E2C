#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\widgets\\gui_input.cpp"
/**
 * @file gui_input.cpp
 * @brief Text input widget implementation
 */

#include "gui_input.h"
#include "gui_draw.h"
#include <cstring>
#include <cstdlib>

namespace GUI {

//=============================================================================
// Constructor / Destructor
//=============================================================================

Input::Input(const char* placeholder, WidgetId id)
    : Widget(WidgetType::Input, id)
    , m_text(nullptr)
    , m_textLen(0)
    , m_textCapacity(0)
    , m_placeholder(nullptr)
    , m_inputType(InputType::Text)
    , m_maxLength(256)
    , m_readOnly(false)
    , m_maskChar('*')
    , m_cursorPos(0)
    , m_scrollOffset(0)
    , m_cursorVisible(true)
    , m_lastCursorBlink(0)
    , m_lastKey(0)
    , m_keyHoldStart(0)
{
    // Inputs are focusable
    style().focusable = true;
    style().opaque = true;
    style().padding = Insets(2, 4);
    style().borderWidth = 1;
    style().backgroundColor = InputColors::Background;
    style().foregroundColor = InputColors::Text;
    style().borderColor = InputColors::Border;

    if (placeholder) {
        setPlaceholder(placeholder);
    }
}

Input::~Input() {
    if (m_text) free(m_text);
    if (m_placeholder) free(m_placeholder);
}

//=============================================================================
// Text Management
//=============================================================================

bool Input::ensureCapacity(size_t required) {
    if (required <= m_textCapacity) {
        return true;
    }

    size_t newCapacity = required + 32;
    char* newText = (char*)realloc(m_text, newCapacity);
    if (!newText) {
        return false;
    }

    m_text = newText;
    m_textCapacity = newCapacity;
    return true;
}

void Input::setText(const char* text) {
    if (text == nullptr) {
        clear();
        return;
    }

    size_t len = strlen(text);
    if (len > m_maxLength) {
        len = m_maxLength;
    }

    if (!ensureCapacity(len + 1)) {
        return;
    }

    memcpy(m_text, text, len);
    m_text[len] = '\0';
    m_textLen = len;

    // Clamp cursor
    if (m_cursorPos > m_textLen) {
        m_cursorPos = m_textLen;
    }

    updateScrollOffset();
    markDirty();
    emitTextChanged();
}

void Input::clear() {
    if (m_text) {
        m_text[0] = '\0';
    }
    m_textLen = 0;
    m_cursorPos = 0;
    m_scrollOffset = 0;
    markDirty();
    emitTextChanged();
}

void Input::setPlaceholder(const char* placeholder) {
    if (m_placeholder) {
        free(m_placeholder);
        m_placeholder = nullptr;
    }
    if (placeholder) {
        m_placeholder = strdup(placeholder);
    }
    markDirty();
}

//=============================================================================
// Cursor Movement
//=============================================================================

void Input::setCursorPosition(size_t pos) {
    if (pos > m_textLen) {
        pos = m_textLen;
    }
    if (m_cursorPos != pos) {
        m_cursorPos = pos;
        m_cursorVisible = true;
        m_lastCursorBlink = millis();
        updateScrollOffset();
        markDirty();
    }
}

void Input::cursorLeft() {
    if (m_cursorPos > 0) {
        setCursorPosition(m_cursorPos - 1);
    }
}

void Input::cursorRight() {
    if (m_cursorPos < m_textLen) {
        setCursorPosition(m_cursorPos + 1);
    }
}

void Input::cursorHome() {
    setCursorPosition(0);
}

void Input::cursorEnd() {
    setCursorPosition(m_textLen);
}

//=============================================================================
// Editing
//=============================================================================

bool Input::isValidChar(char c) const {
    if (c < 32 || c > 126) {
        return false;  // Non-printable
    }

    switch (m_inputType) {
        case InputType::Number:
            return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+';

        case InputType::Integer:
            return (c >= '0' && c <= '9') || c == '-' || c == '+';

        case InputType::Alphanumeric:
            return (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9');

        case InputType::Ip:
            return (c >= '0' && c <= '9') || c == '.';

        case InputType::Hex:
            return (c >= '0' && c <= '9') ||
                   (c >= 'A' && c <= 'F') ||
                   (c >= 'a' && c <= 'f');

        case InputType::Text:
        case InputType::Password:
        default:
            return true;  // Any printable character
    }
}

bool Input::insertChar(char c) {
    if (m_readOnly || !isValidChar(c)) {
        return false;
    }

    if (m_textLen >= m_maxLength) {
        return false;
    }

    if (!ensureCapacity(m_textLen + 2)) {
        return false;
    }

    // Shift characters after cursor
    for (size_t i = m_textLen; i > m_cursorPos; i--) {
        m_text[i] = m_text[i - 1];
    }

    // Insert character
    m_text[m_cursorPos] = c;
    m_textLen++;
    m_text[m_textLen] = '\0';

    // Move cursor
    m_cursorPos++;

    updateScrollOffset();
    markDirty();
    emitTextChanged();
    return true;
}

bool Input::deleteCharBefore() {
    if (m_readOnly || m_cursorPos == 0) {
        return false;
    }

    // Shift characters
    for (size_t i = m_cursorPos - 1; i < m_textLen; i++) {
        m_text[i] = m_text[i + 1];
    }

    m_textLen--;
    m_cursorPos--;

    updateScrollOffset();
    markDirty();
    emitTextChanged();
    return true;
}

bool Input::deleteCharAt() {
    if (m_readOnly || m_cursorPos >= m_textLen) {
        return false;
    }

    // Shift characters
    for (size_t i = m_cursorPos; i < m_textLen; i++) {
        m_text[i] = m_text[i + 1];
    }

    m_textLen--;

    updateScrollOffset();
    markDirty();
    emitTextChanged();
    return true;
}

//=============================================================================
// Event Handling
//=============================================================================

bool Input::onKeyPress(char key, uint8_t modifiers) {
    (void)modifiers;

    // Store key for hold detection
    m_lastKey = key;
    m_keyHoldStart = millis();

    // Special keys (non-printable)
    switch (key) {
        case '\b':  // Backspace
        case 127:   // Delete
            return deleteCharBefore();

        case '\n':  // Enter
        case '\r':
            emitSubmit();
            return true;

        // Navigation (using FN combinations on M5Cardputer)
        // Arrow keys may come as different codes
        case 0x1B:  // ESC (might be arrow key prefix)
            return false;

        default:
            break;
    }

    // Handle left/right if using arrow key codes
    // (M5Cardputer uses FN + ; / FN + . for up/down, FN + , / FN + / for left/right)
    // For simplicity, we'll use < and > as alternative left/right
    if (key == ',') {
        cursorLeft();
        return true;
    }
    if (key == '.') {
        cursorRight();
        return true;
    }

    // Home/End
    if (key == '^') {  // Custom: FN + something for home
        cursorHome();
        return true;
    }
    if (key == '$') {  // Custom: FN + something for end
        cursorEnd();
        return true;
    }

    // Printable characters
    if (key >= 32 && key < 127) {
        return insertChar(key);
    }

    return false;
}

bool Input::onKeyHold(char key, uint32_t duration, uint8_t modifiers) {
    (void)modifiers;

    // Repeat backspace after hold
    if ((key == '\b' || key == 127) && duration > 300) {
        return deleteCharBefore();
    }

    // Repeat character input
    if (key >= 32 && key < 127 && duration > 500) {
        return insertChar(key);
    }

    return false;
}

void Input::onFocusGained() {
    Widget::onFocusGained();
    m_cursorVisible = true;
    m_lastCursorBlink = millis();
}

void Input::onFocusLost() {
    Widget::onFocusLost();
    m_cursorVisible = false;
}

//=============================================================================
// Signals
//=============================================================================

SlotId Input::onTextChanged(SlotFunction handler) {
    return signal().connect(SignalType::TextChanged, handler, this);
}

SlotId Input::onSubmit(SlotFunction handler) {
    return signal().connect(SignalType::Submit, handler, this);
}

void Input::emitTextChanged() {
    Event e(SignalType::TextChanged, SignalPriority::High);
    e.sender = this;
    e.data.value.stringValue = m_text;
    signal().emit(e);
}

void Input::emitSubmit() {
    Event e = Events::submit();
    e.sender = this;
    e.data.value.stringValue = m_text;
    signal().emit(e);
}

//=============================================================================
// Display Helpers
//=============================================================================

void Input::getDisplayText(char* buffer, size_t bufferSize) const {
    if (!m_text || m_textLen == 0) {
        buffer[0] = '\0';
        return;
    }

    if (m_inputType == InputType::Password) {
        // Mask text
        size_t len = min(m_textLen, bufferSize - 1);
        for (size_t i = 0; i < len; i++) {
            buffer[i] = m_maskChar;
        }
        buffer[len] = '\0';
    } else {
        strncpy(buffer, m_text, bufferSize - 1);
        buffer[bufferSize - 1] = '\0';
    }
}

void Input::updateScrollOffset() {
    const int16_t charWidth = 6 * style().textSize;
    Rect content = contentRect();
    int16_t visibleWidth = content.width - charWidth;  // Reserve space for cursor

    // Calculate cursor X position
    int16_t cursorX = m_cursorPos * charWidth;

    // Adjust scroll to keep cursor visible
    if (cursorX - m_scrollOffset > visibleWidth) {
        m_scrollOffset = cursorX - visibleWidth + charWidth;
    } else if (cursorX - m_scrollOffset < 0) {
        m_scrollOffset = cursorX;
    }

    // Clamp scroll
    if (m_scrollOffset < 0) {
        m_scrollOffset = 0;
    }
}

//=============================================================================
// Rendering
//=============================================================================

void Input::renderContent() {
    Rect abs = absoluteBounds();
    const int16_t charWidth = 6 * style().textSize;
    const int16_t charHeight = 8 * style().textSize;

    int16_t textX = abs.x + style().padding.left;
    int16_t textY = abs.y + (abs.height - charHeight) / 2;
    int16_t textAreaWidth = abs.width - style().padding.horizontal();

    // Draw background
    Draw::fillRect(abs.x, abs.y, abs.width, abs.height, style().backgroundColor);

    // Draw border
    uint16_t borderColor = isFocused() ? InputColors::FocusBorder : style().borderColor;
    Draw::drawRect(abs.x, abs.y, abs.width, abs.height, borderColor);

    // Get display text
    char displayText[128];
    getDisplayText(displayText, sizeof(displayText));

    bool hasText = displayText[0] != '\0';

    if (hasText) {
        // Draw text with scroll offset
        int16_t scrolledX = textX - m_scrollOffset;

        // Simple clipping by drawing within bounds
        // In a real implementation, we'd use the clipping rectangle
        Draw::drawText(scrolledX, textY, displayText,
                      style().foregroundColor, style().textSize);
    } else if (m_placeholder && !isFocused()) {
        // Draw placeholder
        Draw::drawText(textX, textY, m_placeholder,
                      InputColors::Placeholder, style().textSize);
    }

    // Draw cursor when focused
    if (isFocused()) {
        // Cursor blink
        uint32_t now = millis();
        if (now - m_lastCursorBlink > 500) {
            m_cursorVisible = !m_cursorVisible;
            m_lastCursorBlink = now;
            markDirty();  // Request redraw for blink
        }

        if (m_cursorVisible) {
            int16_t cursorX = textX + (m_cursorPos * charWidth) - m_scrollOffset;
            // Ensure cursor is within bounds
            if (cursorX >= textX && cursorX < textX + textAreaWidth) {
                Draw::fillRect(cursorX, textY, 2, charHeight, InputColors::Cursor);
            }
        }
    }
}

void Input::measure(int16_t availableWidth, int16_t availableHeight) {
    (void)availableHeight;

    const int16_t charHeight = 8 * style().textSize;

    // Width: use available or minimum
    int16_t w = availableWidth > 0 ? availableWidth : 100;

    // Height: based on text size + padding
    int16_t h = charHeight + style().padding.vertical() + 2;  // +2 for border

    setMeasuredSize(w, h);
}

} // namespace GUI
