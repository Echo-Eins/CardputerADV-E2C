#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\menu_engine.cpp"
/*
 * menu_engine.cpp - Menu Navigation and Search Engine Implementation
 *
 * Extracted from main .ino file for modularity.
 */

#include "menu_engine.h"
#include "gui/gui.h"

using LB = GUI::LegacyBridge;

// ============================================================================
// Static member initialization
// ============================================================================

const char* const* MenuEngine::_menuItems = nullptr;
int MenuEngine::_menuSize = 0;
int MenuEngine::_maxVisible = 9;

char MenuEngine::_searchQuery[MENU_SEARCH_MAX_LEN + 1] = {0};
uint8_t MenuEngine::_searchLen = 0;
MenuMode MenuEngine::_mode = MENU_NAVIGATION;
bool MenuEngine::_filterLocked = false;

int16_t MenuEngine::_filteredIdx[MENU_MAX_ITEMS] = {0};
int16_t MenuEngine::_filteredCount = 0;

bool MenuEngine::_searchWaitForSRelease = false;
unsigned long MenuEngine::_searchLastKeyTime = 0;

// Note: currentIndex, menuStartIndex, lastIndex are extern globals from main .ino

// ============================================================================
// Initialization
// ============================================================================

void MenuEngine::init(const char* const* items, int itemCount, int maxVisible) {
    _menuItems = items;
    _menuSize = min(itemCount, MENU_MAX_ITEMS);
    _maxVisible = maxVisible;

    // Initialize filtered list with all items
    _filteredCount = _menuSize;
    for (int i = 0; i < _menuSize; ++i) {
        _filteredIdx[i] = i;
    }

    // Note: currentIndex, menuStartIndex, lastIndex are extern globals - don't reset them
    _mode = MENU_NAVIGATION;
    _filterLocked = false;
    _searchLen = 0;
    _searchQuery[0] = '\0';

    Serial.println(F("[MenuEngine] Initialized"));
}

// ============================================================================
// Helper functions
// ============================================================================

