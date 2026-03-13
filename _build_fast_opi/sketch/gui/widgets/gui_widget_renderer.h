#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\widgets\\gui_widget_renderer.h"
/**
 * @file gui_widget_renderer.h
 * @brief Integration layer between widget system and async renderer
 *
 * This file provides:
 * - Bridge between widgets and GUI::Draw functions
 * - Dirty region optimization
 * - Frame synchronization via Signal/Slot
 * - Input processing from M5Cardputer keyboard
 */

#ifndef GUI_WIDGET_RENDERER_H
#define GUI_WIDGET_RENDERER_H

#include <Arduino.h>
#include "../gui.h"
#include "gui_widget_manager.h"
#include "gui_display_adapter.h"
#include "gui_signal.h"

// M5Cardputer keyboard
#include <M5Cardputer.h>

namespace GUI {

//=============================================================================
// Widget Renderer Integration
//=============================================================================

/**
 * @brief Integration class for widget system and async renderer
 *
 * Bridges the widget system with the existing async rendering infrastructure.
 * Handles:
 * - Dirty region tracking and optimization
 * - Frame synchronization
 * - Input event translation
 */
class WidgetRenderer {
public:
    /**
     * @brief Get singleton instance
     */
    static WidgetRenderer& instance();

    // Prevent copying
    WidgetRenderer(const WidgetRenderer&) = delete;
    WidgetRenderer& operator=(const WidgetRenderer&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    /**
     * @brief Initialize widget renderer integration
     *
     * Must be called after GUI::begin() and before using widgets.
     */
    bool init();

    /**
     * @brief Check if initialized
     */
    bool isInitialized() const { return m_initialized; }

    /**
     * @brief Shutdown
     */
    void shutdown();

    //=========================================================================
    // Main Update Loop
    //=========================================================================

    /**
     * @brief Main update function - call every frame
     *
     * This function:
     * 1. Processes keyboard input from M5Cardputer
     * 2. Updates widget state
     * 3. Renders dirty regions to async queue
     * 4. Signals end of frame
     */
    void update();

    /**
     * @brief Force full redraw
     */
    void fullRedraw();

    /**
     * @brief Sync with renderer (wait for queue to empty)
     */
    void sync();

    //=========================================================================
    // Input Processing
    //=========================================================================

    /**
     * @brief Process M5Cardputer keyboard input
     *
     * Reads current keyboard state and sends events to focused widget.
     */
    void processInput();

    /**
     * @brief Manually inject a key press
     */
    void injectKeyPress(char key, bool fn = false);

    /**
     * @brief Manually inject a key release
     */
    void injectKeyRelease(char key);

    //=========================================================================
    // Frame Control
    //=========================================================================

    /**
     * @brief Begin a new frame
     */
    void beginFrame();

    /**
     * @brief End current frame
     */
    void endFrame();

    /**
     * @brief Get current frame number
     */
    uint32_t frameNumber() const { return m_frameNumber; }

    //=========================================================================
    // Statistics
    //=========================================================================

    struct Stats {
        uint32_t framesRendered;
        uint32_t partialFrames;     // Frames with partial updates
        uint32_t fullFrames;        // Frames with full redraws
        uint32_t inputEvents;       // Key events processed
        uint32_t widgetsRendered;   // Total widgets rendered
        uint32_t avgFrameTimeUs;    // Average frame time

        Stats() { memset(this, 0, sizeof(*this)); }
    };

    const Stats& stats() const { return m_stats; }
    void resetStats() { m_stats = Stats(); }

    //=========================================================================
    // Signals
    //=========================================================================

    /**
     * @brief Signal emitted before rendering each frame
     */
    Signal& frameBeginSignal() { return m_frameBeginSignal; }

    /**
     * @brief Signal emitted after rendering each frame
     */
    Signal& frameEndSignal() { return m_frameEndSignal; }

private:
    WidgetRenderer();
    ~WidgetRenderer() = default;

    // Input state tracking
    bool m_lastKeyState[128];       // Track key states
    bool m_fnPressed;
    uint32_t m_keyPressTime[128];   // When each key was pressed
    uint32_t m_lastRepeatTime;
    uint32_t m_repeatDelayMs;
    uint32_t m_repeatIntervalMs;
    char m_lastPressedKey;

    // Frame state
    uint32_t m_frameNumber;
    uint32_t m_lastFrameTime;
    bool m_needsFullRedraw;

    // State
    bool m_initialized;

    // Statistics
    Stats m_stats;

    // Signals
    Signal m_frameBeginSignal;
    Signal m_frameEndSignal;
};

//=============================================================================
// Main API Functions
//=============================================================================

/**
 * @brief Initialize the complete widget system with async renderer
 *
 * Call this after M5.begin() and GUI::begin() to set up widgets.
 *
 * @code
 * void setup() {
 *     auto cfg = M5.config();
 *     M5Cardputer.begin(cfg);
 *     GUI::begin();
 *     GUI::initWidgetSystem();
 *
 *     // Now create and use widgets...
 * }
 * @endcode
 */
inline bool initWidgetSystem() {
    // Initialize display adapter
    DisplayAdapter::instance().init(DisplayProfile::M5Cardputer);

    // Initialize input adapter
    InputAdapter::instance().init();

    // Connect to renderer
    DisplayAdapter::instance().connectRenderer();

    // Initialize widget renderer
    return WidgetRenderer::instance().init();
}

/**
 * @brief Update widget system - call every frame in loop()
 *
 * @code
 * void loop() {
 *     M5Cardputer.update();  // Update M5 state
 *     GUI::updateWidgetSystem();  // Process widgets
 *     // ... other code
 * }
 * @endcode
 */
inline void updateWidgetSystem() {
    WidgetRenderer::instance().update();
}

/**
 * @brief Shutdown widget system
 */
inline void shutdownWidgetSystem() {
    WidgetRenderer::instance().shutdown();
    DisplayAdapter::instance().disconnectRenderer();
    WidgetManager::instance().shutdown();
}

/**
 * @brief Get widget renderer instance
 */
inline WidgetRenderer& widgetRenderer() {
    return WidgetRenderer::instance();
}

} // namespace GUI

#endif // GUI_WIDGET_RENDERER_H
