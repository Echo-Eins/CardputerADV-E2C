#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\screens\\gui_menu_screen.h"
/**
 * @file gui_menu_screen.h
 * @brief Main menu screen widget using the new async GUI system
 *
 * Provides:
 * - ListView-based menu rendering
 * - Search/filter integration with MenuEngine
 * - Taskbar with status indicators
 * - Theme-aware styling
 */

#ifndef GUI_MENU_SCREEN_H
#define GUI_MENU_SCREEN_H

#include "../widgets/gui_widget.h"
#include "../widgets/gui_container.h"
#include "../widgets/gui_label.h"
#include "../widgets/gui_signal.h"
#include "../gui_theme.h"
#include "../../menu_engine.h"

namespace GUI {

// Forward declarations
class MenuScreen;

//=============================================================================
// Menu Item Extended (with icon support)
//=============================================================================

struct MenuItemData {
    const char* text;           // Menu item text
    int16_t realIndex;          // Real index in menuItems array
    uint16_t textColor;         // Custom color (0 = use theme)
    bool enabled;               // Is item enabled

    MenuItemData()
        : text(nullptr)
        , realIndex(-1)
        , textColor(0)
        , enabled(true) {}

    MenuItemData(const char* t, int16_t idx)
        : text(t)
        , realIndex(idx)
        , textColor(0)
        , enabled(true) {}
};

//=============================================================================
// Taskbar Widget (internal)
//=============================================================================

/**
 * @brief Taskbar for menu screen with status indicators
 */
class MenuTaskbar : public Widget {
public:
    MenuTaskbar();
    virtual ~MenuTaskbar() = default;

    void setTitle(const char* title);
    void setBatteryLevel(uint8_t level);
    void setWifiConnected(bool connected);
    void setTime(const char* timeStr);
    void setSearchMode(bool active, const char* query = nullptr);

    void renderContent() override;

private:
    char m_title[16];
    char m_time[8];
    char m_searchQuery[MENU_SEARCH_MAX_LEN + 1];
    uint8_t m_batteryLevel;
    bool m_wifiConnected;
    bool m_searchMode;
};

//=============================================================================
// Search Bar Widget (internal)
//=============================================================================

/**
 * @brief Search bar shown at bottom during search mode
 */
class MenuSearchBar : public Widget {
public:
    MenuSearchBar();
    virtual ~MenuSearchBar() = default;

    void setQuery(const char* query);
    const char* query() const { return m_query; }

    void renderContent() override;

private:
    char m_query[MENU_SEARCH_MAX_LEN + 1];
};

//=============================================================================
// Menu Screen Widget
//=============================================================================

/**
 * @brief Main menu screen with integrated taskbar and search
 *
 * Layout:
 * +-------------------+
 * | Taskbar (12px)    |
 * +-------------------+
 * | Divider (1px)     |
 * +-------------------+
 * |                   |
 * | Menu ListView     |
 * |                   |
 * +-------------------+
 * | Search Bar (opt)  |
 * +-------------------+
 */
class MenuScreen : public Widget {
public:
    /**
     * @brief Construct menu screen
     * @param menuItems Array of menu item strings (PROGMEM)
     * @param itemCount Number of menu items
     */
    MenuScreen(const char* const* menuItems, size_t itemCount);
    virtual ~MenuScreen();

    //=========================================================================
    // Menu Items
    //=========================================================================

    /**
     * @brief Get current selected real index
     */
    int selectedRealIndex() const;

    /**
     * @brief Set selected index (in current view)
     */
    void setSelectedIndex(int viewIndex);

    /**
     * @brief Get number of items in current view
     */
    size_t viewItemCount() const { return m_viewCount; }

    /**
     * @brief Refresh items from MenuEngine filter
     */
    void refreshFromFilter();

    //=========================================================================
    // Search Mode
    //=========================================================================

    /**
     * @brief Check if in search mode
     */
    bool isSearchMode() const { return m_searchMode; }

    /**
     * @brief Enter search mode
     */
    void enterSearchMode();

    /**
     * @brief Exit search mode
     */
    void exitSearchMode();

    /**
     * @brief Add character to search
     */
    void addSearchChar(char c);

    /**
     * @brief Remove last search character
     */
    void backspaceSearch();

    /**
     * @brief Get search query
     */
    const char* searchQuery() const { return m_searchQuery; }

    //=========================================================================
    // Taskbar
    //=========================================================================

    /**
     * @brief Update taskbar time
     */
    void setTime(const char* timeStr);

    /**
     * @brief Update battery level
     */
    void setBatteryLevel(uint8_t level);

    /**
     * @brief Update WiFi status
     */
    void setWifiConnected(bool connected);

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
     * @brief Fast scroll up
     */
    void selectPreviousFast(int step = 3);

    /**
     * @brief Fast scroll down
     */
    void selectNextFast(int step = 3);

    //=========================================================================
    // Signals
    //=========================================================================

    /**
     * @brief Connect to item selection changed
     */
    SlotId onSelectionChanged(SlotFunction handler);

    /**
     * @brief Connect to item activated (Enter pressed)
     */
    SlotId onItemActivated(SlotFunction handler);

    /**
     * @brief Connect to search mode changed
     */
    SlotId onSearchModeChanged(SlotFunction handler);

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
     * @brief Render taskbar
     */
    void renderTaskbar();

    /**
     * @brief Render menu list
     */
    void renderMenuList();

    /**
     * @brief Render search bar
     */
    void renderSearchBar();

    /**
     * @brief Render single menu item
     */
    void renderMenuItem(int viewIndex, const Rect& itemBounds, bool selected);

    /**
     * @brief Update internal filter from search query
     */
    void rebuildFilter();

    /**
     * @brief Clamp selection to valid range
     */
    void clampSelection();

    /**
     * @brief Number of items that fit in the current viewport
     */
    int visibleItemCount() const;

    /**
     * @brief Emit item activated event
     */
    void emitItemActivated(int realIndex);

    /**
     * @brief Emit selection changed event
     */
    void emitSelectionChanged(int oldIndex, int newIndex);

private:
    // Menu items reference
    const char* const* m_menuItems;
    size_t m_menuItemCount;

    // Filtered view
    int16_t m_filteredIndices[MENU_MAX_ITEMS];
    size_t m_viewCount;

    // Selection state
    int m_selectedIndex;
    int m_scrollOffset;

    // Search state
    bool m_searchMode;
    char m_searchQuery[MENU_SEARCH_MAX_LEN + 1];
    uint8_t m_searchLen;

    // Taskbar state
    char m_time[8];
    uint8_t m_batteryLevel;
    bool m_wifiConnected;

    // Layout constants
    static constexpr int16_t TASKBAR_HEIGHT = 12;
    static constexpr int16_t DIVIDER_HEIGHT = 1;
    static constexpr int16_t SEARCH_BAR_HEIGHT = 12;
    static constexpr int16_t ITEM_HEIGHT = 13;
    static constexpr int16_t ITEM_PADDING_X = 5;

    // Debounce
    unsigned long m_lastKeyTime;
    static constexpr unsigned long KEY_REPEAT_DELAY = 150;
};

} // namespace GUI

#endif // GUI_MENU_SCREEN_H
