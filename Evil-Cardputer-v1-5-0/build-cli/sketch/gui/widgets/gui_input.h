#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\widgets\\gui_input.h"
/**
 * @file gui_input.h
 * @brief Text input widget
 *
 * Supports:
 * - Single-line text input
 * - Cursor positioning
 * - Text selection (basic)
 * - Password masking
 * - Placeholder text
 * - Input validation
 * - Character limit
 */

#pragma once

#ifndef E2C_GUI_WIDGET_INPUT_H
#define E2C_GUI_WIDGET_INPUT_H

// Backward-compatible macro for legacy include checks.
#ifndef GUI_INPUT_H
#define GUI_INPUT_H
#endif

#include "gui_widget.h"

namespace GUI {

/**
 * @brief Input type for validation
 */
enum class InputType : uint8_t {
    Text,               // Any text
    Number,             // Numbers only
    Integer,            // Integer (no decimal)
    Password,           // Hidden text
    Alphanumeric,       // Letters and numbers only
    Ip,                 // IP address
    Hex                 // Hexadecimal
};

/**
 * @brief Text input widget
 */
class Input : public Widget {
public:
    /**
     * @brief Constructor
     * @param placeholder Placeholder text
     * @param id Widget ID
     */
    explicit Input(const char* placeholder = nullptr, WidgetId id = INVALID_WIDGET_ID);

    /**
     * @brief Destructor
     */
    virtual ~Input();

    //=========================================================================
    // Text Content
    //=========================================================================

    /**
     * @brief Get current text
     */
    const char* text() const { return m_text ? m_text : ""; }

    /**
     * @brief Set text content
     */
    void setText(const char* text);

    /**
     * @brief Clear text
     */
    void clear();

    /**
     * @brief Get text length
     */
    size_t textLength() const { return m_textLen; }

    /**
     * @brief Check if empty
     */
    bool isEmpty() const { return m_textLen == 0; }

    //=========================================================================
    // Placeholder
    //=========================================================================

    void setPlaceholder(const char* placeholder);
    const char* placeholder() const { return m_placeholder; }

    //=========================================================================
    // Input Properties
    //=========================================================================

    /**
     * @brief Set input type
     */
    void setInputType(InputType type) {
        m_inputType = type;
    }

    InputType inputType() const { return m_inputType; }

    /**
     * @brief Set maximum length
     */
    void setMaxLength(size_t maxLen) {
        m_maxLength = maxLen;
    }

    size_t maxLength() const { return m_maxLength; }

    /**
     * @brief Set read-only mode
     */
    void setReadOnly(bool readOnly) {
        m_readOnly = readOnly;
    }

    bool isReadOnly() const { return m_readOnly; }

    /**
     * @brief Set password mask character
     */
    void setMaskChar(char mask) {
        m_maskChar = mask;
        markDirty();
    }

    char maskChar() const { return m_maskChar; }

    //=========================================================================
    // Cursor
    //=========================================================================

    /**
     * @brief Get cursor position
     */
    size_t cursorPosition() const { return m_cursorPos; }

    /**
     * @brief Set cursor position
     */
    void setCursorPosition(size_t pos);

    /**
     * @brief Move cursor left
     */
    void cursorLeft();

    /**
     * @brief Move cursor right
     */
    void cursorRight();

    /**
     * @brief Move cursor to start
     */
    void cursorHome();

    /**
     * @brief Move cursor to end
     */
    void cursorEnd();

    //=========================================================================
    // Editing
    //=========================================================================

    /**
     * @brief Insert character at cursor
     * @return true if character was inserted
     */
    bool insertChar(char c);

    /**
     * @brief Delete character before cursor (backspace)
     * @return true if character was deleted
     */
    bool deleteCharBefore();

    /**
     * @brief Delete character at cursor (delete)
     * @return true if character was deleted
     */
    bool deleteCharAt();

    /**
     * @brief Check if character is valid for input type
     */
    bool isValidChar(char c) const;

    //=========================================================================
    // Event Handling
    //=========================================================================

    bool onKeyPress(char key, uint8_t modifiers) override;
    bool onKeyHold(char key, uint32_t duration, uint8_t modifiers) override;
    void onFocusGained() override;
    void onFocusLost() override;

    //=========================================================================
    // Signals
    //=========================================================================

    /**
     * @brief Connect to text changed signal
     */
    SlotId onTextChanged(SlotFunction handler);

    /**
     * @brief Connect to submit signal (Enter pressed)
     */
    SlotId onSubmit(SlotFunction handler);

    //=========================================================================
    // Rendering
    //=========================================================================

    void renderContent() override;
    void measure(int16_t availableWidth, int16_t availableHeight) override;

protected:
    /**
     * @brief Ensure capacity for text
     */
    bool ensureCapacity(size_t required);

    /**
     * @brief Get display text (masked for password)
     */
    void getDisplayText(char* buffer, size_t bufferSize) const;

    /**
     * @brief Calculate scroll offset for cursor visibility
     */
    void updateScrollOffset();

    /**
     * @brief Emit text changed signal
     */
    void emitTextChanged();

    /**
     * @brief Emit submit signal
     */
    void emitSubmit();

private:
    // Text content
    char* m_text;
    size_t m_textLen;
    size_t m_textCapacity;

    // Placeholder
    char* m_placeholder;

    // Input properties
    InputType m_inputType;
    size_t m_maxLength;
    bool m_readOnly;
    char m_maskChar;

    // Cursor state
    size_t m_cursorPos;
    int16_t m_scrollOffset;     // Horizontal scroll for long text
    bool m_cursorVisible;       // For cursor blink
    uint32_t m_lastCursorBlink;

    // Key repeat state
    char m_lastKey;
    uint32_t m_keyHoldStart;
};

//=============================================================================
// Input Colors
//=============================================================================

namespace InputColors {
    constexpr uint16_t Background   = 0x0000;  // Black
    constexpr uint16_t Text         = 0xFFFF;  // White
    constexpr uint16_t Placeholder  = 0x7BEF;  // Gray
    constexpr uint16_t Cursor       = 0x07E0;  // Green
    constexpr uint16_t Border       = 0x4208;  // Dark gray
    constexpr uint16_t FocusBorder  = 0x07E0;  // Green
    constexpr uint16_t Selection    = 0x001F;  // Blue
}

} // namespace GUI

#endif // E2C_GUI_WIDGET_INPUT_H
