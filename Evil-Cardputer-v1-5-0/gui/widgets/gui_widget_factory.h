/**
 * @file gui_widget_factory.h
 * @brief Factory for dynamic widget creation
 *
 * Provides:
 * - Type-safe widget creation with automatic memory management
 * - Pre-configured widgets adapted to display
 * - Common widget combinations (forms, dialogs, lists)
 * - Theme-aware default styling
 */

#ifndef GUI_WIDGET_FACTORY_H
#define GUI_WIDGET_FACTORY_H

#include "gui_widget.h"
#include "gui_label.h"
#include "gui_button.h"
#include "gui_input.h"
#include "gui_container.h"
#include "gui_extras.h"
#include "gui_display_adapter.h"

namespace GUI {

//=============================================================================
// Widget Factory
//=============================================================================

/**
 * @brief Factory for creating widgets
 *
 * All widgets are created with display-adaptive settings and proper memory
 * management. The factory ensures consistent styling and behavior.
 */
class WidgetFactory {
public:
    /**
     * @brief Get singleton instance
     */
    static WidgetFactory& instance();

    // Prevent copying
    WidgetFactory(const WidgetFactory&) = delete;
    WidgetFactory& operator=(const WidgetFactory&) = delete;

    //=========================================================================
    // Basic Widgets
    //=========================================================================

    /**
     * @brief Create a label widget
     */
    Label* createLabel(const char* text = nullptr,
                       const char* name = nullptr);

    /**
     * @brief Create a status label (colored)
     */
    Label* createStatusLabel(const char* text,
                             uint16_t color,
                             const char* name = nullptr);

    /**
     * @brief Create a title label (larger text)
     */
    Label* createTitle(const char* text,
                       const char* name = nullptr);

    /**
     * @brief Create a button widget
     */
    Button* createButton(const char* text,
                         SlotFunction onClick = nullptr,
                         const char* name = nullptr);

    /**
     * @brief Create a toggle button
     */
    Button* createToggleButton(const char* text,
                               bool initialState = false,
                               SlotFunction onToggle = nullptr,
                               const char* name = nullptr);

    /**
     * @brief Create a text input widget
     */
    Input* createInput(const char* placeholder = nullptr,
                       InputType type = InputType::Text,
                       const char* name = nullptr);

    /**
     * @brief Create a password input
     */
    Input* createPasswordInput(const char* placeholder = nullptr,
                               const char* name = nullptr);

    /**
     * @brief Create a numeric input
     */
    Input* createNumberInput(const char* placeholder = nullptr,
                             const char* name = nullptr);

    //=========================================================================
    // Container Widgets
    //=========================================================================

    /**
     * @brief Create a container widget
     */
    Container* createContainer(LayoutDirection direction = LayoutDirection::Vertical,
                               const char* name = nullptr);

    /**
     * @brief Create a horizontal container
     */
    Container* createHBox(const char* name = nullptr);

    /**
     * @brief Create a vertical container
     */
    Container* createVBox(const char* name = nullptr);

    /**
     * @brief Create a scrollable view
     */
    ScrollView* createScrollView(const char* name = nullptr);

    /**
     * @brief Create a list view
     */
    ListView* createListView(const char* name = nullptr);

    //=========================================================================
    // Extra Widgets
    //=========================================================================

    /**
     * @brief Create a progress bar
     */
    ProgressBar* createProgressBar(int initialValue = 0,
                                   bool showPercent = true,
                                   const char* name = nullptr);

    /**
     * @brief Create an indeterminate progress bar
     */
    ProgressBar* createIndeterminateProgress(const char* name = nullptr);

    /**
     * @brief Create a status indicator
     */
    StatusIndicator* createStatusIndicator(IndicatorStatus status = IndicatorStatus::Off,
                                           const char* name = nullptr);

    /**
     * @brief Create a divider
     */
    Divider* createDivider(bool horizontal = true,
                           const char* name = nullptr);