char MenuEngine::toLower(char c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

bool MenuEngine::icaseContains(const char* haystack, const char* needle, uint8_t needleLen) {
    if (needleLen == 0) return true;
    for (uint16_t i = 0; haystack[i]; ++i) {
        if (toLower(haystack[i]) == toLower(needle[0])) {
            uint8_t j = 1;
            while (needle[j] && haystack[i + j] && (toLower(haystack[i + j]) == toLower(needle[j]))) ++j;
            if (j == needleLen) return true;
        }
    }
    return false;
}

// ============================================================================
// Search functionality
// ============================================================================

void MenuEngine::enterSearchMode() {
    if (_mode != MENU_SEARCH) {
        _mode = MENU_SEARCH;
        _filterLocked = false;
        _searchWaitForSRelease = true;
        rebuildFilter();
        currentIndex = 0;
        menuStartIndex = 0;
        lastIndex = -1;
        Serial.println(F("[MenuEngine] Enter search mode"));
    }
}

void MenuEngine::exitSearchMode() {
    _mode = MENU_NAVIGATION;
    _filterLocked = (_searchLen > 0);
    clampSelection();
    lastIndex = -1;
    Serial.printf("[MenuEngine] Exit search -> %s\n", _filterLocked ? "filtered view" : "full menu");
}

void MenuEngine::clearSearch() {
    _searchLen = 0;
    _searchQuery[0] = '\0';
    _filterLocked = false;
    rebuildFilter();
}

void MenuEngine::addSearchChar(char c) {
    if (_searchLen < MENU_SEARCH_MAX_LEN) {
        _searchQuery[_searchLen++] = c;
        _searchQuery[_searchLen] = '\0';
        rebuildFilter();
        currentIndex = 0;
        menuStartIndex = 0;
        lastIndex = -1;
    }
}

void MenuEngine::backspaceSearch() {
    if (_searchLen > 0) {
        _searchQuery[--_searchLen] = '\0';
    }
    rebuildFilter();
}

void MenuEngine::rebuildFilter() {
    _filteredCount = 0;

    if (_searchLen == 0) {
        // Empty query - show all items
        for (int i = 0; i < _menuSize; ++i) {
            _filteredIdx[_filteredCount++] = i;
        }
        Serial.printf("[MenuEngine] query='(empty)' -> %d match(es)\n", (int)_filteredCount);
        return;
    }

    // Filter by search query
    for (int i = 0; i < _menuSize; ++i) {
        const char* item = _menuItems[i];
        if (icaseContains(item, _searchQuery, _searchLen)) {
            _filteredIdx[_filteredCount++] = i;
        }
    }
    Serial.printf("[MenuEngine] query='%s' -> %d match(es)\n", _searchQuery, (int)_filteredCount);
}

// ============================================================================
// Navigation
// ============================================================================

int MenuEngine::viewCount() {
    return (_mode == MENU_SEARCH || _filterLocked) ? (int)_filteredCount : _menuSize;
}

int MenuEngine::mapViewToRealIndex(int viewPos) {
    if (_mode == MENU_SEARCH || _filterLocked) {
        if (viewPos < 0 || viewPos >= (int)_filteredCount) return 0;
        return _filteredIdx[viewPos];
    }
    return viewPos;
}

int MenuEngine::getCurrentRealIndex() {
    return mapViewToRealIndex(currentIndex);
}

void MenuEngine::moveUp() {
    int total = viewCount();
    currentIndex--;
    if (currentIndex < 0) currentIndex = total - 1;
}

void MenuEngine::moveDown() {
    int total = viewCount();
    currentIndex++;
    if (currentIndex >= total) currentIndex = 0;
}

void MenuEngine::moveUpFast(int step) {
    int total = viewCount();
    currentIndex -= step;
    while (currentIndex < 0) currentIndex += max(1, total);
}

void MenuEngine::moveDownFast(int step) {
    int total = viewCount();
    currentIndex += step;
    if (currentIndex >= total) currentIndex %= max(1, total);
}

void MenuEngine::clampSelection() {
    int total = viewCount();
    if (total <= 0) {
        currentIndex = 0;
        menuStartIndex = 0;
        return;
    }
    if (currentIndex >= total) currentIndex = total - 1;
    if (currentIndex < 0) currentIndex = 0;
    menuStartIndex = max(0, min(currentIndex, total - _maxVisible));
}

// ============================================================================
// State accessors
// ============================================================================

MenuMode MenuEngine::getMode() {
    return _mode;
}

bool MenuEngine::isFilterLocked() {
    return _filterLocked;
}

const char* MenuEngine::getSearchQuery() {
    return _searchQuery;
}

int MenuEngine::getSearchLen() {
    return _searchLen;
}

bool MenuEngine::isWaitingForSRelease() {
    return _searchWaitForSRelease;
}

void MenuEngine::setWaitingForSRelease(bool wait) {
    _searchWaitForSRelease = wait;
}

void MenuEngine::clearSReleaseWait() {
    _searchWaitForSRelease = false;
}

bool MenuEngine::checkSearchKeyRepeat() {
    if (millis() - _searchLastKeyTime > _searchKeyRepeatDelay) {
        _searchLastKeyTime = millis();
        return true;
    }
    return false;
}

// ============================================================================
// Draw helpers
// ============================================================================

void MenuEngine::drawSearchBar(uint16_t bgColor, uint16_t textColor) {
    const int barH = 12;
    const int y = LB::height() - barH;
    LB::fillRect(0, y, LB::width(), barH, bgColor);
    LB::setTextColor(textColor, bgColor);
    LB::setCursor(5, y + 1);
    LB::print("Search: ");
    LB::print(_searchQuery);
}

// ============================================================================
// Keyboard helpers
// ============================================================================

char MenuEngine::getPrintableKey() {
    for (int c = 32; c <= 126; ++c) {
        if (M5Cardputer.Keyboard.isKeyPressed((char)c)) return (char)c;
    }
    return 0;
}

void MenuEngine::waitForKeyRelease(char key) {
    while (M5Cardputer.Keyboard.isKeyPressed(key)) {
        M5.update();
        M5Cardputer.update();
        delay(10);
    }
}

bool MenuEngine::checkKeyRepeat(unsigned long& lastTime, unsigned long delayMs) {
    if (millis() - lastTime > delayMs) {
        lastTime = millis();
        return true;
    }
    return false;
}

// ============================================================================
// Legacy compatibility functions
// ============================================================================

void enterSearchMode() {
    MenuEngine::enterSearchMode();
    drawMenu();
    MenuEngine::drawSearchBar();
}

void exitSearchModeAuto() {
    MenuEngine::exitSearchMode();
    drawMenu();
}

void rebuildMenuFilter() {
    MenuEngine::rebuildFilter();
}

int viewCount() {
    return MenuEngine::viewCount();
}

int mapViewToRealIndex(int pos) {
    return MenuEngine::mapViewToRealIndex(pos);
}

void clampMenuSelection() {
    MenuEngine::clampSelection();
}

char getPrintableKey() {
    return MenuEngine::getPrintableKey();
}

void drawSearchBar() {
    MenuEngine::drawSearchBar();
}

void clearSearchBackspaceOne() {
    MenuEngine::backspaceSearch();
}
