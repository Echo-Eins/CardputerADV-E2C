#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\widgets\\gui_container.cpp"
/**
 * @file gui_container.cpp
 * @brief Container widgets implementation
 */

#include "gui_container.h"
#include "gui_draw.h"
#include <cstring>
#include <cstdlib>

namespace GUI {

//=============================================================================
// Container
//=============================================================================

Container::Container(WidgetId id)
    : Widget(WidgetType::Container, id)
    , m_layoutDirection(LayoutDirection::Vertical)
    , m_spacing(2)
{
    style().opaque = false;
}

void Container::layout() {
    if (m_layoutDirection == LayoutDirection::None) {
        // No auto-layout, just measure children
        Widget::layout();
        return;
    }

    Rect content = contentRect();
    int16_t x = 0;
    int16_t y = 0;

    for (Widget* child : children()) {
        if (!child->isVisible()) continue;

        // Measure child
        child->measure(content.width, content.height);

        // Position child
        if (m_layoutDirection == LayoutDirection::Vertical) {
            child->setPosition(x, y);
            child->setSize(content.width, child->measuredHeight());
            y += child->height() + m_spacing;
        } else {
            child->setPosition(x, y);
            child->setSize(child->measuredWidth(), content.height);
            x += child->width() + m_spacing;
        }

        // Layout child's children
        child->layout();
    }
}

void Container::renderContent() {
    // Container itself has no content, just render children
    // Children are rendered by Widget::render() -> renderChildren()
}

//=============================================================================
// ScrollView
//=============================================================================

ScrollView::ScrollView(WidgetId id)
    : Container(id)
    , m_scrollX(0)
    , m_scrollY(0)
    , m_contentWidth(0)
    , m_contentHeight(0)
    , m_showScrollbar(true)
    , m_scrollbarWidth(3)
{
    style().focusable = true;
}

void ScrollView::setScrollPosition(int16_t x, int16_t y) {
    if (m_scrollX != x || m_scrollY != y) {
        m_scrollX = x;
        m_scrollY = y;
        clampScrollPosition();
        markDirty();
    }
}

void ScrollView::scrollBy(int16_t dx, int16_t dy) {
    setScrollPosition(m_scrollX + dx, m_scrollY + dy);
}

void ScrollView::scrollToWidget(Widget* widget) {
    if (!widget) return;

    Rect wBounds = widget->bounds();
    Rect viewport = contentRect();

    // Check if widget is visible
    if (wBounds.y < m_scrollY) {
        m_scrollY = wBounds.y;
    } else if (wBounds.bottom() > m_scrollY + viewport.height) {
        m_scrollY = wBounds.bottom() - viewport.height;
    }

    if (wBounds.x < m_scrollX) {
        m_scrollX = wBounds.x;
    } else if (wBounds.right() > m_scrollX + viewport.width) {
        m_scrollX = wBounds.right() - viewport.width;
    }

    clampScrollPosition();
    markDirty();
}

void ScrollView::scrollToTop() {
    setScrollPosition(m_scrollX, 0);
}

void ScrollView::scrollToBottom() {
    Rect viewport = contentRect();
    setScrollPosition(m_scrollX, m_contentHeight - viewport.height);
}

void ScrollView::setContentSize(int16_t w, int16_t h) {
    m_contentWidth = w;
    m_contentHeight = h;
    clampScrollPosition();
}

bool ScrollView::canScrollHorizontally() const {
    return m_contentWidth > contentRect().width;
}

bool ScrollView::canScrollVertically() const {
    return m_contentHeight > contentRect().height;
}

void ScrollView::clampScrollPosition() {
    Rect viewport = contentRect();

    int16_t maxScrollX = max(0, m_contentWidth - viewport.width);
    int16_t maxScrollY = max(0, m_contentHeight - viewport.height);

    m_scrollX = constrain(m_scrollX, (int16_t)0, maxScrollX);
    m_scrollY = constrain(m_scrollY, (int16_t)0, maxScrollY);
}

void ScrollView::updateContentSize() {
    int16_t maxW = 0;
    int16_t maxH = 0;

    for (Widget* child : children()) {
        if (!child->isVisible()) continue;

        int16_t childRight = child->x() + child->width();
        int16_t childBottom = child->y() + child->height();

        if (childRight > maxW) maxW = childRight;
        if (childBottom > maxH) maxH = childBottom;
    }

    m_contentWidth = maxW;
    m_contentHeight = maxH;
}

bool ScrollView::onKeyPress(char key, uint8_t modifiers) {
    const int16_t scrollStep = 13;  // About one line

    // Navigation keys
    switch (key) {
        case ';':  // Up (FN + ;)
            if (modifiers & KeyEventData::MOD_FN) {
                scrollBy(0, -scrollStep);
                return true;
            }
            break;

        case '.':  // Down (FN + .)
            if (modifiers & KeyEventData::MOD_FN) {
                scrollBy(0, scrollStep);
                return true;
            }
            break;

        case ',':  // Left (FN + ,)
            if (modifiers & KeyEventData::MOD_FN) {
                scrollBy(-scrollStep, 0);
                return true;
            }
            break;

        case '/':  // Right (FN + /)
            if (modifiers & KeyEventData::MOD_FN) {
                scrollBy(scrollStep, 0);
                return true;
            }
            break;

        // Page up/down using [ and ]
        case '[':
            scrollBy(0, -contentRect().height / 2);
            return true;

        case ']':
            scrollBy(0, contentRect().height / 2);
            return true;
    }

    return Container::onKeyPress(key, modifiers);
}

void ScrollView::layout() {
    Container::layout();
    updateContentSize();
    clampScrollPosition();
}

void ScrollView::renderContent() {
    Rect abs = absoluteBounds();
    Rect viewport = contentRect();

    // Draw background
    if (style().opaque) {
        Draw::fillRect(abs.x, abs.y, abs.width, abs.height,
                      effectiveBackgroundColor());
    }

    // Set clip rectangle for scrolling content
    WidgetDraw::setClip(abs.x + style().padding.left,
                        abs.y + style().padding.top,
                        viewport.width - (m_showScrollbar && canScrollVertically() ? m_scrollbarWidth : 0),
                        viewport.height);

    // Render visible children with scroll offset
    for (Widget* child : children()) {
        if (!child->isVisible()) continue;

        // Check if child is visible in viewport
        Rect childBounds = child->bounds();
        childBounds.x -= m_scrollX;
        childBounds.y -= m_scrollY;

        if (childBounds.intersects(Rect(0, 0, viewport.width, viewport.height))) {
            // Temporarily adjust child position for scroll
            int16_t origX = child->x();
            int16_t origY = child->y();
            child->setRenderPosition(origX - m_scrollX,
                                     origY - m_scrollY);

            child->render();

            // Restore position
            child->setRenderPosition(origX, origY);
        }
    }

    // Clear clip
    WidgetDraw::clearClip();

    // Draw scrollbar
    if (m_showScrollbar) {
        renderScrollbar();
    }
}

void ScrollView::renderChildren() {
    // Children are rendered manually inside renderContent() with viewport offset.
}

void ScrollView::renderScrollbar() {
    if (!canScrollVertically() && !canScrollHorizontally()) {
        return;
    }

    Rect abs = absoluteBounds();

    // Vertical scrollbar
    if (canScrollVertically()) {
        int16_t trackX = abs.right() - m_scrollbarWidth - 1;
        int16_t trackY = abs.y + style().padding.top;
        int16_t trackH = abs.height - style().padding.vertical();

        // Track
        Draw::fillRect(trackX, trackY, m_scrollbarWidth, trackH,
                      ContainerColors::ScrollbarTrack);

        // Thumb
        float visibleRatio = (float)contentRect().height / m_contentHeight;
        float scrollRatio = (float)m_scrollY / (m_contentHeight - contentRect().height);

        int16_t thumbH = max((int16_t)8, (int16_t)(trackH * visibleRatio));
        int16_t thumbY = trackY + (int16_t)((trackH - thumbH) * scrollRatio);

        Draw::fillRect(trackX, thumbY, m_scrollbarWidth - 1, thumbH,
                      ContainerColors::ScrollbarThumb);
    }

    // Horizontal scrollbar (if needed)
    if (canScrollHorizontally()) {
        int16_t trackX = abs.x + style().padding.left;
        int16_t trackY = abs.bottom() - m_scrollbarWidth - 1;
        int16_t trackW = abs.width - style().padding.horizontal();

        if (canScrollVertically()) {
            trackW -= m_scrollbarWidth;  // Account for vertical scrollbar
        }

        // Track
        Draw::fillRect(trackX, trackY, trackW, m_scrollbarWidth,
                      ContainerColors::ScrollbarTrack);

        // Thumb
        float visibleRatio = (float)contentRect().width / m_contentWidth;
        float scrollRatio = (float)m_scrollX / (m_contentWidth - contentRect().width);

        int16_t thumbW = max((int16_t)8, (int16_t)(trackW * visibleRatio));
        int16_t thumbX = trackX + (int16_t)((trackW - thumbW) * scrollRatio);

        Draw::fillRect(thumbX, trackY, thumbW, m_scrollbarWidth - 1,
                      ContainerColors::ScrollbarThumb);
    }
}

//=============================================================================
// ListView
//=============================================================================

ListView::ListView(WidgetId id)
    : Widget(WidgetType::ListView, id)
    , m_items(nullptr)
    , m_itemCount(0)
    , m_itemCapacity(0)
    , m_selectedIndex(-1)
    , m_scrollOffset(0)
    , m_itemHeight(13)
    , m_itemRenderer(nullptr)
    , m_showScrollbar(true)
    , m_scrollbarWidth(3)
{
    style().focusable = true;
    style().opaque = true;
    style().backgroundColor = ContainerColors::ItemBackground;
}

ListView::~ListView() {
    clearItems();
}

bool ListView::ensureCapacity(size_t required) {
    if (required <= m_itemCapacity) {
        return true;
    }

    size_t newCapacity = required + 16;
    ListItem* newItems = (ListItem*)realloc(m_items, newCapacity * sizeof(ListItem));
    if (!newItems) {
        return false;
    }

    m_items = newItems;
    m_itemCapacity = newCapacity;
    return true;
}

void ListView::setItems(const ListItem* items, size_t count) {
    clearItems();

    if (!ensureCapacity(count)) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        m_items[i] = items[i];
        // Deep copy text if needed
        if (items[i].text) {
            m_items[i].text = strdup(items[i].text);
        }
        if (items[i].subtext) {
            m_items[i].subtext = strdup(items[i].subtext);
        }
    }

