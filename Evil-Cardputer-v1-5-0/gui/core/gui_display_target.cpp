/*
 * GUI Runtime Display Target
 */

#include "gui_display_target.h"

namespace GUI {
namespace {

lgfx::LGFX_Device* s_activeDisplay = nullptr;
lgfx::LGFX_Device* s_physicalDisplay = nullptr;
uint16_t s_width = 0;
uint16_t s_height = 0;
uint8_t s_colorDepth = 16;
bool s_canvasActive = false;

inline void ensureDisplay() {
    if (s_activeDisplay == nullptr) {
        s_activeDisplay = &M5.Display;
    }
    if (s_physicalDisplay == nullptr) {
        s_physicalDisplay = s_activeDisplay;
    }
}

} // namespace

bool setRuntimeDisplay(lgfx::LGFX_Device* device, uint8_t colorDepth) {
    if (device == nullptr) {
        return false;
    }
    s_activeDisplay = device;
    s_physicalDisplay = device;
    s_canvasActive = false;
    s_colorDepth = colorDepth;
    refreshRuntimeDisplayMetrics();
    return true;
}

bool setRuntimeCanvasDisplay(lgfx::LGFX_Device* canvas, uint8_t colorDepth) {
    if (!canvas) return false;
    ensureDisplay();
    s_activeDisplay = canvas;
    s_canvasActive = true;
    s_colorDepth = colorDepth;
    refreshRuntimeDisplayMetrics();
    return true;
}

void restoreRuntimePhysicalDisplay() {
    ensureDisplay();
    s_activeDisplay = s_physicalDisplay ? s_physicalDisplay : &M5.Display;
    s_canvasActive = false;
    s_colorDepth = 16;
    refreshRuntimeDisplayMetrics();
}

void resetRuntimeDisplayToBuiltin() {
    s_activeDisplay = &M5.Display;
    s_physicalDisplay = &M5.Display;
    s_canvasActive = false;
    s_colorDepth = 16;
    refreshRuntimeDisplayMetrics();
}

lgfx::LGFX_Device& runtimeDisplay() {
    ensureDisplay();
    return *s_activeDisplay;
}

lgfx::LGFX_Device* runtimeDisplayPtr() {
    ensureDisplay();
    return s_activeDisplay;
}

lgfx::LGFX_Device& physicalDisplay() {
    ensureDisplay();
    return *s_physicalDisplay;
}

lgfx::LGFX_Device* physicalDisplayPtr() {
    ensureDisplay();
    return s_physicalDisplay;
}

bool runtimeDisplayIsCanvas() { return s_canvasActive; }

void refreshRuntimeDisplayMetrics() {
    ensureDisplay();
    const int16_t w = static_cast<int16_t>(s_activeDisplay->width());
    const int16_t h = static_cast<int16_t>(s_activeDisplay->height());
    if (w > 0) {
        s_width = static_cast<uint16_t>(w);
    }
    if (h > 0) {
        s_height = static_cast<uint16_t>(h);
    }
}

uint16_t runtimeDisplayWidth() {
    return s_width;
}

uint16_t runtimeDisplayHeight() {
    return s_height;
}

uint8_t runtimeDisplayColorDepth() {
    return s_colorDepth;
}

} // namespace GUI
