#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\docs\\GUI_ARCHITECTURE.md"
# Universal Async GUI Driver Architecture

## Vision

A decoupled, asynchronous GUI system that:
- Doesn't block the main loop or network operations
- Supports multiple displays (internal + external)
- Provides theming and styling abstraction
- Allows different views per display
- Is easy to use for new module development

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         APPLICATION LAYER                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐                │
│   │  Module  │  │  Module  │  │  Module  │  │  Module  │                │
│   │  (RDP)   │  │  (Chat)  │  │ (Editor) │  │ (Shell)  │                │
│   └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘                │
│        │             │             │             │                       │
│        │   ┌─────────▼─────────────▼─────────────▼──────────┐           │
│        │   │              GUI::View                          │           │
│        │   │  - Declarative widget tree                      │           │
│        │   │  - Style hints (theme-aware)                    │           │
│        │   │  - Layout constraints                           │           │
│        └───▶  - Event callbacks                              │           │
│            └─────────────────────┬───────────────────────────┘           │
│                                  │                                       │
├──────────────────────────────────┼───────────────────────────────────────┤
│                         GUI CORE │                                       │
├──────────────────────────────────┼───────────────────────────────────────┤
│                                  │                                       │
│   ┌──────────────────────────────▼───────────────────────────┐          │
│   │                    GUI::Compositor                        │          │
│   │  - Scene graph management                                 │          │
│   │  - Dirty region tracking                                  │          │
│   │  - Z-ordering / layering                                  │          │
│   │  - Animation timeline                                     │          │
│   └──────────────────────────────┬───────────────────────────┘          │
│                                  │                                       │
│   ┌──────────────────────────────▼───────────────────────────┐          │
│   │                    GUI::Renderer                          │          │
│   │  - Async render queue (ring buffer)                       │          │
│   │  - Per-display framebuffer                                │          │
│   │  - Delta updates (only changed regions)                   │          │
│   │  - Priority queue (urgent vs deferred)                    │          │
│   └──────────────────────────────┬───────────────────────────┘          │
│                                  │                                       │
│   ┌──────────────────────────────▼───────────────────────────┐          │
│   │                    GUI::Theme                             │          │
│   │  - Color palette                                          │          │
│   │  - Font styles                                            │          │
│   │  - Widget skins                                           │          │
│   │  - Per-module overrides                                   │          │
│   └──────────────────────────────┬───────────────────────────┘          │
│                                  │                                       │
├──────────────────────────────────┼───────────────────────────────────────┤
│                      DRIVER LAYER│                                       │
├──────────────────────────────────┼───────────────────────────────────────┤
│                                  │                                       │
│   ┌──────────────────────────────▼───────────────────────────┐          │
│   │                  GUI::DisplayManager                      │          │
│   │  - Display enumeration                                    │          │
│   │  - Hot-plug detection                                     │          │
│   │  - View-to-display routing                                │          │
│   └────────────┬─────────────────────────────┬───────────────┘          │
│                │                             │                           │
│   ┌────────────▼────────────┐   ┌────────────▼────────────┐             │
│   │   InternalDisplay       │   │   ExternalDisplay       │             │
│   │   (ST7789V via SPI)     │   │   (I2C/SPI/HDMI)        │             │
│   │   240x135, 16-bit       │   │   Variable resolution   │             │
│   │   DMA double-buffer     │   │   Optional              │             │
│   └─────────────────────────┘   └─────────────────────────┘             │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

## Key Components

### 1. GUI::View - Declarative Widget Tree

```cpp
// Example: Module declares its UI without knowing about display details
class ChatView : public GUI::View {
public:
    void build() override {
        root = Column({
            Header("LLM Chat", Style::Title),
            Scrollable(messageList, Style::ChatArea),
            Row({
                TextInput(inputField, Style::Input),
                Button("Send", onSend, Style::Primary)
            })
        });
    }

    void setMessages(const std::vector<Message>& msgs) {
        messageList.update(msgs);
        markDirty();  // Request re-render
    }
};
```

### 2. GUI::Renderer - Async Render Queue

```cpp
// Runs on separate task/core (ESP32 has 2 cores)
class Renderer {
    RingBuffer<RenderCommand> queue;
    TaskHandle_t renderTask;

    void pushCommand(RenderCommand cmd, Priority prio = Normal) {
        queue.push(cmd, prio);
        // Non-blocking, returns immediately
    }

    void renderLoop() {  // Runs on Core 0
        while (running) {
            auto cmd = queue.pop();  // Blocks if empty
            executeRender(cmd);      // Actual SPI transfer
        }
    }
};
```

### 3. Delta Updates (Dirty Regions)

