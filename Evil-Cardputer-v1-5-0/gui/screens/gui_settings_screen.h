/**
 * @file gui_settings_screen.h
 * @brief Settings screen widget using the new async GUI system
 *
 * Provides:
 * - ListView-based settings menu
 * - Various setting types (toggle, slider, selection)
 * - Navigation with back support
 * - Theme-aware styling
 */

#ifndef GUI_SETTINGS_SCREEN_H
#define GUI_SETTINGS_SCREEN_H

#include "../widgets/gui_widget.h"
#include "../widgets/gui_container.h"
#include "../widgets/gui_label.h"
#include "../widgets/gui_signal.h"
#include "../gui_theme.h"
#include <functional>
#include <vector>

namespace GUI {

//=============================================================================
// Setting Types
//=============================================================================

enum class SettingType : uint8_t {
    Action,         // Simple action (just calls callback)
    Toggle,         // On/Off toggle
    Slider,         // Numeric value with min/max
    Selection,      // Choose from list
    SubMenu,        // Opens another screen
    Info            // Display-only (not selectable)
};

//=============================================================================
// Setting Item
//=============================================================================

/**
 * @brief Data for a single setting
 */
struct SettingItem {
    const char* label;              // Display label
    const char* description;        // Optional description
    SettingType type;               // Setting type

    // Value storage (union-like)
    union {
        struct {
            bool* valuePtr;         // Pointer to bool value
            const char* onText;     // Text when ON
            const char* offText;    // Text when OFF
        } toggle;

        struct {
            int* valuePtr;          // Pointer to int value
            int minVal;             // Minimum value
            int maxVal;             // Maximum value
            int step;               // Step size
            const char* suffix;     // Unit suffix (e.g., "%", "ms")
        } slider;

        struct {
            int* valuePtr;          // Pointer to selection index
            const char** options;   // Array of option strings
            int optionCount;        // Number of options
        } selection;
    };

    std::function<void()> callback; // Action callback
    bool enabled;                   // Is setting enabled

    SettingItem()
        : label(nullptr)
        , description(nullptr)
        , type(SettingType::Action)
        , callback(nullptr)
        , enabled(true) {
        memset(&toggle, 0, sizeof(toggle));
    }

    // Create action setting
    static SettingItem action(const char* label,
                              std::function<void()> callback) {
        SettingItem item;
        item.label = label;
        item.type = SettingType::Action;
        item.callback = callback;
        return item;
    }

    // Create toggle setting
    static SettingItem makeToggle(const char* label,
                              bool* valuePtr,
                              std::function<void()> onChange = nullptr,
                              const char* onText = "ON",
                              const char* offText = "OFF") {
        SettingItem item;
        item.label = label;
        item.type = SettingType::Toggle;
        item.toggle.valuePtr = valuePtr;
        item.toggle.onText = onText;
        item.toggle.offText = offText;
        item.callback = onChange;
        return item;
    }

    // Create slider setting
    static SettingItem makeSlider(const char* label,
                              int* valuePtr,
                              int minVal, int maxVal, int step = 1,
                              std::function<void()> onChange = nullptr,
                              const char* suffix = "") {
        SettingItem item;
        item.label = label;
        item.type = SettingType::Slider;
        item.slider.valuePtr = valuePtr;
        item.slider.minVal = minVal;
        item.slider.maxVal = maxVal;
        item.slider.step = step;
        item.slider.suffix = suffix;
        item.callback = onChange;
        return item;
    }

    // Create selection setting
    static SettingItem makeSelection(const char* label,
                                 int* valuePtr,
                                 const char** options, int optionCount,
                                 std::function<void()> onChange = nullptr) {
        SettingItem item;
        item.label = label;
        item.type = SettingType::Selection;
        item.selection.valuePtr = valuePtr;
        item.selection.options = options;
        item.selection.optionCount = optionCount;
        item.callback = onChange;
        return item;
    }

    // Create submenu setting
    static SettingItem submenu(const char* label,
                               std::function<void()> openCallback) {
        SettingItem item;
        item.label = label;
        item.type = SettingType::SubMenu;
        item.callback = openCallback;
        return item;
    }

    // Create info setting
    static SettingItem info(const char* label, const char* description) {
        SettingItem item;
        item.label = label;
        item.description = description;
        item.type = SettingType::Info;
        item.enabled = false;
        return item;
    }
};

//=============================================================================
// Settings Screen Widget
//=============================================================================

/**
 * @brief Settings screen with various control types
 *
 * Layout:
 * +-------------------+
 * | Title Bar (12px)  |
 * +-------------------+
 * | Divider (1px)     |
 * +-------------------+
 * |                   |
 * | Settings List     |
 * |                   |
 * +-------------------+
 */
class SettingsScreen : public Widget {
public:
    /**
     * @brief Construct settings screen
     * @param title Screen title
     */
    explicit SettingsScreen(const char* title = "Settings");
    virtual ~SettingsScreen();

