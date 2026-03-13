#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\core\\gui_display_target.cpp"
/*
 * GUI Runtime Display Target
 */

#include "gui_display_target.h"

namespace GUI {
namespace {

lgfx::LGFX_Device* s_activeDisplay = nullptr;
uint16_t s_width = 0;
uint16_t s_height = 0;
uint8_t s_colorDepth = 16;

inline void ensureDisplay() {
    if (s_activeDisplay == nullptr) {
        s_activeDisplay = &M5.Display;
    }
}

} // namespace

bool setRuntimeDisplay(lgfx::LGFX_Device* device, uint8_t colorDepth) {
    if (device == nullptr) {
        return false;
    }
    s_activeDisplay = device;
    s_colorDepth = colorDepth;
    refreshRuntimeDisplayMetrics();
    return true;
}

void resetRuntimeDisplayToBuiltin() {
    s_activeDisplay = &M5.Display;
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