```cpp
// Only update what changed
struct DirtyRegion {
    int16_t x, y, w, h;
};

void Compositor::update() {
    auto regions = collectDirtyRegions();
    for (auto& region : regions) {
        // Render only the changed area
        renderer.pushCommand({
            .type = RenderPartial,
            .region = region,
            .buffer = getRegionBuffer(region)
        });
    }
    clearDirtyFlags();
}
```

### 4. Multi-Display Routing

```cpp
// Different content on different displays
void DisplayManager::route(View* view, DisplayTarget target) {
    switch (target) {
        case DisplayTarget::Internal:
            internalDisplay.setRootView(view);
            break;
        case DisplayTarget::External:
            if (externalDisplay.isConnected()) {
                externalDisplay.setRootView(view);
            }
            break;
        case DisplayTarget::Both:
            // Clone or different views
            break;
    }
}

// Example: RDP shows full desktop on external, controls on internal
rdpModule.setView(controlPanel, DisplayTarget::Internal);
rdpModule.setView(desktopMirror, DisplayTarget::External);
```

### 5. Theming System

```cpp
struct Theme {
    struct Colors {
        uint16_t background;
        uint16_t foreground;
        uint16_t primary;
        uint16_t secondary;
        uint16_t error;
        uint16_t success;
    } colors;

    struct Fonts {
        const GFXfont* normal;
        const GFXfont* bold;
        const GFXfont* mono;
    } fonts;

    // Widget-specific overrides
    std::map<WidgetType, WidgetStyle> widgetStyles;
};

// Modules can request style hints
auto style = theme.getStyle(WidgetType::Button, StyleVariant::Primary);
```

## Migration Strategy

### Phase 1: Compatibility Layer
```cpp
// Wrap existing M5.Display calls
namespace LegacyGUI {
    inline void print(const String& s) {
        GUI::renderer.pushText(s, GUI::theme.colors.foreground);
    }
    inline void fillRect(int x, int y, int w, int h, uint16_t c) {
        GUI::renderer.pushRect({x, y, w, h}, c);
    }
}

// Gradual migration via macro
#define M5_Display LegacyGUI
```

### Phase 2: New Modules Use GUI::View
- All new modules use declarative API
- Old modules continue to work via compatibility layer

### Phase 3: Migrate Critical Modules
- RDP, Chat, FileEditor → native GUI::View
- Main menu → native

### Phase 4: Remove Legacy Layer
- All modules migrated
- Remove compatibility macros

## Performance Considerations

### ESP32 Constraints
- 320KB SRAM, 4MB Flash
- Dual-core (240MHz each)
- SPI to display: ~40MHz max

### Optimizations
1. **DMA transfers** - Use ESP32 DMA for SPI, free CPU
2. **PSRAM framebuffer** - External 8MB PSRAM available
3. **Dirty rectangles** - Only transfer changed pixels
4. **Render on Core 0** - Networking on Core 1
5. **Priority queue** - Urgent updates (RDP frames) first

### Memory Layout
```
┌──────────────────────────────────────┐
│ PSRAM (8MB)                          │
│ ├── Framebuffer 0 (65KB)             │
│ ├── Framebuffer 1 (65KB)             │  Double-buffer
│ ├── External FB (variable)           │
│ └── Render cache                     │
├──────────────────────────────────────┤
│ SRAM (320KB)                         │
│ ├── Render queue (~4KB)              │
│ ├── View tree (~10KB)                │
│ └── Application heap                 │
└──────────────────────────────────────┘
```

## External Display Support

### Supported Interfaces
- **I2C**: SSD1306 (128x64 OLED), SH1106
- **SPI**: ILI9341 (320x240), ST7735 (128x160)
- **GPIO**: Grove port compatible

### Detection
```cpp
void DisplayManager::scanExternalDisplays() {
    // I2C scan for known display addresses
    for (uint8_t addr : {0x3C, 0x3D}) {
        if (i2cProbe(addr)) {
            auto display = createI2CDisplay(addr);
            externalDisplays.push_back(display);
        }
    }
}
```

## API Summary

```cpp
// For module developers:
class MyModule : public GUI::Module {
    GUI::View* createView() override;
    void onInput(GUI::InputEvent event) override;
    void onTick() override;  // Non-UI logic
};

// Simple widgets
auto btn = GUI::Button("Click me", []{ doSomething(); });
auto txt = GUI::Text("Hello", Style::Header);
auto list = GUI::ScrollList(items);
auto input = GUI::TextInput(placeholder);

// Layout
auto layout = GUI::Column({
    GUI::Row({ btn, txt }),
    list,
    input
});

// Display routing
GUI::display.show(layout, Target::Internal);
```

## Implementation Priority

1. **Core Renderer** - Async queue, DMA, double-buffer
2. **Basic Widgets** - Text, Rect, Button, List
3. **Compositor** - Dirty tracking, layering
4. **Theme System** - Colors, fonts
5. **External Display** - I2C/SPI drivers
6. **Legacy Wrapper** - M5.Display compatibility
