#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\widgets\\README.md"
# GUI Widget System

A complete widget-based UI framework for ESP32 Cardputer with Signal/Slot event handling, lazy rendering, and async renderer integration.

## Features

- **Signal/Slot Pattern**: Decoupled event handling for render and input signals
- **Lazy Rendering**: Only redraws widgets when they change (dirty flags)
- **Dynamic Memory**: Efficient memory allocation for widgets
- **Display Adaptation**: Auto-selects widget variants based on screen resolution
- **Async Renderer Integration**: Works with existing Phase 2 rendering system
- **Factory Pattern**: Easy widget creation with consistent styling

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Application                             │
├─────────────────────────────────────────────────────────────┤
│  WidgetFactory     │    Signal/Slot    │   InputAdapter     │
├────────────────────┼───────────────────┼────────────────────┤
│                    │                   │                    │
│    Widgets         │   WidgetManager   │  DisplayAdapter    │
│    (Label,Button,  │   (Focus,Layout,  │  (Resolution,      │
│     Input,List)    │    DirtyTrack)    │   Scaling)         │
│                    │                   │                    │
├─────────────────────────────────────────────────────────────┤
│                   WidgetRenderer                             │
│              (Frame sync, Input routing)                     │
├─────────────────────────────────────────────────────────────┤
│                   GUI::Draw (Async Queue)                    │
├─────────────────────────────────────────────────────────────┤
│                   Renderer Task (Core 0)                     │
└─────────────────────────────────────────────────────────────┘
```

## Quick Start

```cpp
#include "gui/widgets/gui_widgets.h"
#include "gui/widgets/gui_widget_renderer.h"

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);

    // Initialize async renderer
    GUI::begin();

    // Initialize widget system
    GUI::initWidgetSystem();

    // Create a simple screen
    auto* screen = GUI::Factory().createSettingsScreen("My Settings");
    auto* content = static_cast<GUI::ScrollView*>(screen->findChild("content"));

    // Add widgets
    content->addChild(GUI::label("Hello World!"));
    content->addChild(GUI::button("Click Me", [](const GUI::Event&) {
        Serial.println("Button clicked!");
    }));

    // Show screen
    GUI::WIDGETS.setRoot(screen);
}

void loop() {
    M5Cardputer.update();
    GUI::updateWidgetSystem();  // That's it!
}
```

## Widgets

### Label
Static or dynamic text with status colors.

```cpp
auto* label = GUI::Factory().createLabel("Hello");
label->setText("Updated text");    // Only this label redraws!
label->setStatusOk();              // Green color
label->setStatusError();           // Red color
```

### Button
Clickable button with keyboard support.

```cpp
auto* btn = GUI::Factory().createButton("Submit", [](const GUI::Event&) {
    // Handle click
});
btn->setShortcut('s');  // Press 'S' to activate
```

### Input
Text input field with validation.

```cpp
auto* input = GUI::Factory().createInput("Enter IP");
input->setInputType(GUI::InputType::Ip);
input->onSubmit([](const GUI::Event& e) {
    const char* text = e.data.value.stringValue;
});
```

### ListView
Efficient scrollable list.

```cpp
auto* list = GUI::Factory().createListView();
list->addItem("Item 1", "Description");
list->addItem("Item 2", "OK", GUI::LabelColors::Ok);
list->onItemActivated([](const GUI::Event& e) {
    int index = e.data.value.newValue;
});
```

### Container/ScrollView
Layout containers.

```cpp
auto* vbox = GUI::Factory().createVBox();
auto* hbox = GUI::Factory().createHBox();
auto* scroll = GUI::Factory().createScrollView();
```

## Signal/Slot System

Events are decoupled through signals:

```cpp
// Connect to widget signal
button->onClick([](const GUI::Event& e) {
    Serial.println("Clicked!");
});

// Connect to value changes
input->onTextChanged([](const GUI::Event& e) {
    Serial.printf("New text: %s\n", e.data.value.stringValue);
});

// One-shot connection (auto-disconnects after first call)
signal.connectOnce(SignalType::Click, handler);

// Disconnect
SlotId id = signal.connect(...);
signal.disconnect(id);
```

## Lazy Rendering

Widgets only redraw when their state changes:

```cpp
label->setText("New text");  // Marks dirty, will redraw
label->setText("New text");  // Same text, no redraw!

// System automatically tracks dirty regions
// Only changed areas are sent to async renderer
```

## Display Adaptation

Widgets adapt to display size:

```cpp
// Query display info
auto& display = GUI::Display();
int16_t w = display.width();          // 240
int16_t h = display.height();         // 135
float scale = display.scaleFactor();  // 1.0

// Recommended sizes
int itemH = display.recommendedItemHeight();   // 13 for Cardputer
uint8_t textSize = display.recommendedTextSize(); // 1

// Widget variants auto-selected
// Full -> Compact -> Minimal -> Icon based on space
```

## Custom Widgets

Create custom widgets by extending Widget:

```cpp
class MyWidget : public GUI::Widget {
public:
    MyWidget() : Widget(WidgetType::Custom) {
        style().focusable = true;
    }

    void renderContent() override {
        auto abs = absoluteBounds();
        GUI::Draw::fillRect(abs.x, abs.y, abs.width, abs.height,
                           effectiveBackgroundColor());
        // Custom drawing...
    }

    bool onKeyPress(char key, uint8_t modifiers) override {
        if (key == ' ') {
            doSomething();
            return true;  // Consumed
        }
        return false;
    }
};
```

## Builder Pattern

Fluent API for widget creation:

```cpp
auto* label = GUI::LabelBuilder()
    .text("Status")
    .color(GUI::LabelColors::Info)
    .textSize(2)
    .align(1)  // Center
    .build();

auto* button = GUI::ButtonBuilder()
    .text("OK")
    .shortcut('o')
    .onClick([](auto&) { /* ... */ })
    .build();
```

## Files

| File | Description |
|------|-------------|
| `gui_signal.h` | Signal/Slot event system |
| `gui_widget.h/cpp` | Base Widget class |
| `gui_label.h/cpp` | Label widget |
| `gui_button.h/cpp` | Button widget |
| `gui_input.h/cpp` | Text input widget |
| `gui_container.h/cpp` | Container, ScrollView, ListView |
| `gui_extras.h/cpp` | ProgressBar, StatusIndicator, Divider, MenuItem |
| `gui_widget_manager.h/cpp` | Widget lifecycle and focus management |
| `gui_display_adapter.h/cpp` | Display info and adaptation |
| `gui_widget_factory.h/cpp` | Factory for widget creation |
| `gui_widget_renderer.h/cpp` | Async renderer integration |
| `gui_widgets.h` | Main include header |

## Memory Usage

| Component | Memory |
|-----------|--------|
| Signal (base) | ~32 bytes |
| Widget (base) | ~80 bytes |
| Label | ~100 bytes + text |
| Button | ~90 bytes + text |
| Input | ~120 bytes + text |
| ListView | ~100 bytes + items |
| WidgetManager | ~200 bytes |

Total overhead is minimal - most memory goes to actual content.
