/**
 * @file gui_widget_manager.h
 * @brief Central widget management system
 *
 * Responsibilities:
 * - Widget lifecycle management
 * - Focus management
 * - Input event routing
 * - Dirty region tracking for lazy rendering
 * - Display adaptation (resolution, scaling)
 * - Integration with async renderer via Signal/Slot
 */

#ifndef GUI_WIDGET_MANAGER_H
#define GUI_WIDGET_MANAGER_H

#include <Arduino.h>
#include <vector>
#include <memory>
#include <functional>
#include <cstring>
#include "gui_widget.h"
#include "gui_signal.h"

namespace GUI {

// Forward declarations
class Widget;

//=============================================================================
// Display Info
//=============================================================================

/**
 * @brief Display information from video driver
 */
struct DisplayInfo {
    int16_t width;              // Physical width
    int16_t height;             // Physical height
    uint8_t colorDepth;         // Color depth (16 = RGB565)
    uint8_t rotation;           // Display rotation (0, 1, 2, 3)
    float scaleFactor;          // Scaling factor
    bool doubleBuffered;        // Double buffering enabled
    bool dmaEnabled;            // DMA transfers enabled

    DisplayInfo()
        : width(240)
        , height(135)
        , colorDepth(16)
        , rotation(0)
        , scaleFactor(1.0f)
        , doubleBuffered(true)
        , dmaEnabled(true) {}
};

//=============================================================================
// Input State
//=============================================================================

/**
 * @brief Current input state from keyboard
 */
struct InputState {
    bool fnPressed;             // FN key state
    char lastKey;               // Last pressed key
    uint8_t modifiers;          // Current modifiers
    uint32_t keyPressTime;      // When key was pressed
    uint32_t keyHoldTime;       // How long key held
    uint32_t lastRepeatCount;   // Last delivered repeat ordinal
    uint32_t lastRepeatTime;    // Timestamp of last repeat dispatch
    bool isRepeat;              // Is this a repeat event

    InputState()
        : fnPressed(false)
        , lastKey(0)
        , modifiers(0)
        , keyPressTime(0)
        , keyHoldTime(0)
        , lastRepeatCount(0)
        , lastRepeatTime(0)
        , isRepeat(false) {}
};

//=============================================================================
// Widget Manager
//=============================================================================

/**
 * @brief Central widget management system (Singleton)
 */
class WidgetManager {
public:
    /**
     * @brief Get singleton instance
     */
    static WidgetManager& instance();

