# Legacy Bridge Migration Guide

## Overview

The Legacy Bridge provides a compatibility layer that allows gradual migration from direct `M5.Display` calls to the new async `RenderQueue` system. It can be easily disabled once migration is complete.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     Application Code                            │
├──────────────────────┬──────────────────────┬───────────────────┤
│   Legacy Code        │    Migrating Code    │    New Code       │
│   M5.Display.*()     │  LegacyBridge::*()   │  GUI::Draw::*()   │
├──────────────────────┴──────────────────────┴───────────────────┤
│                     Legacy Bridge Layer                         │
│            (Routes calls based on GUI_LEGACY_BRIDGE_MODE)       │
├─────────────────────────────────────────────────────────────────┤
│                        RenderQueue                              │
│              (Lock-free SPSC ring buffer)                       │
├─────────────────────────────────────────────────────────────────┤
│                    Renderer Task (Core 0)                       │
│              (Consumes queue, calls M5.Display)                 │
├─────────────────────────────────────────────────────────────────┤
│                      M5.Display / M5GFX                         │
└─────────────────────────────────────────────────────────────────┘
```

## Configuration

### Bridge Modes

Set in `gui/gui_config.h`:

```cpp
// Mode 0: DISABLED - Bridge completely off, direct M5.Display only
// Mode 1: PASSTHROUGH - Bridge active, routes to M5.Display (debugging)
// Mode 2: QUEUED - Full async rendering through RenderQueue (default)
// Mode 3: HYBRID - Urgent calls direct, others queued

#define GUI_LEGACY_BRIDGE_MODE 2
```

### Disabling the Bridge

To completely disable after migration:

```cpp
#define GUI_LEGACY_BRIDGE_MODE 0
```

When set to 0:
- No bridge code is compiled
- Zero memory overhead
- All code must use `GUI::Draw::*` or direct `M5.Display` calls

## Migration Steps

### Phase 1: Enable Bridge (Current State)

1. Include the bridge header in files that use display:
```cpp
#include "gui/legacy/gui_legacy_bridge.h"
```

2. Initialize bridge in setup (after GUI system):
```cpp
void setup() {
    M5.begin();

    // Initialize GUI system
    GUI::guiInit();
    GUI::guiStart();

    // Initialize legacy bridge
    GUI::LegacyBridge::init();
}
```

### Phase 2: Gradual Migration

Replace `M5.Display.*` calls with `LegacyBridge::*`:

```cpp
// Before:
M5.Display.fillScreen(TFT_BLACK);
M5.Display.setCursor(10, 20);
M5.Display.setTextColor(TFT_GREEN);
M5.Display.println("Hello");

// After:
GUI::LegacyBridge::fillScreen(GUI::Colors::Black);
GUI::LegacyBridge::setCursor(10, 20);
GUI::LegacyBridge::setTextColor(GUI::Colors::Green);
GUI::LegacyBridge::println("Hello");
```

### Phase 3: Convert to New API

Replace LegacyBridge calls with GUI::Draw::*:

```cpp
// LegacyBridge:
GUI::LegacyBridge::fillRect(10, 20, 100, 50, GUI::Colors::Red);
GUI::LegacyBridge::drawCircle(50, 50, 25, GUI::Colors::Blue);

