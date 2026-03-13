#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\widgets\\gui_widget_factory.cpp"
/**
 * @file gui_widget_factory.cpp
 * @brief Widget factory implementation
 */

#include "gui_widget_factory.h"

namespace GUI {

//=============================================================================
// Singleton
//=============================================================================

WidgetFactory& WidgetFactory::instance() {
    static WidgetFactory s_instance;
    return s_instance;
}

WidgetId WidgetFactory::nextWidgetId() {
    static WidgetId s_nextId = 1000;  // Start from 1000 for factory widgets
    return s_nextId++;
}

//=============================================================================
// Helper Methods
//=============================================================================

void WidgetFactory::applyDisplaySettings(Widget* widget) {
    if (!widget) return;

    auto& display = DisplayAdapter::instance();

    // Apply recommended settings based on display
    widget->setTextSize(display.recommendedTextSize());
    widget->setPadding(display.recommendedPadding());
    widget->setVariant(display.recommendedVariant());
}

uint8_t WidgetFactory::recommendedTextSize() const {
    return DisplayAdapter::instance().recommendedTextSize();
}

Insets WidgetFactory::recommendedPadding() const {
    return DisplayAdapter::instance().recommendedPadding();
}

int16_t WidgetFactory::recommendedItemHeight() const {
    return DisplayAdapter::instance().recommendedItemHeight();
}

//=============================================================================
// Basic Widgets
//=============================================================================

Label* WidgetFactory::createLabel(const char* text, const char* name) {
    Label* label = new Label(text, nextWidgetId());
    if (name) label->setName(name);
    applyDisplaySettings(label);
    return label;
}

Label* WidgetFactory::createStatusLabel(const char* text, uint16_t color,
                                        const char* name) {
    Label* label = createLabel(text, name);
    label->setStatusColor(color);
    return label;
}

Label* WidgetFactory::createTitle(const char* text, const char* name) {
    Label* label = createLabel(text, name);

    // Larger text for titles
    uint8_t size = recommendedTextSize();
    if (size < 2) size++;  // At least size 2 for titles
    label->setTextSize(size);
    label->setTextAlign(1);  // Center

    return label;
}

Button* WidgetFactory::createButton(const char* text, SlotFunction onClick,
                                    const char* name) {
    Button* button = new Button(text, nextWidgetId());
    if (name) button->setName(name);
    applyDisplaySettings(button);

    // Set button height
    int16_t height = DisplayAdapter::instance().recommendedButtonHeight();
    button->setMinSize(50, height);

    if (onClick) {
        button->onClick(onClick);
    }

    return button;
}

Button* WidgetFactory::createToggleButton(const char* text, bool initialState,
                                          SlotFunction onToggle, const char* name) {
    Button* button = createButton(text, nullptr, name);
    button->setToggleable(true);
    button->setToggled(initialState);

    if (onToggle) {
        button->onToggleChanged(onToggle);
    }

    return button;
}

Input* WidgetFactory::createInput(const char* placeholder, InputType type,
                                  const char* name) {
    Input* input = new Input(placeholder, nextWidgetId());
    if (name) input->setName(name);
    input->setInputType(type);
    applyDisplaySettings(input);

    // Set input height
    int16_t height = DisplayAdapter::instance().recommendedInputHeight();
    input->setMinSize(100, height);

    return input;
}

Input* WidgetFactory::createPasswordInput(const char* placeholder, const char* name) {
    return createInput(placeholder, InputType::Password, name);
}

Input* WidgetFactory::createNumberInput(const char* placeholder, const char* name) {
    return createInput(placeholder, InputType::Number, name);
}

//=============================================================================
// Container Widgets
//=============================================================================

Container* WidgetFactory::createContainer(LayoutDirection direction, const char* name) {
    Container* container = new Container(nextWidgetId());
    if (name) container->setName(name);
    container->setLayoutDirection(direction);
    container->setSpacing(2);
    return container;
}

Container* WidgetFactory::createHBox(const char* name) {
    return createContainer(LayoutDirection::Horizontal, name);
}

Container* WidgetFactory::createVBox(const char* name) {
    return createContainer(LayoutDirection::Vertical, name);
}

ScrollView* WidgetFactory::createScrollView(const char* name) {
    ScrollView* scroll = new ScrollView(nextWidgetId());
    if (name) scroll->setName(name);
    scroll->setShowScrollbar(true);
    return scroll;
}

ListView* WidgetFactory::createListView(const char* name) {
    ListView* list = new ListView(nextWidgetId());
    if (name) list->setName(name);

    // Apply display settings
    list->setItemHeight(recommendedItemHeight());
    list->setShowScrollbar(true);

    return list;
}

//=============================================================================
// Extra Widgets
//=============================================================================

ProgressBar* WidgetFactory::createProgressBar(int initialValue, bool showPercent,
                                              const char* name) {
    ProgressBar* progress = new ProgressBar(nextWidgetId());
    if (name) progress->setName(name);
    progress->setValue(initialValue);
    progress->setShowPercent(showPercent);
    applyDisplaySettings(progress);
    return progress;
}

ProgressBar* WidgetFactory::createIndeterminateProgress(const char* name) {
    ProgressBar* progress = createProgressBar(0, false, name);
    progress->setIndeterminate(true);
    return progress;
}

StatusIndicator* WidgetFactory::createStatusIndicator(IndicatorStatus status,
                                                       const char* name) {
    StatusIndicator* indicator = new StatusIndicator(nextWidgetId());
    if (name) indicator->setName(name);
    indicator->setStatus(status);
    return indicator;
}

Divider* WidgetFactory::createDivider(bool horizontal, const char* name) {
    Divider* divider = new Divider(horizontal, nextWidgetId());
    if (name) divider->setName(name);
    return divider;
}

MenuItem* WidgetFactory::createMenuItem(const char* text, char shortcut,
                                        SlotFunction onClick, const char* name) {
    MenuItem* item = new MenuItem(text, nextWidgetId());
    if (name) item->setName(name);
    if (shortcut) item->setShortcut(shortcut);

    applyDisplaySettings(item);
    item->setMinSize(0, recommendedItemHeight());

    if (onClick) {
        item->onClick(onClick);
    }

    return item;
}

MenuItem* WidgetFactory::createSeparator(const char* name) {
    MenuItem* sep = new MenuItem(nullptr, nextWidgetId());
    if (name) sep->setName(name);
    sep->setSeparator(true);
    return sep;
}

//=============================================================================
// Composite Widgets
//=============================================================================

Container* WidgetFactory::createLabeledInput(const char* label,
                                             const char* placeholder,
                                             InputType type,
                                             const char* name) {
    Container* row = createHBox(name);
    row->setSpacing(4);

    // Label (30% width)
    Label* lbl = createLabel(label);
    lbl->setPreferredSize(60, -1);
    row->addChild(lbl);

    // Input (70% width)
    Input* input = createInput(placeholder, type);
    input->setName(name ? (String(name) + "_input").c_str() : nullptr);
    row->addChild(input);

    return row;
}

Container* WidgetFactory::createLabeledValue(const char* label, const char* value,
                                             uint16_t valueColor, const char* name) {
    Container* row = createHBox(name);
    row->setSpacing(4);

    // Label
    Label* lbl = createLabel(label);
    lbl->setPreferredSize(60, -1);
    row->addChild(lbl);

    // Value
    Label* val = createLabel(value);
    if (valueColor) {
        val->setStatusColor(valueColor);
    }
    row->addChild(val);

    return row;
}

Container* WidgetFactory::createButtonRow(const char* button1, SlotFunction onClick1,
                                          const char* button2, SlotFunction onClick2,
                                          const char* button3, SlotFunction onClick3) {
    Container* row = createHBox();
    row->setSpacing(8);

    if (button1) {
        row->addChild(createButton(button1, onClick1));
    }
    if (button2) {
        row->addChild(createButton(button2, onClick2));
    }
    if (button3) {
        row->addChild(createButton(button3, onClick3));
    }

    return row;
}

Container* WidgetFactory::createStatusRow(const char* label, IndicatorStatus status,
                                          const char* name) {
    Container* row = createHBox(name);
    row->setSpacing(4);

    // Status indicator
    StatusIndicator* indicator = createStatusIndicator(status);
    row->addChild(indicator);

    // Label
    Label* lbl = createLabel(label);
    row->addChild(lbl);

    return row;
}

//=============================================================================
// Screen Builders
//=============================================================================

Container* WidgetFactory::createScreen(const char* title, const char* name) {
    auto& display = DisplayAdapter::instance();

    Container* screen = createVBox(name);
    screen->setBounds(0, 0, display.width(), display.height());
    screen->setSpacing(0);
    screen->setOpaque(true);
    screen->setBackgroundColor(ThemeManager::instance().theme().menuBackgroundColor());

    // Title bar
    Label* titleLabel = createTitle(title, "title");
    titleLabel->setPreferredSize(-1, 12);
    titleLabel->setBackgroundColor(ThemeManager::instance().theme().taskbarBackgroundColor());
    titleLabel->setOpaque(true);
    screen->addChild(titleLabel);

    // Divider
    screen->addChild(createDivider(true, "divider"));

    // Content area
    Container* content = createVBox("content");
    content->setPadding(Insets(2, 4));
    screen->addChild(content);

    return screen;
}

Container* WidgetFactory::createSettingsScreen(const char* title, const char* name) {
    Container* screen = createScreen(title, name);

    // Replace content with scroll view
    Widget* content = screen->findChild("content");
    if (content) {
        screen->removeChild(content);
        delete content;
    }

    // Add scroll view for settings
    ScrollView* scroll = createScrollView("content");
    scroll->setLayoutDirection(LayoutDirection::Vertical);
    scroll->setSpacing(2);
    scroll->setPadding(Insets(2, 4));
    screen->addChild(scroll);

    return screen;
}

Container* WidgetFactory::createFormScreen(const char* title,
                                           SlotFunction onSubmit,
                                           SlotFunction onCancel,
                                           const char* name) {
    auto& display = DisplayAdapter::instance();

    Container* screen = createVBox(name);
    screen->setBounds(0, 0, display.width(), display.height());
    screen->setSpacing(0);
    screen->setOpaque(true);
    screen->setBackgroundColor(ThemeManager::instance().theme().menuBackgroundColor());

    // Title
    Label* titleLabel = createTitle(title, "title");
    titleLabel->setPreferredSize(-1, 12);
    titleLabel->setOpaque(true);
    screen->addChild(titleLabel);

    // Divider
    screen->addChild(createDivider(true));

    // Form content (scrollable)
    ScrollView* content = createScrollView("content");
    content->setLayoutDirection(LayoutDirection::Vertical);
    content->setSpacing(4);
    content->setPadding(Insets(4));
    screen->addChild(content);

    // Bottom divider
    screen->addChild(createDivider(true));

    // Button row
    Container* buttons = createHBox("buttons");
    buttons->setSpacing(8);
    buttons->setPadding(Insets(2, 4));

    if (onCancel) {
        buttons->addChild(createButton("Cancel", onCancel, "btnCancel"));
    }
    buttons->addChild(createButton("OK", onSubmit, "btnSubmit"));

    screen->addChild(buttons);

    return screen;
}

Container* WidgetFactory::createStatusScreen(const char* title, const char* status,
                                             const char* subtitle, const char* name) {
    auto& display = DisplayAdapter::instance();

    Container* screen = createVBox(name);
    screen->setBounds(0, 0, display.width(), display.height());
    screen->setSpacing(0);
    screen->setOpaque(true);
    screen->setBackgroundColor(ThemeManager::instance().theme().menuBackgroundColor());

    // Centered content
    Container* center = createVBox("center");
    center->setPadding(Insets(20, 10));
    center->setSpacing(8);

    // Title
    Label* titleLabel = createTitle(title, "title");
    center->addChild(titleLabel);

    // Status text (larger)
    Label* statusLabel = createLabel(status, "status");
    statusLabel->setTextSize(recommendedTextSize() + 1);
    statusLabel->setTextAlign(1);  // Center
    center->addChild(statusLabel);

    // Subtitle (if provided)
    if (subtitle) {
        Label* subLabel = createLabel(subtitle, "subtitle");
        subLabel->setTextAlign(1);
        subLabel->setStatusColor(LabelColors::Info);
        center->addChild(subLabel);
    }

    // Indeterminate progress
    ProgressBar* progress = createIndeterminateProgress("progress");
    center->addChild(progress);

    screen->addChild(center);

    // Footer
    Label* footer = createLabel("BACKSPACE to cancel", "footer");
    footer->setTextAlign(1);
    footer->setStatusColor(LabelColors::Muted);
    footer->setPreferredSize(-1, 12);
    screen->addChild(footer);

    return screen;
}

} // namespace GUI
