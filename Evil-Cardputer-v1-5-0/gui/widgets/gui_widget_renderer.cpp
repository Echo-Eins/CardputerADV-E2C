/**
 * @file gui_widget_renderer.cpp
 * @brief Widget renderer integration implementation
 */

#include "gui_widget_renderer.h"
#include <M5Cardputer.h>
#include <cstring>

namespace GUI {

//=============================================================================
// Singleton
//=============================================================================

WidgetRenderer& WidgetRenderer::instance() {
    static WidgetRenderer s_instance;
    return s_instance;
}

WidgetRenderer::WidgetRenderer()
    : m_fnPressed(false)
    , m_lastPressedKey(0)
    , m_frameNumber(0)
    , m_lastFrameTime(0)
    , m_needsFullRedraw(true)
    , m_lastRepeatTime(0)
    , m_repeatDelayMs(400)
    , m_repeatIntervalMs(50)
    , m_initialized(false)
{
    memset(m_lastKeyState, 0, sizeof(m_lastKeyState));
    memset(m_keyPressTime, 0, sizeof(m_keyPressTime));
}

//=============================================================================
// Initialization
//=============================================================================

bool WidgetRenderer::init() {
    if (m_initialized) {
        return true;
    }

    // Ensure GUI system is running
    if (!guiIsRunning()) {
        Serial.println("[WidgetRenderer] GUI not running, cannot init");
        return false;
    }

    // Get display dimensions from renderer
    Renderer& r = renderer();
    uint16_t width = r.displayWidth();
    uint16_t height = r.displayHeight();

    // Update display info
    DisplayInfo info;
    info.width = width;
    info.height = height;
    info.colorDepth = 16;
    info.rotation = 0;
    info.scaleFactor = 1.0f;
    info.doubleBuffered = GUI_DOUBLE_BUFFER;
    info.dmaEnabled = GUI_USE_DMA;

    // Initialize widget manager with display info
    WidgetManager::instance().init(info);

    m_initialized = true;
    m_needsFullRedraw = true;

    Serial.printf("[WidgetRenderer] Initialized (%dx%d)\n", width, height);

    return true;
}

void WidgetRenderer::shutdown() {
    if (!m_initialized) {
        return;
    }

    WidgetManager::instance().shutdown();
    m_initialized = false;
}

//=============================================================================
// Main Update Loop
//=============================================================================

void WidgetRenderer::update() {
    if (!m_initialized) {
        return;
    }

    uint32_t frameStart = micros();

    // Begin frame
    beginFrame();

    // Process input
    processInput();

    // Update widgets (layout if needed)
    WidgetManager& wm = WidgetManager::instance();
    if (wm.root()) {
        // Check if full redraw needed
        if (m_needsFullRedraw) {
            wm.markAllDirty();
            m_needsFullRedraw = false;
            m_stats.fullFrames++;
        } else if (wm.hasDirtyRegions()) {
            m_stats.partialFrames++;
        }

        // Render dirty regions
        wm.renderDirty();

        // Submit one frame boundary for widget rendering path.
        Draw::endFrame();
    }

    // End frame
    endFrame();

    // Update stats
    uint32_t frameTime = micros() - frameStart;
    m_stats.avgFrameTimeUs = (m_stats.avgFrameTimeUs * 7 + frameTime) / 8;
    m_stats.framesRendered++;

    m_lastFrameTime = millis();
}

void WidgetRenderer::fullRedraw() {
    m_needsFullRedraw = true;
}

void WidgetRenderer::sync() {
    if (!Draw::sync()) {
        Serial.println("[WidgetRenderer] Draw::sync timeout");
    }
}

//=============================================================================
// Input Processing
//=============================================================================

void WidgetRenderer::processInput() {
    // Get keyboard state from M5Cardputer
    M5Cardputer.update();

    if (!M5Cardputer.Keyboard.isChange()) {
        // No keyboard change, check for key hold
        if (m_lastPressedKey != 0) {
            uint32_t now = millis();
            uint32_t holdTime = now - m_keyPressTime[m_lastPressedKey & 0x7F];

            // Key repeat after hold delay
            if (holdTime > m_repeatDelayMs &&
                (now - m_lastRepeatTime) >= m_repeatIntervalMs) {
                uint8_t modifiers = m_fnPressed ? KeyEventData::MOD_FN : 0;
                WidgetManager::instance().processKeyHold(m_lastPressedKey, holdTime, modifiers);
                m_lastRepeatTime = now;
            }
        }
        return;
    }

    // Get key state
    Keyboard_Class::KeysState state = M5Cardputer.Keyboard.keysState();
    m_fnPressed = state.fn;

    // Check for pressed keys
    if (M5Cardputer.Keyboard.isPressed()) {
        // Get the pressed key
        char key = 0;

        // Check special keys first
        if (state.enter) {
            key = '\n';
        } else if (state.del) {
            key = '\b';
        } else if (state.tab) {
            key = '\t';
        } else {
            // Get regular key
            // M5Cardputer Keyboard returns keys via word
            for (int i = 0; i < state.word.length(); i++) {
                key = state.word[i];
                break;  // Take first key
            }
        }

        if (key != 0) {
            uint8_t modifiers = m_fnPressed ? KeyEventData::MOD_FN : 0;

            // Track key press time
            const uint32_t now = millis();
            m_keyPressTime[key & 0x7F] = now;
            m_lastPressedKey = key;
            m_lastKeyState[key & 0x7F] = true;
            m_lastRepeatTime = now;

            // Send to widget manager
            WidgetManager::instance().processKeyInput(key, modifiers);
            m_stats.inputEvents++;
        }
    } else {
        // Key released
        if (m_lastPressedKey != 0) {
            WidgetManager::instance().processKeyRelease(
                m_lastPressedKey,
                m_fnPressed ? KeyEventData::MOD_FN : 0
            );
            m_lastKeyState[m_lastPressedKey & 0x7F] = false;
            m_lastPressedKey = 0;
            m_lastRepeatTime = 0;
        }
    }
}

void WidgetRenderer::injectKeyPress(char key, bool fn) {
    m_fnPressed = fn;
    uint8_t modifiers = fn ? KeyEventData::MOD_FN : 0;

    m_keyPressTime[key & 0x7F] = millis();
    m_lastPressedKey = key;
    m_lastKeyState[key & 0x7F] = true;
    m_lastRepeatTime = m_keyPressTime[key & 0x7F];

    WidgetManager::instance().processKeyInput(key, modifiers);
    m_stats.inputEvents++;
}

void WidgetRenderer::injectKeyRelease(char key) {
    m_lastKeyState[key & 0x7F] = false;
    WidgetManager::instance().processKeyRelease(key, m_fnPressed ? KeyEventData::MOD_FN : 0);
    if (m_lastPressedKey == key) {
        m_lastPressedKey = 0;
        m_lastRepeatTime = 0;
    }
}

//=============================================================================
// Frame Control
//=============================================================================

void WidgetRenderer::beginFrame() {
    m_frameNumber++;

    // Emit frame begin signal
    Event e(SignalType::Render, SignalPriority::High);
    e.data.render.frameId = m_frameNumber & 0xFF;
    m_frameBeginSignal.emit(e);
}

void WidgetRenderer::endFrame() {
    // Emit frame end signal
    Event e(SignalType::Render, SignalPriority::Normal);
    e.data.render.frameId = m_frameNumber & 0xFF;
    m_frameEndSignal.emit(e);
}

} // namespace GUI