    /**
     * @brief Create a menu item
     */
    MenuItem* createMenuItem(const char* text,
                             char shortcut = 0,
                             SlotFunction onClick = nullptr,
                             const char* name = nullptr);

    /**
     * @brief Create a separator menu item
     */
    MenuItem* createSeparator(const char* name = nullptr);

    //=========================================================================
    // Composite Widgets
    //=========================================================================

    /**
     * @brief Create a labeled input (label + input field)
     */
    Container* createLabeledInput(const char* label,
                                  const char* placeholder = nullptr,
                                  InputType type = InputType::Text,
                                  const char* name = nullptr);

    /**
     * @brief Create a labeled value (label + value label)
     */
    Container* createLabeledValue(const char* label,
                                  const char* value,
                                  uint16_t valueColor = 0,
                                  const char* name = nullptr);

    /**
     * @brief Create a button row (multiple buttons horizontally)
     */
    Container* createButtonRow(const char* button1,
                               SlotFunction onClick1,
                               const char* button2 = nullptr,
                               SlotFunction onClick2 = nullptr,
                               const char* button3 = nullptr,
                               SlotFunction onClick3 = nullptr);

    /**
     * @brief Create a status row (label + status indicator)
     */
    Container* createStatusRow(const char* label,
                               IndicatorStatus status = IndicatorStatus::Off,
                               const char* name = nullptr);

    //=========================================================================
    // Screen Builders
    //=========================================================================

    /**
     * @brief Create a basic screen with title and content area
     */
    Container* createScreen(const char* title,
                            const char* name = nullptr);

    /**
     * @brief Create a settings-style screen
     * Returns the content container for adding items
     */
    Container* createSettingsScreen(const char* title,
                                    const char* name = nullptr);

    /**
     * @brief Create a form screen with submit/cancel buttons
     */
    Container* createFormScreen(const char* title,
                                SlotFunction onSubmit,
                                SlotFunction onCancel = nullptr,
                                const char* name = nullptr);

    /**
     * @brief Create a status display screen (for connection states, etc.)
     */
    Container* createStatusScreen(const char* title,
                                  const char* status,
                                  const char* subtitle = nullptr,
                                  const char* name = nullptr);

    //=========================================================================
    // Helper Methods
    //=========================================================================

    /**
     * @brief Apply display-adaptive settings to widget
     */
    void applyDisplaySettings(Widget* widget);

    /**
     * @brief Get recommended text size for display
     */
    uint8_t recommendedTextSize() const;

    /**
     * @brief Get recommended padding for display
     */
    Insets recommendedPadding() const;

    /**
     * @brief Get recommended item height for display
     */
    int16_t recommendedItemHeight() const;

private:
    WidgetFactory() = default;
    ~WidgetFactory() = default;

    // Internal helpers
    WidgetId nextWidgetId();
};

//=============================================================================
// Convenience Functions
//=============================================================================

/**
 * @brief Get widget factory instance
 */
inline WidgetFactory& Factory() {
    return WidgetFactory::instance();
}

//=============================================================================
// Builder Pattern Support
//=============================================================================

/**
 * @brief Fluent builder for Label
 */
class LabelBuilder {
public:
    LabelBuilder() : m_label(new Label()) {}

    LabelBuilder& text(const char* t) { m_label->setText(t); return *this; }
    LabelBuilder& name(const char* n) { m_label->setName(n); return *this; }
    LabelBuilder& color(uint16_t c) { m_label->setStatusColor(c); return *this; }
    LabelBuilder& textSize(uint8_t s) { m_label->setTextSize(s); return *this; }
    LabelBuilder& align(uint8_t a) { m_label->setTextAlign(a); return *this; }
    LabelBuilder& overflow(TextOverflow o) { m_label->setOverflow(o); return *this; }
    LabelBuilder& maxLines(uint8_t l) { m_label->setMaxLines(l); return *this; }
    LabelBuilder& prefix(const char* p) { m_label->setPrefix(p); return *this; }
    LabelBuilder& suffix(const char* s) { m_label->setSuffix(s); return *this; }