    m_itemCount = count;
    m_selectedIndex = count > 0 ? 0 : -1;
    m_scrollOffset = 0;
    markDirty();
}

void ListView::addItem(const ListItem& item) {
    if (!ensureCapacity(m_itemCount + 1)) {
        return;
    }

    m_items[m_itemCount] = item;
    // Deep copy
    if (item.text) {
        m_items[m_itemCount].text = strdup(item.text);
    }
    if (item.subtext) {
        m_items[m_itemCount].subtext = strdup(item.subtext);
    }

    m_itemCount++;

    if (m_selectedIndex < 0 && m_itemCount == 1) {
        m_selectedIndex = 0;
    }

    markDirty();
}

void ListView::addItem(const char* text, const char* subtext, uint16_t color) {
    ListItem item(text, subtext, color);
    addItem(item);
}

void ListView::removeItem(size_t index) {
    if (index >= m_itemCount) return;

    // Free strings
    if (m_items[index].text) {
        free((void*)m_items[index].text);
    }
    if (m_items[index].subtext) {
        free((void*)m_items[index].subtext);
    }

    // Shift items
    for (size_t i = index; i < m_itemCount - 1; i++) {
        m_items[i] = m_items[i + 1];
    }

    m_itemCount--;

    // Adjust selection
    if (m_selectedIndex >= (int)m_itemCount) {
        m_selectedIndex = m_itemCount - 1;
    }

    markDirty();
}

