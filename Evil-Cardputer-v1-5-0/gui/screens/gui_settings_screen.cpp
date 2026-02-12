/**
 * @file gui_settings_screen.cpp
 * @brief Settings screen implementation
 */

#include "gui_settings_screen.h"
#include "../widgets/gui_draw.h"
#include <cstring>
#include <cstdio>

namespace GUI {

//=============================================================================
// SettingsScreen Implementation
//=============================================================================

SettingsScreen::SettingsScreen(const char* title)
    : Widget(WidgetType::Custom, INVALID_WIDGET_ID)
    , m_selectedIndex(0)
    , m_scrollOffset(0)
    , m_editing(false)
{
    strncpy(m_title, title ? title : "Settings", sizeof(m_title) - 1);
    m_title[sizeof(m_title) - 1] = '\0';

    style().focusable = true;
    style().opaque = true;
}

SettingsScreen::~SettingsScreen() {
    clearSettings();
}

//=============================================================================
// Settings Items
//=============================================================================

void SettingsScreen::addSetting(const SettingItem& item) {
    m_settings.push_back(item);

    // If first selectable item, select it
    if (m_selectedIndex < 0 && item.enabled && item.type != SettingType::Info) {
        m_selectedIndex = static_cast<int>(m_settings.size()) - 1;
    }

    markDirty();
}

void SettingsScreen::addAction(const char* label, std::function<void()> callback) {
    addSetting(SettingItem::action(label, callback));
}

void SettingsScreen::addToggle(const char* label, bool* valuePtr,
                               std::function<void()> onChange) {
    addSetting(SettingItem::toggle(label, valuePtr, onChange));
}

void SettingsScreen::addSlider(const char* label, int* valuePtr,
                               int minVal, int maxVal, int step,
                               std::function<void()> onChange,
                               const char* suffix) {
    addSetting(SettingItem::slider(label, valuePtr, minVal, maxVal, step, onChange, suffix));
}

void SettingsScreen::addSelection(const char* label, int* valuePtr,
                                  const char** options, int optionCount,
                                  std::function<void()> onChange) {
    addSetting(SettingItem::selection(label, valuePtr, options, optionCount, onChange));
}

void SettingsScreen::addInfo(const char* label, const char* description) {
    addSetting(SettingItem::info(label, description));
}

void SettingsScreen::clearSettings() {
    m_settings.clear();
    m_selectedIndex = -1;
    m_scrollOffset = 0;
    m_editing = false;
    markDirty();
}

//=============================================================================
// Selection
//=============================================================================

void SettingsScreen::setSelectedIndex(int index) {
    if (index < 0) index = 0;
    if (index >= static_cast<int>(m_settings.size())) {
        index = static_cast<int>(m_settings.size()) - 1;
    }

    // Find selectable item
    while (index >= 0 && index < static_cast<int>(m_settings.size())) {
        const auto& item = m_settings[index];
        if (item.enabled && item.type != SettingType::Info) {
            break;
        }
        index++;
    }

    if (index != m_selectedIndex) {
        m_selectedIndex = index;
        scrollToSelected();
        markDirty();
    }
}

//=============================================================================
// Navigation
//=============================================================================

void SettingsScreen::selectPrevious() {
    int newIndex = findNextSelectable(m_selectedIndex - 1, -1);
    if (newIndex >= 0) {
        m_selectedIndex = newIndex;
        scrollToSelected();
        markDirty();
    }
}

void SettingsScreen::selectNext() {
    int newIndex = findNextSelectable(m_selectedIndex + 1, 1);
    if (newIndex >= 0) {
        m_selectedIndex = newIndex;
        scrollToSelected();
        markDirty();
    }
}

void SettingsScreen::activateCurrent() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_settings.size())) {
        return;
    }

    auto& item = m_settings[m_selectedIndex];
    if (!item.enabled) return;

    switch (item.type) {
        case SettingType::Action:
        case SettingType::SubMenu:
            if (item.callback) {
                item.callback();
            }
            break;

        case SettingType::Toggle:
            if (item.toggle.valuePtr) {
                *item.toggle.valuePtr = !*item.toggle.valuePtr;
                if (item.callback) {
                    item.callback();
                }
                emitSettingChanged(m_selectedIndex);
            }
            break;

        case SettingType::Slider:
        case SettingType::Selection:
            // Enter edit mode
            m_editing = !m_editing;
            break;

        default:
            break;
    }

    markDirty();
}

