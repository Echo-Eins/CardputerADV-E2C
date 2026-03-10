/**
 * @file gui_display_adapter.h
 * @brief Display adapter for video driver integration
 *
 * Provides:
 * - Resolution querying from video driver
 * - Scale factor calculation
 * - Widget variant selection based on display
 * - Integration with async renderer via Signal/Slot
 */

#ifndef GUI_DISPLAY_ADAPTER_H
#define GUI_DISPLAY_ADAPTER_H

#include <Arduino.h>
#include "gui_widget_manager.h"
#include "gui_signal.h"

namespace GUI {

//=============================================================================
// Display Profiles
//=============================================================================

/**
 * @brief Predefined display profiles
 */
enum class DisplayProfile : uint8_t {
    M5Cardputer,        // 240x135 ST7789V (default)
    M5StickCPlus,       // 240x135 ST7789V
    M5StickC,           // 160x80  ST7789V
    M5Stack,            // 320x240 ILI9341
    TTGO_TDisplay,      // 240x135 ST7789V
    Custom              // Custom resolution
};

/**
 * @brief Display profile data
 */
struct DisplayProfileData {
    int16_t width;
    int16_t height;
    float baseDpi;              // Base DPI for scaling
    WidgetVariant defaultVariant;
    const char* name;
};

/**
 * @brief Get profile data
 */
const DisplayProfileData& getDisplayProfile(DisplayProfile profile);

//=============================================================================
// Display Adapter
//=============================================================================

/**
 * @brief Display adapter for video driver integration (Singleton)
 */
class DisplayAdapter {
public:
    /**
     * @brief Get singleton instance
     */
    static DisplayAdapter& instance();

    // Prevent copying
    DisplayAdapter(const DisplayAdapter&) = delete;
    DisplayAdapter& operator=(const DisplayAdapter&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    /**
     * @brief Initialize with display profile
     */
    void init(DisplayProfile profile = DisplayProfile::M5Cardputer);

    /**
     * @brief Initialize with custom display info
     */
    void init(const DisplayInfo& info);

    /**
     * @brief Query current display info from video driver
     */
    void queryDisplayInfo();

    /**
     * @brief Check if initialized
     */
    bool isInitialized() const { return m_initialized; }

    //=========================================================================
    // Display Info
    //=========================================================================

    /**
     * @brief Get current display info
     */
    const DisplayInfo& displayInfo() const { return m_displayInfo; }

    /**
     * @brief Get display width
     */
    int16_t width() const { return m_displayInfo.width; }

    /**
     * @brief Get display height
     */
    int16_t height() const { return m_displayInfo.height; }

    /**
     * @brief Get display rotation
     */
    uint8_t rotation() const { return m_displayInfo.rotation; }

    /**
     * @brief Set display rotation
     */
    void setRotation(uint8_t rotation);

    /**
     * @brief Get current display profile
     */
    DisplayProfile profile() const { return m_profile; }

    //=========================================================================
    // Scaling
    //=========================================================================

    /**
     * @brief Get scale factor
     */
    float scaleFactor() const { return m_displayInfo.scaleFactor; }

    /**
     * @brief Set custom scale factor
     */
    void setScaleFactor(float factor);

    /**
     * @brief Calculate scale factor from DPI
     */
    float calculateScaleFactor(float targetDpi = 160.0f) const;

    /**
     * @brief Scale a dimension
     */
    int16_t scale(int16_t value) const {
        return static_cast<int16_t>(value * m_displayInfo.scaleFactor);
    }

    /**
     * @brief Unscale a dimension
     */
    int16_t unscale(int16_t value) const {
        return static_cast<int16_t>(value / m_displayInfo.scaleFactor);
    }

    //=========================================================================
    // Widget Adaptation
    //=========================================================================

    /**
     * @brief Get recommended widget variant for display
     */
    WidgetVariant recommendedVariant() const;

    /**
     * @brief Select variant based on available space
     */
    WidgetVariant selectVariant(int16_t availableWidth, int16_t availableHeight);

    /**
     * @brief Get recommended text size
     */
    uint8_t recommendedTextSize() const;

    /**
     * @brief Get recommended padding
     */
    Insets recommendedPadding() const;

    /**
     * @brief Get recommended item height (for lists)
     */
    int16_t recommendedItemHeight() const;

    /**
     * @brief Get recommended button height
     */
    int16_t recommendedButtonHeight() const;

    /**
     * @brief Get recommended input height
     */
    int16_t recommendedInputHeight() const;

    /**
     * @brief Get recommended scrollbar width
     */
    uint8_t recommendedScrollbarWidth() const;

    //=========================================================================
    // Layout Helpers
    //=========================================================================

    /**
     * @brief Get safe area (excluding system UI)
     */
    Rect safeArea() const;

    /**
     * @brief Get taskbar area
     */
    Rect taskbarArea() const;

    /**
     * @brief Get content area (below taskbar)
     */
    Rect contentArea() const;

    /**
     * @brief Get maximum visible list items
     */
    int maxVisibleListItems() const;

    /**
     * @brief Get maximum visible columns
     */
    int maxVisibleColumns() const;

    //=========================================================================
    // Renderer Integration
    //=========================================================================

    /**
     * @brief Connect to async renderer
     */
    void connectRenderer();

    /**
     * @brief Disconnect from renderer
     */
    void disconnectRenderer();

    /**
     * @brief Check if renderer is connected
     */
    bool isRendererConnected() const { return m_rendererConnected; }

    /**
     * @brief Request render of region
     */
    void requestRender(const Rect& region);

    /**
     * @brief Request full redraw
     */
    void requestFullRedraw();

    /**
     * @brief End frame (signal to renderer)
     */
    void endFrame();

    /**
     * @brief Sync with renderer (wait for queue to empty)
     */
    void sync();

    //=========================================================================
    // Signals
    //=========================================================================

    /**
     * @brief Signal emitted when display info changes
     */
    Signal& displaySignal() { return m_displaySignal; }

    /**
     * @brief Connect to display ready event
     */
    SlotId onDisplayReady(SlotFunction handler);

    /**
     * @brief Connect to resolution changed event
     */
    SlotId onResolutionChanged(SlotFunction handler);

private:
    DisplayAdapter();
    ~DisplayAdapter() = default;

    // Internal helpers
    void updateDisplayInfo(const DisplayInfo& info);
    void emitDisplayReady();
    void emitResolutionChanged();

    // State
    bool m_initialized;
    DisplayProfile m_profile;
    DisplayInfo m_displayInfo;
    bool m_rendererConnected;

    // Signals
    Signal m_displaySignal;
};

//=============================================================================
// Convenience Functions
//=============================================================================

/**
 * @brief Get display adapter instance
 */
inline DisplayAdapter& Display() {
    return DisplayAdapter::instance();
}

//=============================================================================
// Input Adapter
//=============================================================================

/**
 * @brief Input adapter for M5Cardputer keyboard
 *
 * Translates M5Cardputer keyboard events to widget events
 */
class InputAdapter {
public:
    /**
     * @brief Get singleton instance
     */
    static InputAdapter& instance();