void ListView::clearItems() {
    for (size_t i = 0; i < m_itemCount; i++) {
        if (m_items[i].text) free((void*)m_items[i].text);
        if (m_items[i].subtext) free((void*)m_items[i].subtext);
    }

    if (m_items) {
        free(m_items);
        m_items = nullptr;
    }

    m_itemCount = 0;
    m_itemCapacity = 0;
    m_selectedIndex = -1;
    m_scrollOffset = 0;
    markDirty();
}

const ListItem* ListView::itemAt(size_t index) const {
    if (index < m_itemCount) {
        return &m_items[index];
    }
    return nullptr;
}

void ListView::updateItem(size_t index, const ListItem& item) {
    if (index >= m_itemCount) return;

    // Free old strings
    if (m_items[index].text) free((void*)m_items[index].text);
    if (m_items[index].subtext) free((void*)m_items[index].subtext);

    // Copy new item
    m_items[index] = item;
    if (item.text) m_items[index].text = strdup(item.text);
    if (item.subtext) m_items[index].subtext = strdup(item.subtext);

    markDirty();
}

void ListView::setSelectedIndex(int index) {
    if (index < -1) index = -1;
    if (index >= (int)m_itemCount) index = m_itemCount - 1;

    // Skip non-selectable items
    while (index >= 0 && index < (int)m_itemCount &&
           !m_items[index].selectable) {
        index++;
    }
    if (index >= (int)m_itemCount) index = -1;

    if (m_selectedIndex != index) {
        int oldIndex = m_selectedIndex;
        m_selectedIndex = index;
        scrollToSelected();
        markDirty();
        emitSelectionChanged(oldIndex, index);
    }
}