void SettingsScreen::adjustValue(int delta) {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_settings.size())) {
        return;
    }

    auto& item = m_settings[m_selectedIndex];

    switch (item.type) {
        case SettingType::Slider:
            if (item.slider.valuePtr) {
                int newVal = *item.slider.valuePtr + delta * item.slider.step;
                if (newVal < item.slider.minVal) newVal = item.slider.minVal;
                if (newVal > item.slider.maxVal) newVal = item.slider.maxVal;
                if (newVal != *item.slider.valuePtr) {
                    *item.slider.valuePtr = newVal;
                    if (item.callback) {
                        item.callback();
                    }
                    emitSettingChanged(m_selectedIndex);
                    markDirty();
                }
            }
            break;

        case SettingType::Selection:
            if (item.selection.valuePtr && item.selection.optionCount > 0) {
                int newIdx = *item.selection.valuePtr + delta;
                if (newIdx < 0) newIdx = item.selection.optionCount - 1;
                if (newIdx >= item.selection.optionCount) newIdx = 0;
                if (newIdx != *item.selection.valuePtr) {
                    *item.selection.valuePtr = newIdx;
                    if (item.callback) {
                        item.callback();
                    }
                    emitSettingChanged(m_selectedIndex);
                    markDirty();
                }
            }
            break;

        default:
            break;
    }
}

int SettingsScreen::findNextSelectable(int from, int direction) {
    int size = static_cast<int>(m_settings.size());
    if (size == 0) return -1;

    // Clamp starting point
    if (from < 0) from = size - 1;
    if (from >= size) from = 0;

    int current = from;
    for (int i = 0; i < size; i++) {
        const auto& item = m_settings[current];
        if (item.enabled && item.type != SettingType::Info) {
            return current;
        }
        current += direction;
        if (current < 0) current = size - 1;
        if (current >= size) current = 0;
    }

    return -1;
}

void SettingsScreen::scrollToSelected() {
    if (m_selectedIndex < 0) return;

    int16_t listHeight = height() - TITLE_HEIGHT - DIVIDER_HEIGHT;
    int visibleCount = listHeight / ITEM_HEIGHT;

    if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = m_selectedIndex;
    } else if (m_selectedIndex >= m_scrollOffset + visibleCount) {
        m_scrollOffset = m_selectedIndex - visibleCount + 1;
    }

    // Clamp scroll offset
    int maxOffset = static_cast<int>(m_settings.size()) - visibleCount;
    if (maxOffset < 0) maxOffset = 0;
    if (m_scrollOffset > maxOffset) m_scrollOffset = maxOffset;
    if (m_scrollOffset < 0) m_scrollOffset = 0;
}

//=============================================================================
// Signals
//=============================================================================

SlotId SettingsScreen::onBack(SlotFunction handler) {
    return signal().connect(SignalType::Blur, handler, this);
}

SlotId SettingsScreen::onSettingChanged(SlotFunction handler) {
    return signal().connect(SignalType::ValueChanged, handler, this);
}

void SettingsScreen::emitBack() {
    Event e = Events::blur();
    e.sender = this;
    signal().emit(e);
}

void SettingsScreen::emitSettingChanged(int index) {
    Event e(SignalType::ValueChanged, SignalPriority::Normal);
    e.sender = this;
    e.data.value.newValue = index;
    if (index >= 0 && index < static_cast<int>(m_settings.size())) {
        e.data.value.stringValue = m_settings[index].label;
    }
    signal().emit(e);
}

//=============================================================================
// Event Handling
//=============================================================================

bool SettingsScreen::onKeyPress(char key, uint8_t modifiers) {
    (void)modifiers;

    switch (key) {
        case ';':  // Up
            if (m_editing) {
                adjustValue(1);
            } else {
                selectPrevious();
            }
            return true;

        case '.':  // Down
            if (m_editing) {
                adjustValue(-1);
            } else {
                selectNext();
            }
            return true;

        case ',':  // Left (decrease value)
            if (m_editing || m_settings[m_selectedIndex].type == SettingType::Slider ||
                m_settings[m_selectedIndex].type == SettingType::Selection) {
                adjustValue(-1);
                return true;
            }
            break;

        case '/':  // Right (increase value)
            if (m_editing || m_settings[m_selectedIndex].type == SettingType::Slider ||
                m_settings[m_selectedIndex].type == SettingType::Selection) {
                adjustValue(1);
                return true;
            }
            break;

        case '\n':  // Enter
        case '\r':
            if (m_editing) {
                m_editing = false;
                markDirty();
            } else {
                activateCurrent();
            }
            return true;

        case '\b':  // Backspace - go back
            if (m_editing) {
                m_editing = false;
                markDirty();
            } else {
                emitBack();
            }
            return true;

        case '[':  // Page up
            for (int i = 0; i < 5; i++) selectPrevious();
            return true;

        case ']':  // Page down
            for (int i = 0; i < 5; i++) selectNext();
            return true;
    }

    return Widget::onKeyPress(key, modifiers);
}

