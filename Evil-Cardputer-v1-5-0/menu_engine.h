/*
 * menu_engine.h - Menu Navigation and Search Engine
 *
 * Extracted from main .ino file for modularity.
 * Provides:
 * - Menu search/filter functionality
 * - Navigation state management
 * - Reusable menu drawing helpers
 */

#ifndef MENU_ENGINE_H
#define MENU_ENGINE_H

#include <Arduino.h>
#include <M5Cardputer.h>

// ============================================================================
// Constants
// ============================================================================

#define MENU_SEARCH_MAX_LEN     16
#define MENU_MAX_ITEMS          128
#define MENU_DEFAULT_LINE_HEIGHT 13
#define MENU_DEFAULT_START_Y    10
#define MENU_DEFAULT_START_X    5

// ============================================================================
// Menu Mode Enum
// ============================================================================

enum MenuMode : uint8_t {
    MENU_NAVIGATION = 0,
    MENU_SEARCH = 1
};

// ============================================================================
// MenuEngine Class
// ============================================================================

class MenuEngine {
public:
    // Initialize with menu items array and size
    static void init(const char* const* items, int itemCount, int maxVisible);

    // Search functionality
    static void enterSearchMode();
    static void exitSearchMode();
    static void clearSearch();
    static void addSearchChar(char c);
    static void backspaceSearch();
    static void rebuildFilter();

    // Navigation
    static void moveUp();
    static void moveDown();
    static void moveUpFast(int step = 3);
    static void moveDownFast(int step = 3);
    static void clampSelection();

    // Index mapping
    static int viewCount();
    static int mapViewToRealIndex(int viewPos);
    static int getCurrentRealIndex();

    // State accessors
    static MenuMode getMode();
    static bool isFilterLocked();
    static const char* getSearchQuery();
    static int getSearchLen();

    // Search debounce state
    static bool isWaitingForSRelease();
    static void setWaitingForSRelease(bool wait);
    static void clearSReleaseWait();
    static bool checkSearchKeyRepeat();

    // Draw helpers
    static void drawSearchBar(uint16_t bgColor = TFT_BLACK, uint16_t textColor = TFT_YELLOW);

    // Keyboard helpers
    static char getPrintableKey();
    static void waitForKeyRelease(char key);

    // Debounce helpers
    static bool checkKeyRepeat(unsigned long& lastTime, unsigned long delay = 150);

private:
    // Menu items reference (stored in .ino)
    static const char* const* _menuItems;
    static int _menuSize;
    static int _maxVisible;

    // Search state
    static char _searchQuery[MENU_SEARCH_MAX_LEN + 1];
    static uint8_t _searchLen;
    static MenuMode _mode;
    static bool _filterLocked;

    // Filtered indices
    static int16_t _filteredIdx[MENU_MAX_ITEMS];
    static int16_t _filteredCount;

    // Debounce state
    static bool _searchWaitForSRelease;
    static unsigned long _searchLastKeyTime;
    static const unsigned long _searchKeyRepeatDelay = 200;

    // Helper functions
    static char toLower(char c);
    static bool icaseContains(const char* haystack, const char* needle, uint8_t needleLen);
};

// ============================================================================
// External dependencies from main .ino
// ============================================================================

// Theme colors (defined in main .ino as int)
extern int menuBackgroundColor;
extern int menuSelectedBackgroundColor;
extern int menuTextFocusedColor;
extern int menuTextUnFocusedColor;

// Menu state flag
extern bool inMenu;

// Navigation state (globals in main .ino, used by MenuEngine)
extern int currentIndex;
extern int lastIndex;
extern int menuStartIndex;

// Draw function (defined in main .ino)
extern void drawMenu();

// ============================================================================
// Legacy compatibility functions
// ============================================================================

// These wrap MenuEngine methods for backward compatibility with existing code
void enterSearchMode();
void exitSearchModeAuto();
void rebuildMenuFilter();
int viewCount();
int mapViewToRealIndex(int pos);
void clampMenuSelection();
char getPrintableKey();
void drawSearchBar();

// Clear search with backspace
void clearSearchBackspaceOne();

#endif // MENU_ENGINE_H
