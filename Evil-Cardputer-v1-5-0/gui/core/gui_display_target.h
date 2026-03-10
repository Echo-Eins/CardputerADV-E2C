/*
 * GUI Runtime Display Target
 *
 * Provides a single runtime-selected display device for the GUI stack.
 * Renderer, DMA, LegacyBridge and widgets use this instead of hardcoded M5.Display.
 */

#ifndef GUI_DISPLAY_TARGET_H
#define GUI_DISPLAY_TARGET_H

#include <cstdint>
#include <M5Unified.h>

namespace GUI {

// Set active display device for GUI runtime.
// Returns false if device is null.
bool setRuntimeDisplay(lgfx::LGFX_Device* device, uint8_t colorDepth = 16);

// Reset active display to built-in M5.Display.
void resetRuntimeDisplayToBuiltin();

// Get active display reference/pointer.
lgfx::LGFX_Device& runtimeDisplay();
lgfx::LGFX_Device* runtimeDisplayPtr();

// Refresh cached width/height from active display.
void refreshRuntimeDisplayMetrics();

// Cached active display metrics.
uint16_t runtimeDisplayWidth();
uint16_t runtimeDisplayHeight();
uint8_t runtimeDisplayColorDepth();

} // namespace GUI

#endif // GUI_DISPLAY_TARGET_H