    LabelBuilder& statusOk() { m_label->setStatusOk(); return *this; }
    LabelBuilder& statusError() { m_label->setStatusError(); return *this; }
    LabelBuilder& statusWarning() { m_label->setStatusWarning(); return *this; }
    LabelBuilder& statusInfo() { m_label->setStatusInfo(); return *this; }

    Label* build() { return m_label; }
    operator Label*() { return m_label; }

private:
    Label* m_label;
};

/**
 * @brief Fluent builder for Button
 */
class ButtonBuilder {
public:
    ButtonBuilder() : m_button(new Button()) {}

    ButtonBuilder& text(const char* t) { m_button->setText(t); return *this; }
    ButtonBuilder& name(const char* n) { m_button->setName(n); return *this; }
    ButtonBuilder& shortcut(char k) { m_button->setShortcut(k); return *this; }
    ButtonBuilder& icon(char i) { m_button->setIcon(i); return *this; }
    ButtonBuilder& toggleable(bool t) { m_button->setToggleable(t); return *this; }
    ButtonBuilder& toggled(bool t) { m_button->setToggled(t); return *this; }
    ButtonBuilder& style(ButtonStyle s) { m_button->setButtonStyle(s); return *this; }

    ButtonBuilder& onClick(SlotFunction handler) {
        m_button->onClick(handler);
        return *this;
    }

    ButtonBuilder& onToggle(SlotFunction handler) {
        m_button->onToggleChanged(handler);
        return *this;
    }

    Button* build() { return m_button; }
    operator Button*() { return m_button; }

private:
    Button* m_button;
};

/**
 * @brief Fluent builder for Input
 */
class InputBuilder {
public:
    InputBuilder() : m_input(new Input()) {}

    InputBuilder& placeholder(const char* p) { m_input->setPlaceholder(p); return *this; }
    InputBuilder& name(const char* n) { m_input->setName(n); return *this; }
    InputBuilder& text(const char* t) { m_input->setText(t); return *this; }
    InputBuilder& type(InputType t) { m_input->setInputType(t); return *this; }
    InputBuilder& maxLength(size_t l) { m_input->setMaxLength(l); return *this; }
    InputBuilder& readOnly(bool r) { m_input->setReadOnly(r); return *this; }
    InputBuilder& password() { m_input->setInputType(InputType::Password); return *this; }
    InputBuilder& number() { m_input->setInputType(InputType::Number); return *this; }
    InputBuilder& ip() { m_input->setInputType(InputType::Ip); return *this; }

    InputBuilder& onSubmit(SlotFunction handler) {
        m_input->onSubmit(handler);
        return *this;
    }

    InputBuilder& onTextChanged(SlotFunction handler) {
        m_input->onTextChanged(handler);
        return *this;
    }

    Input* build() { return m_input; }
    operator Input*() { return m_input; }

private:
    Input* m_input;
};

/**
 * @brief Fluent builder for ListView
 */
class ListViewBuilder {
public:
    ListViewBuilder() : m_list(new ListView()) {}

    ListViewBuilder& name(const char* n) { m_list->setName(n); return *this; }
    ListViewBuilder& itemHeight(int16_t h) { m_list->setItemHeight(h); return *this; }
    ListViewBuilder& showScrollbar(bool s) { m_list->setShowScrollbar(s); return *this; }

    ListViewBuilder& addItem(const char* text, const char* sub = nullptr, uint16_t color = 0) {
        m_list->addItem(text, sub, color);
        return *this;
    }

    ListViewBuilder& onSelectionChanged(SlotFunction handler) {
        m_list->onSelectionChanged(handler);
        return *this;
    }

    ListViewBuilder& onItemActivated(SlotFunction handler) {
        m_list->onItemActivated(handler);
        return *this;
    }

    ListView* build() { return m_list; }
    operator ListView*() { return m_list; }

private:
    ListView* m_list;
};

} // namespace GUI

#endif // GUI_WIDGET_FACTORY_H
