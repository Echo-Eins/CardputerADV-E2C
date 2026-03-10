#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\widgets\\gui_label.h"
/**
 * @file gui_label.h
 * @brief Label widget for displaying text
 *
 * Supports:
 * - Static and dynamic text
 * - Multiple text sizes and alignments
 * - Auto-truncation with ellipsis
 * - Color coding (status indicators)
 * - Compact/full variants for adaptive display
 */

#ifndef GUI_LABEL_H
#define GUI_LABEL_H

#include "gui_widget.h"

namespace GUI {

/**
 * @brief Text overflow handling
 */
enum class TextOverflow : uint8_t {
    Clip,               // Clip text at boundary
    Ellipsis,           // Show "..." at end
    Wrap,               // Wrap to next line
    Scroll              // Auto-scroll long text
};

/**
 * @brief Label widget
 */
class Label : public Widget {
public:
    /**
     * @brief Constructor
     * @param text Initial text (can be nullptr)
     * @param id Widget ID (0 = auto)
     */
    explicit Label(const char* text = nullptr, WidgetId id = INVALID_WIDGET_ID);

    /**
     * @brief Destructor
     */
    virtual ~Label();

    //=========================================================================
    // Text
    //=========================================================================

    /**
     * @brief Get current text
     */
    const char* text() const { return m_text; }

    /**
     * @brief Set text (copies the string)
     */
    void setText(const char* text);

    /**
     * @brief Set text with format string
     */
    void setTextF(const char* format, ...);

    /**
     * @brief Set text from PROGMEM
     */
    void setText_P(const char* text_p);

    /**
     * @brief Get text length
     */
    size_t textLength() const { return m_textLen; }

    /**
     * @brief Check if text has been modified
     */
    bool isTextDirty() const { return m_textDirty; }

    //=========================================================================
    // Text Properties
    //=========================================================================

    /**
     * @brief Set text overflow mode
     */
    void setOverflow(TextOverflow overflow) {
        m_overflow = overflow;
        markDirty();
    }

    TextOverflow overflow() const { return m_overflow; }

    /**
     * @brief Set maximum number of lines (for wrap mode)
     */
    void setMaxLines(uint8_t lines) {
        m_maxLines = lines;
        markDirty();
    }

    uint8_t maxLines() const { return m_maxLines; }

    /**
     * @brief Set text color directly (overrides style)
     */
    void setTextColor(uint16_t color) {
        setForegroundColor(color);
    }

    uint16_t textColor() const {
        return effectiveForegroundColor();
    }

    /**
     * @brief Set prefix text (e.g., bullet, icon char)
     */
    void setPrefix(const char* prefix);
    const char* prefix() const { return m_prefix; }

    /**
     * @brief Set suffix text (e.g., unit, status)
     */
    void setSuffix(const char* suffix);
    const char* suffix() const { return m_suffix; }

    //=========================================================================
    // Status Colors (for status indicators)
    //=========================================================================

    /**
     * @brief Set status color (quick color change)
     */
    void setStatusColor(uint16_t color) {
        m_statusColor = color;
        m_useStatusColor = true;
        markDirty();
    }

    /**
     * @brief Clear status color (use normal foreground)
     */
    void clearStatusColor() {
        m_useStatusColor = false;
        markDirty();
    }

    /**
     * @brief Set as OK status (green)
     */
    void setStatusOk();

    /**
     * @brief Set as error status (red)
     */
    void setStatusError();

    /**
     * @brief Set as warning status (yellow)
     */
    void setStatusWarning();

    /**
     * @brief Set as info status (cyan)
     */
    void setStatusInfo();

    //=========================================================================
    // Rendering
    //=========================================================================

    void renderContent() override;
    void measure(int16_t availableWidth, int16_t availableHeight) override;

    //=========================================================================
    // Display Adaptation
    //=========================================================================

    WidgetVariant selectVariant(int16_t displayWidth,
                                int16_t displayHeight) override;

protected:
    /**
     * @brief Get effective text color (considering status)
     */
    uint16_t effectiveTextColor() const;

    /**
     * @brief Calculate text width in pixels
     */
    int16_t calculateTextWidth(const char* text, uint8_t textSize) const;

    /**
     * @brief Render text with current settings
     */
    void renderText(int16_t x, int16_t y, int16_t maxWidth, int16_t maxHeight);

    /**
     * @brief Render ellipsis truncated text
     */
    void renderEllipsisText(int16_t x, int16_t y, int16_t maxWidth);

    /**
     * @brief Render wrapped text
     */
    void renderWrappedText(int16_t x, int16_t y, int16_t maxWidth, int16_t maxHeight);

private:
    // Text content (dynamically allocated)
    char* m_text;
    size_t m_textLen;
    size_t m_textCapacity;
    bool m_textDirty;

    // Prefix/suffix (dynamically allocated)
    char* m_prefix;
    char* m_suffix;

    // Text properties
    TextOverflow m_overflow;
    uint8_t m_maxLines;

    // Status color
    uint16_t m_statusColor;
    bool m_useStatusColor;

    // Scroll state (for TextOverflow::Scroll)
    int16_t m_scrollOffset;
    uint32_t m_lastScrollTime;

    // Helper to ensure capacity
    bool ensureCapacity(size_t required);
};

//=============================================================================
// Common Status Colors (RGB565)
//=============================================================================

namespace LabelColors {
    constexpr uint16_t Ok      = 0x07E0;  // Green
    constexpr uint16_t Error   = 0xF800;  // Red
    constexpr uint16_t Warning = 0xFFE0;  // Yellow
    constexpr uint16_t Info    = 0x07FF;  // Cyan
    constexpr uint16_t Muted   = 0x7BEF;  // Gray
}

} // namespace GUI

#endif // GUI_LABEL_H