// New API:
GUI::Draw::fillRect(10, 20, 100, 50, GUI::Colors::Red);
GUI::Draw::fillCircle(50, 50, 25, GUI::Colors::Blue);
GUI::Draw::endFrame();  // Signal frame complete
```

### Phase 4: Disable Bridge

1. Set `GUI_LEGACY_BRIDGE_MODE` to 0
2. Remove all `LegacyBridge::` calls (compiler will error on any remaining)
3. Remove `gui/legacy/gui_legacy_bridge.h` includes

## API Reference

### Screen Operations

| LegacyBridge Method | M5.Display Equivalent |
|---------------------|----------------------|
| `clear()` | `clear()` |
| `fillScreen(color)` | `fillScreen(color)` |
| `display()` | `display()` |
| `width()` | `width()` |
| `height()` | `height()` |

### Text Operations

| LegacyBridge Method | M5.Display Equivalent |
|---------------------|----------------------|
| `setCursor(x, y)` | `setCursor(x, y)` |
| `setTextColor(color)` | `setTextColor(color)` |
| `setTextColor(fg, bg)` | `setTextColor(fg, bg)` |
| `setTextSize(size)` | `setTextSize(size)` |
| `setTextFont(font)` | `setTextFont(font)` |
| `print(text)` | `print(text)` |
| `println(text)` | `println(text)` |
| `printf(fmt, ...)` | `printf(fmt, ...)` |
| `textWidth(text)` | `textWidth(text)` |
| `fontHeight()` | `fontHeight()` |
| `drawString(text, x, y)` | `drawString(text, x, y)` |

### Graphics Primitives

| LegacyBridge Method | M5.Display Equivalent |
|---------------------|----------------------|
| `drawPixel(x, y, color)` | `drawPixel(x, y, color)` |
| `drawLine(x0, y0, x1, y1, color)` | `drawLine(x0, y0, x1, y1, color)` |
| `drawRect(x, y, w, h, color)` | `drawRect(x, y, w, h, color)` |
| `fillRect(x, y, w, h, color)` | `fillRect(x, y, w, h, color)` |
| `drawCircle(x, y, r, color)` | `drawCircle(x, y, r, color)` |
| `fillCircle(x, y, r, color)` | `fillCircle(x, y, r, color)` |
| `drawRoundRect(x, y, w, h, r, color)` | `drawRoundRect(x, y, w, h, r, color)` |
| `fillRoundRect(x, y, w, h, r, color)` | `fillRoundRect(x, y, w, h, r, color)` |
| `drawTriangle(...)` | `drawTriangle(...)` |
| `fillTriangle(...)` | `fillTriangle(...)` |

### Image Operations

| LegacyBridge Method | M5.Display Equivalent |
|---------------------|----------------------|
| `pushImage(x, y, w, h, data)` | `pushImage(x, y, w, h, data)` |
| `drawJpg(data, len, x, y)` | `drawJpg(data, len, x, y)` |
| `drawJpgFile(fs, path, x, y)` | `drawJpgFile(fs, path, x, y)` |

### Display Control

| LegacyBridge Method | M5.Display Equivalent |
|---------------------|----------------------|
| `setBrightness(level)` | `setBrightness(level)` |
| `getBrightness()` | `getBrightness()` |
| `setRotation(r)` | `setRotation(r)` |
| `getRotation()` | `getRotation()` |

### Clipping

| LegacyBridge Method | M5.Display Equivalent |
|---------------------|----------------------|
| `setClipRect(x, y, w, h)` | `setClipRect(x, y, w, h)` |
| `clearClipRect()` | `clearClipRect()` |

### Synchronization

| LegacyBridge Method | Description |
|---------------------|-------------|
| `sync()` | Wait for all queued operations to complete |
| `isIdle()` | Check if queue is empty |

## Statistics & Debugging

Get bridge statistics:

```cpp
const auto& state = GUI::LegacyBridge::getState();
Serial.printf("Direct calls: %u\n", state.directCalls);
Serial.printf("Queued calls: %u\n", state.queuedCalls);
Serial.printf("Dropped calls: %u\n", state.droppedCalls);
```

Reset statistics:

```cpp
GUI::LegacyBridge::resetStats();
```

Change mode at runtime (for debugging):

```cpp
GUI::LegacyBridge::setMode(GUI_LEGACY_BRIDGE_PASSTHROUGH);  // Direct calls
GUI::LegacyBridge::setMode(GUI_LEGACY_BRIDGE_QUEUED);       // Async calls
```

## Files Requiring Migration

Based on codebase analysis, these files need migration:

| File | Priority | Estimated Legacy Calls |
|------|----------|----------------------|
| `ble_attacks.cpp` | High | ~40+ |
| `sip_attacks.cpp` | High | ~15 |
| `remote_desktop.cpp` | High | ~25 |
| `llm_gateway.cpp` | Medium | ~20 |
| `ldap_dump.cpp` | Medium | ~15 |
| `llm_chat.cpp` | Medium | ~12 |
| `file_editor.cpp` | Medium | ~20 |
| `terminals.cpp` | Low | ~10 |
| `bluetooth_keyboard.cpp` | Low | ~8 |
| `menu_engine.cpp` | Low | ~5 |
| `hardware.cpp` | Low | Wrapper layer |

## Color Constants

Use `GUI::Colors::*` instead of `TFT_*`:

```cpp
GUI::Colors::Black      // 0x0000
GUI::Colors::White      // 0xFFFF
GUI::Colors::Red        // 0xF800
GUI::Colors::Green      // 0x07E0
GUI::Colors::Blue       // 0x001F
GUI::Colors::Yellow     // 0xFFE0
GUI::Colors::Cyan       // 0x07FF
GUI::Colors::Magenta    // 0xF81F
GUI::Colors::Orange     // 0xFD20
GUI::Colors::Navy       // 0x000F
GUI::Colors::DarkGreen  // 0x03E0
GUI::Colors::DarkGrey   // 0x7BEF
GUI::Colors::LightGrey  // 0xC618

// Custom colors from RGB:
GUI::Colors::fromRGB(255, 128, 0)  // Orange
```

## Memory Usage

| Mode | Code Size | RAM Usage |
|------|-----------|-----------|
| Disabled (0) | 0 | 0 |
| Passthrough (1) | ~1KB | ~256 bytes |
| Queued (2) | ~2KB | ~256 bytes + queue |
| Hybrid (3) | ~2KB | ~256 bytes + queue |

## Best Practices

1. **Always call `sync()` before reading pixels** - ensures all draw operations complete
2. **Use `endFrame()` at end of each frame** - helps with buffer management
3. **Prefer `fillRect` over many `drawPixel` calls** - more efficient
4. **Batch related operations** - reduces queue overhead
5. **Check `isIdle()` before time-critical operations** - avoid blocking