    //=========================================================================
    // Settings Items
    //=========================================================================

    /**
     * @brief Add a setting item
     */
    void addSetting(const SettingItem& item);

    /**
     * @brief Add action setting
     */
    void addAction(const char* label, std::function<void()> callback);

    /**
     * @brief Add toggle setting
     */
    void addToggle(const char* label, bool* valuePtr,
                   std::function<void()> onChange = nullptr);

    /**
     * @brief Add slider setting
     */
    void addSlider(const char* label, int* valuePtr,
                   int minVal, int maxVal, int step = 1,
                   std::function<void()> onChange = nullptr,
                   const char* suffix = "");

    /**
     * @brief Add selection setting
     */
    void addSelection(const char* label, int* valuePtr,
                      const char** options, int optionCount,
                      std::function<void()> onChange = nullptr);

    /**
     * @brief Add separator/info item
     */
    void addInfo(const char* label, const char* description = nullptr);

    /**
     * @brief Clear all settings
     */
    void clearSettings();

    /**
     * @brief Get setting count
     */
    size_t settingCount() const { return m_settings.size(); }

    //=========================================================================
    // Selection
    //=========================================================================

    /**
     * @brief Get selected index
     */
    int selectedIndex() const { return m_selectedIndex; }

    /**
     * @brief Set selected index
     */
    void setSelectedIndex(int index);

    /**
     * @brief Check if editing a value
     */
    bool isEditing() const { return m_editing; }

    //=========================================================================
    // Navigation
    //=========================================================================

    /**
     * @brief Move selection up
     */
    void selectPrevious();

    /**
     * @brief Move selection down
     */
    void selectNext();

    /**
     * @brief Activate current setting
     */
    void activateCurrent();

    /**
     * @brief Adjust current setting value (for slider/selection)
     */
    void adjustValue(int delta);

    //=========================================================================
    // Signals
    //=========================================================================

    /**
     * @brief Connect to back button pressed
     */
    SlotId onBack(SlotFunction handler);

    /**
     * @brief Connect to setting value changed
     */
    SlotId onSettingChanged(SlotFunction handler);

    //=========================================================================
    // Event Handling
    //=========================================================================

    bool onKeyPress(char key, uint8_t modifiers) override;
    bool onKeyHold(char key, uint32_t duration, uint8_t modifiers) override;

    //=========================================================================
    // Rendering
    //=========================================================================

    void render() override;
    void renderContent() override;

protected:
    /**
     * @brief Render title bar
     */
    void renderTitleBar();

    /**
     * @brief Render settings list
     */
    void renderSettingsList();

    /**
     * @brief Render single setting item
     */
    void renderSettingItem(int index, const Rect& itemBounds, bool selected);

    /**
     * @brief Get display text for setting value
     */
    void getValueText(const SettingItem& item, char* buffer, size_t bufSize);

    /**
     * @brief Emit back event
     */
    void emitBack();

    /**
     * @brief Emit setting changed event
     */
    void emitSettingChanged(int index);

    /**
     * @brief Find next selectable item
     */
    int findNextSelectable(int from, int direction);

    /**
     * @brief Ensure selection is visible
     */
    void scrollToSelected();

private:
    // Title
    char m_title[32];

    // Settings list
    std::vector<SettingItem> m_settings;

    // Selection state
    int m_selectedIndex;
    int m_scrollOffset;
    bool m_editing;

    // Layout constants
    static constexpr int16_t TITLE_HEIGHT = 12;
    static constexpr int16_t DIVIDER_HEIGHT = 1;
    static constexpr int16_t ITEM_HEIGHT = 12;
    static constexpr int16_t ITEM_PADDING_X = 5;
    static constexpr int16_t VALUE_WIDTH = 60;
};

//=============================================================================
// Selection Dialog (for SettingType::Selection)
//=============================================================================

/**
 * @brief Modal dialog for selecting from a list of options
 */
class SelectionDialog : public Widget {
public:
    SelectionDialog(const char* title, const char** options, int count, int current);
    virtual ~SelectionDialog() = default;

    int selectedIndex() const { return m_selectedIndex; }
    bool isConfirmed() const { return m_confirmed; }

    bool onKeyPress(char key, uint8_t modifiers) override;
    void render() override;

private:
    char m_title[32];
    const char** m_options;
    int m_optionCount;
    int m_selectedIndex;
    int m_scrollOffset;
    bool m_confirmed;

    static constexpr int16_t ITEM_HEIGHT = 12;
};

} // namespace GUI

#endif // GUI_SETTINGS_SCREEN_H