    // Prevent copying
    WidgetManager(const WidgetManager&) = delete;
    WidgetManager& operator=(const WidgetManager&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    /**
     * @brief Initialize the widget system
     * @param displayInfo Display information from video driver
     */
    void init(const DisplayInfo& displayInfo);

    /**
     * @brief Check if initialized
     */
    bool isInitialized() const { return m_initialized; }

    /**
     * @brief Shutdown the widget system
     */
    void shutdown();

    //=========================================================================
    // Display Info
    //=========================================================================

    /**
     * @brief Get current display info
     */
    const DisplayInfo& displayInfo() const { return m_displayInfo; }

    /**
     * @brief Update display info (e.g., on rotation change)
     */
    void setDisplayInfo(const DisplayInfo& info);

    /**
     * @brief Request display info from video driver
     */
    void queryDisplayInfo();

    /**
     * @brief Get display width
     */
    int16_t displayWidth() const { return m_displayInfo.width; }

    /**
     * @brief Get display height
     */
    int16_t displayHeight() const { return m_displayInfo.height; }

    /**
     * @brief Get scale factor
     */
    float scaleFactor() const { return m_displayInfo.scaleFactor; }

    //=========================================================================
    // Root Widget Management
    //=========================================================================

    /**
     * @brief Set root widget (takes ownership)
     */
    void setRoot(Widget* root);

    /**
     * @brief Get root widget
     */
    Widget* root() const { return m_root; }

    /**
     * @brief Clear root widget
     */
    void clearRoot();

    //=========================================================================
    // Screen/Page Management
    //=========================================================================

    /**
     * @brief Push a new screen (saves current root)
     */
    void pushScreen(Widget* screen);

    /**
     * @brief Pop current screen (restore previous)
     * @return Previous root widget (caller takes ownership) or nullptr
     */
    Widget* popScreen();

    /**
     * @brief Get screen stack depth
     */
    size_t screenStackDepth() const { return m_screenStack.size(); }

    //=========================================================================
    // Focus Management
    //=========================================================================

    /**
     * @brief Get currently focused widget
     */
    Widget* focusedWidget() const { return m_focusedWidget; }

    /**
     * @brief Set focus to widget
     * @return true if focus was set
     */
    bool setFocus(Widget* widget);

    /**
     * @brief Clear focus
     */
    void clearFocus();

    /**
     * @brief Move focus to next focusable widget
     */
    void focusNext();

    /**
     * @brief Move focus to previous focusable widget
     */
    void focusPrevious();

    /**
     * @brief Find first focusable widget in tree
     */
    Widget* findFirstFocusable(Widget* root = nullptr);

    /**
     * @brief Find next focusable widget after given widget
     */
    Widget* findNextFocusable(Widget* from);

    /**
     * @brief Find previous focusable widget before given widget
     */
    Widget* findPreviousFocusable(Widget* from);

    //=========================================================================
    // Input Handling
    //=========================================================================

    /**
     * @brief Process keyboard input
     * @param key Key code
     * @param modifiers Modifier flags
     * @return true if input was handled
     */
    bool processKeyInput(char key, uint8_t modifiers);

    /**
     * @brief Process key hold (repeat)
     * @param key Key code
     * @param duration Hold duration in ms
     * @param modifiers Modifier flags
     * @return true if input was handled
     */
    bool processKeyHold(char key, uint32_t duration, uint8_t modifiers);

    /**
     * @brief Notify about key release (clears repeat state)
     */
    void processKeyRelease(char key, uint8_t modifiers);

    /**
     * @brief Update input state from M5Cardputer keyboard
     * Call this every frame to process input
     */
    void updateInput();

    /**
     * @brief Get current input state
     */
    const InputState& inputState() const { return m_inputState; }

    //=========================================================================
    // Dirty Tracking & Rendering
    //=========================================================================

    /**
     * @brief Mark region as dirty (needs redraw)
     */
    void markDirty(const Rect& region);

    /**
     * @brief Mark widget as dirty
     */
    void markDirty(Widget* widget);

    /**
     * @brief Mark entire screen dirty
     */
    void markAllDirty();

    /**
     * @brief Check if any region is dirty
     */
    bool hasDirtyRegions() const { return !m_dirtyRegions.empty(); }

    /**
     * @brief Get combined dirty region
     */
    Rect combinedDirtyRegion() const;

    /**
     * @brief Clear dirty regions
     */
    void clearDirtyRegions();

    /**
     * @brief Render dirty widgets only (lazy rendering)
     */
    void renderDirty();

    /**
     * @brief Render all widgets (full redraw)
     */
    void renderAll();

    /**
     * @brief Update cycle - call every frame
     * Processes input, updates animations, renders dirty regions
     */
    void update();

    //=========================================================================
    // Signals
    //=========================================================================

    /**
     * @brief Get global signal emitter
     * Used for system-wide events (theme changes, etc.)
     */
    Signal& globalSignal() { return m_globalSignal; }

    /**
     * @brief Emit render request to async renderer
     */
    void emitRenderRequest(const Rect& region);

    /**
     * @brief Emit invalidate request
     */
    void emitInvalidate(const Rect& region);

    //=========================================================================
    // Widget Lookup
    //=========================================================================

    /**
     * @brief Find widget by ID
     */
    Widget* findWidget(WidgetId id);

    /**
     * @brief Find widget by name
     */
    Widget* findWidget(const char* name);

    /**
     * @brief Find widget at coordinates
     */
    Widget* widgetAt(int16_t x, int16_t y);

    //=========================================================================
    // Layout
    //=========================================================================

    /**
     * @brief Trigger layout pass
     */
    void layout();

    /**
     * @brief Request layout on next update
     */
    void requestLayout();

    //=========================================================================
    // Statistics
    //=========================================================================

    /**
     * @brief Get number of widgets
     */
    size_t widgetCount() const;

    /**
     * @brief Get render statistics
     */
    struct RenderStats {
        uint32_t framesRendered;
        uint32_t partialRenders;
        uint32_t fullRenders;
        uint32_t widgetsRendered;
        uint32_t lastRenderTimeUs;
        float avgRenderTimeUs;

        RenderStats() { memset(this, 0, sizeof(*this)); }
    };

    const RenderStats& renderStats() const { return m_renderStats; }

    //=========================================================================
    // Callbacks for Video Driver Integration
    //=========================================================================

    /**
     * @brief Callback type for render requests
     */
    using RenderCallback = std::function<void(const Rect& region, bool fullRedraw)>;

    /**
     * @brief Set render callback (called when widget needs rendering)
     */
    void setRenderCallback(RenderCallback callback) {
        m_renderCallback = callback;
    }

    /**
     * @brief Callback for display info queries
     */
    using DisplayInfoCallback = std::function<DisplayInfo()>;

    /**
     * @brief Set display info callback
     */
    void setDisplayInfoCallback(DisplayInfoCallback callback) {
        m_displayInfoCallback = callback;
    }

private:
    WidgetManager();
    ~WidgetManager();

    // Recursive helpers
    void collectFocusableWidgets(Widget* widget, std::vector<Widget*>& list);
    size_t countWidgets(Widget* widget) const;
    Widget* findWidgetByIdRecursive(Widget* widget, WidgetId id);
    Widget* findWidgetByNameRecursive(Widget* widget, const char* name);
    Widget* widgetAtRecursive(Widget* widget, int16_t x, int16_t y);
    void renderWidgetIfDirty(Widget* widget);

    // State
    bool m_initialized;
    DisplayInfo m_displayInfo;
    Widget* m_root;
    Widget* m_focusedWidget;
    std::vector<Widget*> m_screenStack;

    // Input
    InputState m_inputState;
    uint32_t m_keyRepeatDelay;      // Delay before repeat (ms)
    uint32_t m_keyRepeatInterval;   // Interval between repeats (ms)

    // Dirty tracking
    std::vector<Rect> m_dirtyRegions;
    bool m_needsLayout;
    bool m_fullRedrawPending;

    // Signals
    Signal m_globalSignal;

    // Callbacks
    RenderCallback m_renderCallback;
    DisplayInfoCallback m_displayInfoCallback;

    // Statistics
    RenderStats m_renderStats;
};

//=============================================================================
// Convenience Macros
//=============================================================================

#define WIDGETS GUI::WidgetManager::instance()

} // namespace GUI

#endif // GUI_WIDGET_MANAGER_H