    // Prevent copying
    InputAdapter(const InputAdapter&) = delete;
    InputAdapter& operator=(const InputAdapter&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    /**
     * @brief Initialize input adapter
     */
    void init();

    /**
     * @brief Check if initialized
     */
    bool isInitialized() const { return m_initialized; }

    //=========================================================================
    // Input Processing
    //=========================================================================

    /**
     * @brief Update input state (call every frame)
     */
    void update();

    /**
     * @brief Process key from M5Cardputer
     * @param key Key character
     * @param fn FN key pressed
     * @param isPressed Key press (true) or release (false)
     */
    void processKey(char key, bool fn, bool isPressed);

    //=========================================================================
    // Key Mapping
    //=========================================================================

    /**
     * @brief Key mapping for navigation
     */
    enum class NavKey : uint8_t {
        Up,
        Down,
        Left,
        Right,
        Enter,
        Back,
        Tab
    };

    /**
     * @brief Check if navigation key
     */
    bool isNavKey(char key, bool fn, NavKey& navKey) const;

    /**
     * @brief Get modifiers from current state
     */
    uint8_t getModifiers() const;

    //=========================================================================
    // Key Repeat
    //=========================================================================

    /**
     * @brief Set key repeat parameters
     */
    void setKeyRepeat(uint32_t delayMs, uint32_t intervalMs);

    /**
     * @brief Get key repeat delay
     */
    uint32_t keyRepeatDelay() const { return m_repeatDelay; }

    /**
     * @brief Get key repeat interval
     */
    uint32_t keyRepeatInterval() const { return m_repeatInterval; }

    //=========================================================================
    // Signals
    //=========================================================================

    Signal& inputSignal() { return m_inputSignal; }

private:
    InputAdapter();
    ~InputAdapter() = default;

    // State
    bool m_initialized;
    bool m_fnPressed;
    char m_lastKey;
    uint32_t m_keyPressTime;
    uint32_t m_lastRepeatTime;
    uint32_t m_repeatDelay;
    uint32_t m_repeatInterval;

    // Signals
    Signal m_inputSignal;
};

/**
 * @brief Get input adapter instance
 */
inline InputAdapter& InputAdapterInstance() {
    return InputAdapter::instance();
}

} // namespace GUI

#endif // GUI_DISPLAY_ADAPTER_H
