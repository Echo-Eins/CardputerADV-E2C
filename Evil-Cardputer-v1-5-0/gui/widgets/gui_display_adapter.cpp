/**
 * @file gui_display_adapter.cpp
 * @brief Display adapter implementation
 */

#include "gui_display_adapter.h"
#include "gui_draw.h"
#include "../gui_config.h"
#include "../core/gui_display_lock.h"
#include "../core/gui_display_target.h"
#include <M5Unified.h>
#include <algorithm>
#include <cmath>

namespace GUI {

//=============================================================================
// Display Profiles Data
//=============================================================================

static const DisplayProfileData s_profiles[] = {
    // M5Cardputer - 240x135 (default)
    { 240, 135, 160.0f, WidgetVariant::Compact, "M5Cardputer" },

    // M5StickCPlus - 240x135
    { 240, 135, 160.0f, WidgetVariant::Compact, "M5StickCPlus" },

    // M5StickC - 160x80
    { 160, 80, 120.0f, WidgetVariant::Minimal, "M5StickC" },

    // M5Stack - 320x240
    { 320, 240, 200.0f, WidgetVariant::Full, "M5Stack" },

    // TTGO T-Display - 240x135
    { 240, 135, 160.0f, WidgetVariant::Compact, "TTGO_TDisplay" },

    // Custom (placeholder)
    { 240, 135, 160.0f, WidgetVariant::Auto, "Custom" }
};

const DisplayProfileData& getDisplayProfile(DisplayProfile profile) {
    size_t index = static_cast<size_t>(profile);
    if (index >= sizeof(s_profiles) / sizeof(s_profiles[0])) {
        index = 0;  // Default to M5Cardputer
    }
    return s_profiles[index];
}

//=============================================================================
// Display Adapter
//=============================================================================

DisplayAdapter& DisplayAdapter::instance() {
    static DisplayAdapter s_instance;
    return s_instance;
}

DisplayAdapter::DisplayAdapter()
    : m_initialized(false)
    , m_profile(DisplayProfile::M5Cardputer)
    , m_rendererConnected(false)
{
}

void DisplayAdapter::init(DisplayProfile profile) {
    m_profile = profile;

    const DisplayProfileData& data = getDisplayProfile(profile);

    DisplayInfo info;
    info.width = data.width;
    info.height = data.height;
    info.colorDepth = 16;  // RGB565
    info.rotation = 0;
    info.scaleFactor = 1.0f;
    info.doubleBuffered = GUI_DOUBLE_BUFFER;
    info.dmaEnabled = GUI_USE_DMA;

    init(info);
}

void DisplayAdapter::init(const DisplayInfo& info) {
    updateDisplayInfo(info);
    m_initialized = true;

    // Initialize WidgetManager with display info
    WidgetManager::instance().init(info);

    // Set up display info callback
    WidgetManager::instance().setDisplayInfoCallback([this]() {
        return m_displayInfo;
    });

    // Prefer actual panel parameters over profile defaults when hardware is ready.
    queryDisplayInfo();

    emitDisplayReady();
}

void DisplayAdapter::queryDisplayInfo() {
    DisplayInfo info = m_displayInfo;
    DisplayLockGuard lockGuard;
    if (lockGuard.locked()) {
        const int16_t hwWidth = static_cast<int16_t>(runtimeDisplay().width());
        const int16_t hwHeight = static_cast<int16_t>(runtimeDisplay().height());
        if (hwWidth > 0 && hwHeight > 0) {
            info.width = hwWidth;
            info.height = hwHeight;
            info.rotation = static_cast<uint8_t>(runtimeDisplay().getRotation() & 0x03);
        }
    }

    // Capabilities are compile-time for this target.
    info.colorDepth = 16;
    info.doubleBuffered = GUI_DOUBLE_BUFFER;
    info.dmaEnabled = GUI_USE_DMA;

    updateDisplayInfo(info);
    WidgetManager::instance().setDisplayInfo(m_displayInfo);
}

void DisplayAdapter::updateDisplayInfo(const DisplayInfo& info) {
    bool changed = (m_displayInfo.width != info.width ||
                    m_displayInfo.height != info.height ||
                    m_displayInfo.rotation != info.rotation ||
                    m_displayInfo.scaleFactor != info.scaleFactor);

    m_displayInfo = info;

    if (changed && m_initialized) {
        emitResolutionChanged();
    }
}

void DisplayAdapter::setRotation(uint8_t rotation) {
    const uint8_t normalized = static_cast<uint8_t>(rotation & 0x03);
    if (m_displayInfo.rotation == normalized) {
        return;
    }

    DisplayInfo info = m_displayInfo;

    // Apply rotation to hardware first, then read effective dimensions back.
    DisplayLockGuard lockGuard;
    if (lockGuard.locked()) {
        runtimeDisplay().setRotation(normalized);
        const int16_t hwWidth = static_cast<int16_t>(runtimeDisplay().width());
        const int16_t hwHeight = static_cast<int16_t>(runtimeDisplay().height());
        if (hwWidth > 0 && hwHeight > 0) {
            info.width = hwWidth;
            info.height = hwHeight;
            info.rotation = static_cast<uint8_t>(runtimeDisplay().getRotation() & 0x03);
        } else {
            info.rotation = normalized;
        }
    } else {
        // If lock fails, keep deterministic software state.
        info.rotation = normalized;
    }

    updateDisplayInfo(info);
    WidgetManager::instance().setDisplayInfo(m_displayInfo);
}

void DisplayAdapter::setScaleFactor(float factor) {
    if (!std::isfinite(factor) || factor <= 0.0f) {
        GUI_LOG_ERROR("DisplayAdapter: invalid scale factor %.5f", static_cast<double>(factor));
        return;
    }

    if (m_displayInfo.scaleFactor == factor) {
        return;
    }

    DisplayInfo info = m_displayInfo;
    info.scaleFactor = factor;
    updateDisplayInfo(info);
    WidgetManager::instance().setDisplayInfo(m_displayInfo);
}

float DisplayAdapter::calculateScaleFactor(float targetDpi) const {
    if (!std::isfinite(targetDpi) || targetDpi <= 0.0f) {
        return 1.0f;
    }
    const DisplayProfileData& data = getDisplayProfile(m_profile);
    return data.baseDpi / targetDpi;
}

//=============================================================================
// Widget Adaptation
//=============================================================================

WidgetVariant DisplayAdapter::recommendedVariant() const {
    const DisplayProfileData& data = getDisplayProfile(m_profile);
    if (data.defaultVariant != WidgetVariant::Auto) {
        return data.defaultVariant;
    }

    // Calculate based on display size
    const uint32_t area = static_cast<uint32_t>(std::max<int16_t>(0, m_displayInfo.width)) *
                          static_cast<uint32_t>(std::max<int16_t>(0, m_displayInfo.height));

    if (area >= 320 * 240) {
        return WidgetVariant::Full;
    } else if (area >= 240 * 135) {
        return WidgetVariant::Compact;
    } else if (area >= 160 * 80) {
        return WidgetVariant::Minimal;
    }
    return WidgetVariant::Icon;
}

WidgetVariant DisplayAdapter::selectVariant(int16_t availableWidth,
                                            int16_t availableHeight) {
    const uint32_t area = static_cast<uint32_t>(std::max<int16_t>(0, availableWidth)) *
                          static_cast<uint32_t>(std::max<int16_t>(0, availableHeight));

    if (area >= 200 * 100) {
        return WidgetVariant::Full;
    } else if (area >= 100 * 50) {
        return WidgetVariant::Compact;
    } else if (area >= 50 * 20) {
        return WidgetVariant::Minimal;
    }
    return WidgetVariant::Icon;
}

uint8_t DisplayAdapter::recommendedTextSize() const {
    switch (recommendedVariant()) {
        case WidgetVariant::Full:     return 2;
        case WidgetVariant::Compact:  return 1;
        case WidgetVariant::Minimal:  return 1;
        case WidgetVariant::Icon:     return 1;
        default:                      return 1;
    }
}

Insets DisplayAdapter::recommendedPadding() const {
    switch (recommendedVariant()) {
        case WidgetVariant::Full:     return Insets(4, 8);
        case WidgetVariant::Compact:  return Insets(2, 4);
        case WidgetVariant::Minimal:  return Insets(1, 2);
        case WidgetVariant::Icon:     return Insets(1, 1);
        default:                      return Insets(2, 4);
    }
}

int16_t DisplayAdapter::recommendedItemHeight() const {
    switch (recommendedVariant()) {
        case WidgetVariant::Full:     return 18;
        case WidgetVariant::Compact:  return 13;
        case WidgetVariant::Minimal:  return 10;
        case WidgetVariant::Icon:     return 8;
        default:                      return 13;
    }
}

int16_t DisplayAdapter::recommendedButtonHeight() const {
    switch (recommendedVariant()) {
        case WidgetVariant::Full:     return 24;
        case WidgetVariant::Compact:  return 16;
        case WidgetVariant::Minimal:  return 12;
        case WidgetVariant::Icon:     return 10;
        default:                      return 16;
    }
}

int16_t DisplayAdapter::recommendedInputHeight() const {
    return recommendedButtonHeight();
}

uint8_t DisplayAdapter::recommendedScrollbarWidth() const {
    switch (recommendedVariant()) {
        case WidgetVariant::Full:     return 6;
        case WidgetVariant::Compact:  return 3;
        case WidgetVariant::Minimal:  return 2;
        case WidgetVariant::Icon:     return 2;
        default:                      return 3;
    }
}

//=============================================================================
// Layout Helpers
//=============================================================================

Rect DisplayAdapter::safeArea() const {
    // Full display area (no notches on M5Cardputer)
    return Rect(0, 0, m_displayInfo.width, m_displayInfo.height);
}

Rect DisplayAdapter::taskbarArea() const {
    // Taskbar at top, 10 pixels high
    const int16_t taskbarHeight = 10;
    return Rect(0, 0, m_displayInfo.width, taskbarHeight);
}

Rect DisplayAdapter::contentArea() const {
    Rect taskbar = taskbarArea();
    return Rect(0, taskbar.height + 1,  // +1 for divider
                m_displayInfo.width,
                m_displayInfo.height - taskbar.height - 1);
}

int DisplayAdapter::maxVisibleListItems() const {
    Rect content = contentArea();
    return content.height / recommendedItemHeight();
}

int DisplayAdapter::maxVisibleColumns() const {
    const int16_t charWidth = 6 * recommendedTextSize();
    return m_displayInfo.width / charWidth;
}

//=============================================================================
// Renderer Integration
//=============================================================================

void DisplayAdapter::connectRenderer() {
    // Connect to render signals
    m_rendererConnected = true;

    // Set up render callback in widget manager
    WidgetManager::instance().setRenderCallback(
        [this](const Rect& region, bool fullRedraw) {
            (void)region;
            (void)fullRedraw;
            // This is called before rendering starts
            // We could set clipping here if partial rendering
        }
    );
}

void DisplayAdapter::disconnectRenderer() {
    m_rendererConnected = false;
    WidgetManager::instance().setRenderCallback(nullptr);
}

void DisplayAdapter::requestRender(const Rect& region) {
    WidgetManager::instance().emitRenderRequest(region);
}

void DisplayAdapter::requestFullRedraw() {
    WidgetManager::instance().markAllDirty();
}

void DisplayAdapter::endFrame() {
    Draw::endFrame();
}

void DisplayAdapter::sync() {
    if (!Draw::sync()) {
        GUI_LOG_ERROR("DisplayAdapter: Draw::sync timeout");
    }
}

//=============================================================================
// Signals
//=============================================================================

void DisplayAdapter::emitDisplayReady() {
    Event e = Events::displayReady(
        m_displayInfo.width,
        m_displayInfo.height,
        m_displayInfo.colorDepth,
        m_displayInfo.rotation,
        m_displayInfo.scaleFactor
    );
    m_displaySignal.emit(e);
}

void DisplayAdapter::emitResolutionChanged() {
    Event e(SignalType::ResolutionChanged, SignalPriority::High);
    e.data.display.width = m_displayInfo.width;
    e.data.display.height = m_displayInfo.height;
    e.data.display.rotation = m_displayInfo.rotation;
    e.data.display.scaleFactor = m_displayInfo.scaleFactor;
    m_displaySignal.emit(e);
}

SlotId DisplayAdapter::onDisplayReady(SlotFunction handler) {
    return m_displaySignal.connect(SignalType::DisplayReady, handler, nullptr);
}

SlotId DisplayAdapter::onResolutionChanged(SlotFunction handler) {
    return m_displaySignal.connect(SignalType::ResolutionChanged, handler, nullptr);
}

//=============================================================================
// Input Adapter
//=============================================================================

InputAdapter& InputAdapter::instance() {
    static InputAdapter s_instance;
    return s_instance;
}

InputAdapter::InputAdapter()
    : m_initialized(false)
    , m_fnPressed(false)
    , m_lastKey(0)
    , m_keyPressTime(0)
    , m_lastRepeatTime(0)
    , m_repeatDelay(400)
    , m_repeatInterval(50)
{
}

void InputAdapter::init() {
    m_initialized = true;
}

void InputAdapter::update() {
    if (!m_initialized) return;

    uint32_t now = millis();

    // Handle key repeat
    if (m_lastKey != 0) {
        uint32_t holdTime = now - m_keyPressTime;

        if (holdTime > m_repeatDelay) {
            uint32_t timeSinceLastRepeat = now - m_lastRepeatTime;

            if (timeSinceLastRepeat > m_repeatInterval) {
                // Send key hold event
                uint8_t modifiers = getModifiers();
                WidgetManager::instance().processKeyHold(m_lastKey, holdTime, modifiers);
                m_lastRepeatTime = now;
            }
        }
    }
}

void InputAdapter::processKey(char key, bool fn, bool isPressed) {
    m_fnPressed = fn;

    if (isPressed) {
        m_lastKey = key;
        m_keyPressTime = millis();
        m_lastRepeatTime = m_keyPressTime;

        // Send key press event
        uint8_t modifiers = getModifiers();
        WidgetManager::instance().processKeyInput(key, modifiers);

        // Also emit to signal
        Event e = Events::keyPress(key, modifiers);
        m_inputSignal.emit(e);
    } else {
        // Key release
        if (m_lastKey == key) {
            m_lastKey = 0;
        }

        // Emit key release signal
        Event e(SignalType::KeyRelease, SignalPriority::Immediate);
        e.data.key.key = key;
        e.data.key.modifiers = getModifiers();
        m_inputSignal.emit(e);
    }
}

bool InputAdapter::isNavKey(char key, bool fn, NavKey& navKey) const {
    if (fn) {
        // FN + key combinations for navigation
        switch (key) {
            case ';':
                navKey = NavKey::Up;
                return true;
            case '.':
                navKey = NavKey::Down;
                return true;
            case ',':
                navKey = NavKey::Left;
                return true;
            case '/':
                navKey = NavKey::Right;
                return true;
        }
    }

    // Direct keys
    switch (key) {
        case '\n':
        case '\r':
            navKey = NavKey::Enter;
            return true;
        case '\b':
        case 127:
            navKey = NavKey::Back;
            return true;
        case '\t':
            navKey = NavKey::Tab;
            return true;
    }

    return false;
}

uint8_t InputAdapter::getModifiers() const {
    uint8_t modifiers = 0;
    if (m_fnPressed) {
        modifiers |= KeyEventData::MOD_FN;
    }
    return modifiers;
}

void InputAdapter::setKeyRepeat(uint32_t delayMs, uint32_t intervalMs) {
    m_repeatDelay = delayMs;
    m_repeatInterval = intervalMs;
}

} // namespace GUI