bool SettingsScreen::onKeyHold(char key, uint32_t duration, uint8_t modifiers) {
    (void)modifiers;

    // Fast adjust on hold
    if (duration > 200) {
        if (key == ',' || key == ';') {
            adjustValue(-1);
            return true;
        } else if (key == '/' || key == '.') {
            adjustValue(1);
            return true;
        }
    }

    return false;
}

//=============================================================================
// Rendering
//=============================================================================

void SettingsScreen::render() {
    if (!isVisible()) return;

    Rect abs = absoluteBounds();
    const auto& theme = ThemeManager::instance().theme();

    // Clear background
    Draw::fillRect(abs.x, abs.y, abs.width, abs.height,
                  theme.menuBackgroundColor());

    // Render components
    renderTitleBar();
    renderSettingsList();
}

void SettingsScreen::renderContent() {
    // Content rendering is done in render() for this composite widget
}

void SettingsScreen::renderTitleBar() {
    Rect abs = absoluteBounds();
    const auto& theme = ThemeManager::instance().theme();

    // Title bar background
    Draw::fillRect(abs.x, abs.y, abs.width, TITLE_HEIGHT,
                  theme.taskbarBackgroundColor());

    // Title text
    Draw::drawText(abs.x + 2, abs.y + 2, m_title,
                  theme.taskbarTextColor(), 1);

    // Back indicator
    Draw::drawText(abs.right() - 20, abs.y + 2, "[<-]",
                  theme.taskbarTextColor(), 1);

    // Divider
    Draw::fillRect(abs.x, abs.y + TITLE_HEIGHT,
                  abs.width, DIVIDER_HEIGHT,
                  theme.taskbarDividerColor());
}

void SettingsScreen::renderSettingsList() {
    Rect abs = absoluteBounds();
    const auto& theme = ThemeManager::instance().theme();

    int16_t listY = abs.y + TITLE_HEIGHT + DIVIDER_HEIGHT;
    int16_t listHeight = abs.height - TITLE_HEIGHT - DIVIDER_HEIGHT;

    // Clear list area
    Draw::fillRect(abs.x, listY, abs.width, listHeight,
                  theme.menuBackgroundColor());

    // Render visible items
    int visibleCount = listHeight / ITEM_HEIGHT;
    for (int i = 0; i < visibleCount &&
         (m_scrollOffset + i) < static_cast<int>(m_settings.size()); i++) {
        int index = m_scrollOffset + i;
        bool selected = (index == m_selectedIndex);

        Rect itemBounds(
            abs.x,
            listY + i * ITEM_HEIGHT,
            abs.width,
            ITEM_HEIGHT
        );

        renderSettingItem(index, itemBounds, selected);
    }
}

void SettingsScreen::renderSettingItem(int index, const Rect& itemBounds, bool selected) {
    const auto& theme = ThemeManager::instance().theme();
    const auto& item = m_settings[index];

    // Selection highlight
    if (selected) {
        Draw::fillRect(itemBounds.x, itemBounds.y,
                      itemBounds.width, itemBounds.height,
                      theme.menuSelectedBackgroundColor());
    }

    // Label
    uint16_t textColor;
    if (!item.enabled) {
        textColor = Colors::DarkGrey;
    } else if (selected) {
        textColor = theme.menuTextFocusedColor();
    } else {
        textColor = theme.menuTextUnFocusedColor();
    }

    int16_t textX = itemBounds.x + ITEM_PADDING_X;
    int16_t textY = itemBounds.y + (ITEM_HEIGHT - 8) / 2;

    Draw::drawText(textX, textY, item.label, textColor, 1);

    // Value (for toggle, slider, selection)
    if (item.type != SettingType::Action && item.type != SettingType::SubMenu) {
        char valueText[32];
        getValueText(item, valueText, sizeof(valueText));

        // Value color - highlight if editing
        uint16_t valueColor = textColor;
        if (selected && m_editing) {
            valueColor = Colors::Yellow;
        }

        int16_t valueX = itemBounds.right() - VALUE_WIDTH;
        Draw::drawText(valueX, textY, valueText, valueColor, 1);
    }

    // Submenu indicator
    if (item.type == SettingType::SubMenu) {
        Draw::drawText(itemBounds.right() - 10, textY, ">", textColor, 1);
    }
}