void ListView::selectNext() {
    if (m_itemCount == 0) return;

    int newIndex = m_selectedIndex + 1;
    while (newIndex < (int)m_itemCount && !m_items[newIndex].selectable) {
        newIndex++;
    }

    if (newIndex < (int)m_itemCount) {
        setSelectedIndex(newIndex);
    }
}

void ListView::selectPrevious() {
    if (m_itemCount == 0) return;

    int newIndex = m_selectedIndex - 1;
    while (newIndex >= 0 && !m_items[newIndex].selectable) {
        newIndex--;
    }

    if (newIndex >= 0) {
        setSelectedIndex(newIndex);
    }
}

const ListItem* ListView::selectedItem() const {
    return itemAt(m_selectedIndex);
}

void ListView::setScrollOffset(int offset) {
    int maxOffset = max(0, (int)m_itemCount - visibleItemCount());
    offset = constrain(offset, 0, maxOffset);

    if (m_scrollOffset != offset) {
        m_scrollOffset = offset;
        markDirty();
    }
}

void ListView::scrollToSelected() {
    if (m_selectedIndex < 0) return;

    int visible = visibleItemCount();
    if (visible <= 0) return;

    if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = m_selectedIndex;
    } else if (m_selectedIndex >= m_scrollOffset + visible) {
        m_scrollOffset = m_selectedIndex - visible + 1;
    }
}

int ListView::visibleItemCount() const {
    Rect content = contentRect();
    return content.height / m_itemHeight;
}

bool ListView::onKeyPress(char key, uint8_t modifiers) {
    (void)modifiers;

    switch (key) {
        case ';':  // Up
            selectPrevious();
            return true;

        case '.':  // Down
            selectNext();
            return true;

        case '\n':  // Enter
        case '\r':
            if (m_selectedIndex >= 0) {
                emitItemActivated(m_selectedIndex);
            }
            return true;

        case '[':  // Page up
            setSelectedIndex(max(0, m_selectedIndex - visibleItemCount()));
            return true;

        case ']':  // Page down
            setSelectedIndex(min((int)m_itemCount - 1,
                                m_selectedIndex + visibleItemCount()));
            return true;
    }

    return Widget::onKeyPress(key, modifiers);
}

