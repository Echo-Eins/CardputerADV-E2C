/**
 * @file gui_menu_screen.cpp
 * @brief Main menu screen implementation
 */

#include "gui_menu_screen.h"
#include "../widgets/gui_draw.h"
#include <algorithm>
#include <cstring>
#include <cctype>

namespace GUI {

//=============================================================================
// Helper Functions
//=============================================================================

static char toLowerChar(char c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool icaseContains(const char* haystack, const char* needle, uint8_t needleLen) {
    if (!haystack || !needle || needleLen == 0) return true;

    size_t hayLen = strlen(haystack);
    if (needleLen > hayLen) return false;

    for (size_t i = 0; i <= hayLen - needleLen; i++) {
        bool match = true;
        for (uint8_t j = 0; j < needleLen; j++) {
            if (toLowerChar(haystack[i + j]) != toLowerChar(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

//=============================================================================
// MenuScreen Implementation
//=============================================================================

MenuScreen::MenuScreen(const char* const* menuItems, size_t itemCount)
    : Widget(WidgetType::Custom, INVALID_WIDGET_ID)
    , m_menuItems(menuItems)
    , m_menuItemCount(itemCount)
    , m_viewCount(itemCount)
    , m_selectedIndex(0)
    , m_scrollOffset(0)
    , m_searchMode(false)
    , m_searchLen(0)
    , m_batteryLevel(100)
    , m_wifiConnected(false)
    , m_lastKeyTime(0)
{
    m_searchQuery[0] = '\0';
    m_time[0] = '\0';

    // Initialize filtered indices (all items visible)
    for (size_t i = 0; i < itemCount && i < MENU_MAX_ITEMS; i++) {
        m_filteredIndices[i] = static_cast<int16_t>(i);
    }

    style().focusable = true;
    style().opaque = true;
}

MenuScreen::~MenuScreen() {
    // Nothing to clean up - we don't own the menu items
}

//=============================================================================
// Menu Items
//=============================================================================

int MenuScreen::selectedRealIndex() const {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_viewCount)) {
        return -1;
    }
    return m_filteredIndices[m_selectedIndex];
}

void MenuScreen::setSelectedIndex(int viewIndex) {
    if (viewIndex < 0) viewIndex = 0;
    if (viewIndex >= static_cast<int>(m_viewCount)) {
        viewIndex = static_cast<int>(m_viewCount) - 1;
    }

    if (m_selectedIndex != viewIndex) {
        int oldIndex = selectedRealIndex();
        m_selectedIndex = viewIndex;

        // Scroll to keep selection visible
        int visibleCount = visibleItemCount();
        if (m_selectedIndex < m_scrollOffset) {
            m_scrollOffset = m_selectedIndex;
        } else if (m_selectedIndex >= m_scrollOffset + visibleCount) {
            m_scrollOffset = m_selectedIndex - visibleCount + 1;
        }

        markDirty();
        emitSelectionChanged(oldIndex, selectedRealIndex());
    }
}

void MenuScreen::refreshFromFilter() {
    rebuildFilter();
    clampSelection();
    markDirty();
}

//=============================================================================
// Search Mode
//=============================================================================

void MenuScreen::enterSearchMode() {
    if (!m_searchMode) {
        m_searchMode = true;
        m_searchQuery[0] = '\0';
        m_searchLen = 0;
        markDirty();

        // Emit search mode changed
        Event e(SignalType::ValueChanged, SignalPriority::Normal);
        e.sender = this;
        e.data.value.newValue = 1;  // Search mode ON
        signal().emit(e);
    }
}

void MenuScreen::exitSearchMode() {
    if (m_searchMode) {
        m_searchMode = false;
        // Keep filter if items match, otherwise clear
        if (m_viewCount == 0) {
            m_searchQuery[0] = '\0';
            m_searchLen = 0;
            rebuildFilter();
        }
        markDirty();

        // Emit search mode changed
        Event e(SignalType::ValueChanged, SignalPriority::Normal);
        e.sender = this;
        e.data.value.newValue = 0;  // Search mode OFF
        signal().emit(e);
    }
}

void MenuScreen::addSearchChar(char c) {
    if (m_searchLen < MENU_SEARCH_MAX_LEN) {
        m_searchQuery[m_searchLen++] = c;
        m_searchQuery[m_searchLen] = '\0';
        rebuildFilter();
        clampSelection();
        markDirty();
    }
}

void MenuScreen::backspaceSearch() {
    if (m_searchLen > 0) {
        m_searchLen--;
        m_searchQuery[m_searchLen] = '\0';
        rebuildFilter();
        clampSelection();
        markDirty();
    } else if (m_searchMode) {
        exitSearchMode();
    }
}

//=============================================================================
// Taskbar
//=============================================================================

void MenuScreen::setTime(const char* timeStr) {
    if (timeStr) {
        strncpy(m_time, timeStr, sizeof(m_time) - 1);
        m_time[sizeof(m_time) - 1] = '\0';
    } else {
        m_time[0] = '\0';
    }
    markDirty();
}

void MenuScreen::setBatteryLevel(uint8_t level) {
    if (m_batteryLevel != level) {
        m_batteryLevel = level;
        markDirty();
    }
}

void MenuScreen::setWifiConnected(bool connected) {
    if (m_wifiConnected != connected) {
        m_wifiConnected = connected;
        markDirty();
    }
}

//=============================================================================
// Navigation
//=============================================================================

void MenuScreen::selectPrevious() {
    if (m_selectedIndex > 0) {
        setSelectedIndex(m_selectedIndex - 1);
    }
}

void MenuScreen::selectNext() {
    if (m_selectedIndex < static_cast<int>(m_viewCount) - 1) {
        setSelectedIndex(m_selectedIndex + 1);
    }
}

void MenuScreen::selectPreviousFast(int step) {
    setSelectedIndex(m_selectedIndex - step);
}

void MenuScreen::selectNextFast(int step) {
    setSelectedIndex(m_selectedIndex + step);
}

int MenuScreen::visibleItemCount() const {
    int16_t menuHeight = height() - TASKBAR_HEIGHT - DIVIDER_HEIGHT;
    if (m_searchMode) {
        menuHeight -= SEARCH_BAR_HEIGHT;
    }
    return menuHeight / ITEM_HEIGHT;
}

//=============================================================================
// Signals
//=============================================================================

SlotId MenuScreen::onSelectionChanged(SlotFunction handler) {
    return signal().connect(SignalType::SelectionChanged, handler, this);
}

SlotId MenuScreen::onItemActivated(SlotFunction handler) {
    return signal().connect(SignalType::Click, handler, this);
}

SlotId MenuScreen::onSearchModeChanged(SlotFunction handler) {
    return signal().connect(SignalType::ValueChanged, handler, this);
}

void MenuScreen::emitItemActivated(int realIndex) {
    Event e = Events::click();
    e.sender = this;
    e.data.value.newValue = realIndex;
    if (realIndex >= 0 && realIndex < static_cast<int>(m_menuItemCount)) {
        e.data.value.stringValue = m_menuItems[realIndex];
    }
    signal().emit(e);
}

void MenuScreen::emitSelectionChanged(int oldIndex, int newIndex) {
    Event e(SignalType::SelectionChanged, SignalPriority::High);
    e.sender = this;
    e.data.value.oldValue = oldIndex;
    e.data.value.newValue = newIndex;
    signal().emit(e);
}

//=============================================================================
// Event Handling
//=============================================================================

bool MenuScreen::onKeyPress(char key, uint8_t modifiers) {
    // Handle navigation keys
    switch (key) {
        case ';':  // Up
            selectPrevious();
            return true;

        case '.':  // Down
            selectNext();
            return true;

        case '\n':  // Enter
        case '\r':
            if (m_selectedIndex >= 0 && m_viewCount > 0) {
                emitItemActivated(selectedRealIndex());
            }
            return true;

        case '\b':  // Backspace
            if (m_searchMode) {
                backspaceSearch();
                return true;
            }
            break;

        case '[':  // Page up
            selectPreviousFast(visibleItemCount() - 1);
            return true;

        case ']':  // Page down
            selectNextFast(visibleItemCount() - 1);
            return true;

        case 's':
        case 'S':
            // Toggle search mode (with debounce)
            if (!m_searchMode) {
                enterSearchMode();
            } else {
                // In search mode, 's' is a search character
                addSearchChar(key);
            }
            return true;

        default:
            // In search mode, printable characters are search input
            if (m_searchMode && key >= ' ' && key <= '~') {
                addSearchChar(key);
                return true;
            }
            break;
    }

    return Widget::onKeyPress(key, modifiers);
}

bool MenuScreen::onKeyHold(char key, uint32_t duration, uint8_t modifiers) {
    (void)modifiers;

    // Fast scroll on hold
    if (duration > 200) {
        if (key == ';') {
            selectPrevious();
            return true;
        } else if (key == '.') {
            selectNext();
            return true;
        }
    }

    return false;
}

//=============================================================================
// Rendering
//=============================================================================

void MenuScreen::render() {
    if (!isVisible()) return;

    Rect abs = absoluteBounds();
    const auto& theme = ThemeManager::instance().theme();

    // Clear background
    Draw::fillRect(abs.x, abs.y, abs.width, abs.height,
                  theme.menuBackgroundColor());

    // Render components
    renderTaskbar();
    renderMenuList();

    if (m_searchMode) {
        renderSearchBar();
    }
}

void MenuScreen::renderContent() {
    // Content rendering is done in render() for this composite widget
}

void MenuScreen::renderTaskbar() {
    Rect abs = absoluteBounds();
    const auto& theme = ThemeManager::instance().theme();

    int16_t taskbarY = abs.y;

    // Taskbar background
    Draw::fillRect(abs.x, taskbarY, abs.width, TASKBAR_HEIGHT,
                  theme.taskbarBackgroundColor());

    // Title/Search indicator
    const char* title = m_searchMode ? "Search" : "Menu";
    Draw::drawText(abs.x + 2, taskbarY + 2, title,
                  theme.taskbarTextColor(), static_cast<uint8_t>(1));

    // WiFi indicator
    int16_t indicatorX = abs.right() - 40;
    if (m_wifiConnected) {
        Draw::drawText(indicatorX, taskbarY + 2, "W",
                      theme.taskbarTextColor(), static_cast<uint8_t>(1));
    }

    // Battery indicator
    indicatorX += 12;
    char batStr[5];
    snprintf(batStr, sizeof(batStr), "%d%%", m_batteryLevel);
    Draw::drawText(indicatorX, taskbarY + 2, batStr,
                  theme.taskbarTextColor(), static_cast<uint8_t>(1));

    // Time (if set)
    if (m_time[0] != '\0') {
        int16_t timeX = abs.x + 50;
        Draw::drawText(timeX, taskbarY + 2, m_time,
                      theme.taskbarTextColor(), static_cast<uint8_t>(1));
    }

    // Divider line
    Draw::fillRect(abs.x, taskbarY + TASKBAR_HEIGHT,
                  abs.width, DIVIDER_HEIGHT,
                  theme.taskbarDividerColor());
}

void MenuScreen::renderMenuList() {
    Rect abs = absoluteBounds();
    const auto& theme = ThemeManager::instance().theme();

    int16_t menuY = abs.y + TASKBAR_HEIGHT + DIVIDER_HEIGHT;
    int16_t menuHeight = abs.height - TASKBAR_HEIGHT - DIVIDER_HEIGHT;
    if (m_searchMode) {
        menuHeight -= SEARCH_BAR_HEIGHT;
    }

    // Clear menu area
    Draw::fillRect(abs.x, menuY, abs.width, menuHeight,
                  theme.menuBackgroundColor());

    // Render visible items
    int visibleCount = menuHeight / ITEM_HEIGHT;
    for (int i = 0; i < visibleCount && (m_scrollOffset + i) < static_cast<int>(m_viewCount); i++) {
        int viewIndex = m_scrollOffset + i;
        bool selected = (viewIndex == m_selectedIndex);

        Rect itemBounds(
            abs.x,
            menuY + i * ITEM_HEIGHT,
            abs.width,
            ITEM_HEIGHT
        );

        renderMenuItem(viewIndex, itemBounds, selected);
    }
}

void MenuScreen::renderMenuItem(int viewIndex, const Rect& itemBounds, bool selected) {
    const auto& theme = ThemeManager::instance().theme();

    int16_t realIndex = m_filteredIndices[viewIndex];
    if (realIndex < 0 || realIndex >= static_cast<int16_t>(m_menuItemCount)) {
        return;
    }

    const char* text = m_menuItems[realIndex];

    // Selection highlight
    if (selected) {
        Draw::fillRect(itemBounds.x, itemBounds.y,
                      itemBounds.width, itemBounds.height,
                      theme.menuSelectedBackgroundColor());
    }

    // Text
    uint16_t textColor = selected ?
                         theme.menuTextFocusedColor() :
                         theme.menuTextUnFocusedColor();

    int16_t textX = itemBounds.x + ITEM_PADDING_X;
    int16_t textY = itemBounds.y + (ITEM_HEIGHT - 8) / 2;

    Draw::drawText(textX, textY, text, textColor, static_cast<uint8_t>(1));
}

void MenuScreen::renderSearchBar() {
    Rect abs = absoluteBounds();
    const auto& theme = ThemeManager::instance().theme();

    int16_t searchY = abs.bottom() - SEARCH_BAR_HEIGHT;

    // Background
    Draw::fillRect(abs.x, searchY, abs.width, SEARCH_BAR_HEIGHT,
                  theme.taskbarBackgroundColor());

    // Search text
    char searchText[MENU_SEARCH_MAX_LEN + 10];
    snprintf(searchText, sizeof(searchText), "Search: %s_", m_searchQuery);
    Draw::drawText(abs.x + 2, searchY + 2, searchText,
                  Colors::Yellow, static_cast<uint8_t>(1));
}

//=============================================================================
// Internal Helpers
//=============================================================================

void MenuScreen::rebuildFilter() {
    if (m_searchLen == 0) {
        // No search query - show all items
        m_viewCount = m_menuItemCount;
        for (size_t i = 0; i < m_menuItemCount && i < MENU_MAX_ITEMS; i++) {
            m_filteredIndices[i] = static_cast<int16_t>(i);
        }
    } else {
        // Filter items matching search query
        m_viewCount = 0;
        for (size_t i = 0; i < m_menuItemCount && m_viewCount < MENU_MAX_ITEMS; i++) {
            if (icaseContains(m_menuItems[i], m_searchQuery, m_searchLen)) {
                m_filteredIndices[m_viewCount++] = static_cast<int16_t>(i);
            }
        }
    }
}

void MenuScreen::clampSelection() {
    if (m_viewCount == 0) {
        m_selectedIndex = -1;
        m_scrollOffset = 0;
    } else {
        if (m_selectedIndex < 0) {
            m_selectedIndex = 0;
        }
        if (m_selectedIndex >= static_cast<int>(m_viewCount)) {
            m_selectedIndex = static_cast<int>(m_viewCount) - 1;
        }

        // Clamp scroll offset
        int visibleCount = visibleItemCount();
        if (m_scrollOffset > m_selectedIndex) {
            m_scrollOffset = m_selectedIndex;
        }
        if (m_selectedIndex >= m_scrollOffset + visibleCount) {
            m_scrollOffset = m_selectedIndex - visibleCount + 1;
        }
        if (m_scrollOffset < 0) {
            m_scrollOffset = 0;
        }
        int maxOffset = static_cast<int>(m_viewCount) - visibleCount;
        if (maxOffset < 0) maxOffset = 0;
        if (m_scrollOffset > maxOffset) {
            m_scrollOffset = maxOffset;
        }
    }
}

} // namespace GUI