void SettingsScreen::getValueText(const SettingItem& item, char* buffer, size_t bufSize) {
    switch (item.type) {
        case SettingType::Toggle:
            if (item.toggle.valuePtr) {
                const char* text = *item.toggle.valuePtr ?
                    (item.toggle.onText ? item.toggle.onText : "ON") :
                    (item.toggle.offText ? item.toggle.offText : "OFF");
                strncpy(buffer, text, bufSize - 1);
                buffer[bufSize - 1] = '\0';
            }
            break;

        case SettingType::Slider:
            if (item.slider.valuePtr) {
                snprintf(buffer, bufSize, "%d%s",
                        *item.slider.valuePtr,
                        item.slider.suffix ? item.slider.suffix : "");
            }
            break;

        case SettingType::Selection:
            if (item.selection.valuePtr && item.selection.options &&
                *item.selection.valuePtr >= 0 &&
                *item.selection.valuePtr < item.selection.optionCount) {
                strncpy(buffer, item.selection.options[*item.selection.valuePtr],
                       bufSize - 1);
                buffer[bufSize - 1] = '\0';
            }
            break;

        case SettingType::Info:
            if (item.description) {
                strncpy(buffer, item.description, bufSize - 1);
                buffer[bufSize - 1] = '\0';
            } else {
                buffer[0] = '\0';
            }
            break;

        default:
            buffer[0] = '\0';
            break;
    }
}

//=============================================================================
// SelectionDialog Implementation
//=============================================================================

SelectionDialog::SelectionDialog(const char* title, const char** options,
                                 int count, int current)
    : Widget(WidgetType::Custom, INVALID_WIDGET_ID)
    , m_options(options)
    , m_optionCount(count)
    , m_selectedIndex(current)
    , m_scrollOffset(0)
    , m_confirmed(false)
{
    strncpy(m_title, title ? title : "Select", sizeof(m_title) - 1);
    m_title[sizeof(m_title) - 1] = '\0';

    style().focusable = true;
    style().opaque = true;
}

bool SelectionDialog::onKeyPress(char key, uint8_t modifiers) {
    (void)modifiers;

    switch (key) {
        case ';':  // Up
            if (m_selectedIndex > 0) {
                m_selectedIndex--;
                markDirty();
            }
            return true;

        case '.':  // Down
            if (m_selectedIndex < m_optionCount - 1) {
                m_selectedIndex++;
                markDirty();
            }
            return true;

        case '\n':
        case '\r':
            m_confirmed = true;
            return true;

        case '\b':
            m_confirmed = false;
            // Signal close without selection
            return true;
    }

    return Widget::onKeyPress(key, modifiers);
}

void SelectionDialog::render() {
    Rect abs = absoluteBounds();
    const auto& theme = ThemeManager::instance().theme();

    // Dialog background
    Draw::fillRect(abs.x, abs.y, abs.width, abs.height,
                  theme.menuBackgroundColor());

    // Border
    Draw::drawRect(abs.x, abs.y, abs.width, abs.height,
                  theme.taskbarDividerColor());

    // Title
    Draw::fillRect(abs.x + 1, abs.y + 1, abs.width - 2, 12,
                  theme.taskbarBackgroundColor());
    Draw::drawText(abs.x + 4, abs.y + 2, m_title,
                  theme.taskbarTextColor(), 1);

    // Options
    int16_t listY = abs.y + 14;
    int visibleCount = (abs.height - 14) / ITEM_HEIGHT;

    // Ensure selected is visible
    if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = m_selectedIndex;
    } else if (m_selectedIndex >= m_scrollOffset + visibleCount) {
        m_scrollOffset = m_selectedIndex - visibleCount + 1;
    }

    for (int i = 0; i < visibleCount && (m_scrollOffset + i) < m_optionCount; i++) {
        int idx = m_scrollOffset + i;
        bool selected = (idx == m_selectedIndex);

        int16_t itemY = listY + i * ITEM_HEIGHT;

        if (selected) {
            Draw::fillRect(abs.x + 1, itemY, abs.width - 2, ITEM_HEIGHT,
                          theme.menuSelectedBackgroundColor());
        }

        uint16_t textColor = selected ?
            theme.menuTextFocusedColor() : theme.menuTextUnFocusedColor();

        Draw::drawText(abs.x + 4, itemY + 2, m_options[idx], textColor, 1);
    }
}

} // namespace GUI