bool ListView::onKeyHold(char key, uint32_t duration, uint8_t modifiers) {
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

SlotId ListView::onSelectionChanged(SlotFunction handler) {
    return signal().connect(SignalType::SelectionChanged, handler, this);
}

SlotId ListView::onItemActivated(SlotFunction handler) {
    return signal().connect(SignalType::Click, handler, this);
}

void ListView::emitSelectionChanged(int oldIndex, int newIndex) {
    Event e(SignalType::SelectionChanged, SignalPriority::High);
    e.sender = this;
    e.data.value.oldValue = oldIndex;
    e.data.value.newValue = newIndex;
    signal().emit(e);
}

void ListView::emitItemActivated(int index) {
    Event e = Events::click();
    e.sender = this;
    e.data.value.newValue = index;
    if (index >= 0 && index < (int)m_itemCount) {
        e.data.value.stringValue = m_items[index].text;
    }
    signal().emit(e);
}

void ListView::renderContent() {
    Rect abs = absoluteBounds();
    int visible = visibleItemCount();
    bool needsScrollbar = m_showScrollbar && (int)m_itemCount > visible;

    int16_t itemWidth = abs.width - style().padding.horizontal();
    if (needsScrollbar) {
        itemWidth -= m_scrollbarWidth;
    }

    // Draw items
    for (int i = 0; i < visible && (m_scrollOffset + i) < (int)m_itemCount; i++) {
        int itemIndex = m_scrollOffset + i;
        const ListItem& item = m_items[itemIndex];

        Rect itemBounds(
            abs.x + style().padding.left,
            abs.y + style().padding.top + i * m_itemHeight,
            itemWidth,
            m_itemHeight
        );

        bool selected = (itemIndex == m_selectedIndex);
        bool focused = isFocused();

        if (m_itemRenderer) {
            m_itemRenderer(itemIndex, item, itemBounds, selected, focused);
        } else {
            renderItem(itemIndex, item, itemBounds, selected, focused);
        }
    }

    // Draw scrollbar
    if (needsScrollbar) {
        renderScrollbar();
    }
}

void ListView::renderItem(int index, const ListItem& item,
                          const Rect& bounds, bool selected, bool focused) {
    (void)index;

    // Background
    if (selected) {
        Draw::fillRect(bounds.x, bounds.y, bounds.width, bounds.height,
                      focused ? ContainerColors::ItemSelected :
                               ContainerColors::ItemHighlight);
    }

    // Text
    uint16_t textColor = item.textColor != 0 ? item.textColor :
                         effectiveForegroundColor();

    if (!item.enabled) {
        textColor = LabelColors::Muted;
    }

    int16_t textX = bounds.x + 2;
    int16_t textY = bounds.y + (bounds.height - 8) / 2;

    if (item.text) {
        Draw::drawText(textX, textY, item.text, textColor, style().textSize);
    }

    // Subtext (if present and fits)
    if (item.subtext) {
        int16_t subtextX = bounds.right() - strlen(item.subtext) * 6 - 2;
        if (subtextX > textX + 50) {
            Draw::drawText(subtextX, textY, item.subtext,
                          LabelColors::Muted, style().textSize);
        }
    }
}

void ListView::renderScrollbar() {
    Rect abs = absoluteBounds();
    int visible = visibleItemCount();

    if (m_itemCount <= (size_t)visible) return;

    int16_t trackX = abs.right() - m_scrollbarWidth - 1;
    int16_t trackY = abs.y + style().padding.top;
    int16_t trackH = abs.height - style().padding.vertical();

    // Track
    Draw::fillRect(trackX, trackY, m_scrollbarWidth, trackH,
                  ContainerColors::ScrollbarTrack);

    // Thumb
    float visibleRatio = (float)visible / m_itemCount;
    float scrollRatio = (float)m_scrollOffset / (m_itemCount - visible);

    int16_t thumbH = max((int16_t)8, (int16_t)(trackH * visibleRatio));
    int16_t thumbY = trackY + (int16_t)((trackH - thumbH) * scrollRatio);

    Draw::fillRect(trackX, thumbY, m_scrollbarWidth - 1, thumbH,
                  ContainerColors::ScrollbarThumb);
}

} // namespace GUI
