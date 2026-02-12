/**
 * @file gui_container.h
 * @brief Container widgets (Container, ScrollView, ListView)
 *
 * Container: Basic container for grouping widgets
 * ScrollView: Scrollable container for content larger than viewport
 * ListView: Optimized list for many items (virtual scrolling)
 */

#ifndef GUI_CONTAINER_H
#define GUI_CONTAINER_H

#include "gui_widget.h"
#include "gui_label.h"

namespace GUI {

//=============================================================================
// Layout Direction
//=============================================================================

enum class LayoutDirection : uint8_t {
    Vertical,       // Children stacked vertically
    Horizontal,     // Children laid out horizontally
    None            // No auto-layout (manual positioning)
};

//=============================================================================
// Container Widget
//=============================================================================

/**
 * @brief Basic container for grouping widgets
 */
class Container : public Widget {
public:
    explicit Container(WidgetId id = INVALID_WIDGET_ID);
    virtual ~Container() = default;

    //=========================================================================
    // Layout
    //=========================================================================

    void setLayoutDirection(LayoutDirection dir) {
        m_layoutDirection = dir;
        markDirty(DirtyFlag::Layout);
    }

    LayoutDirection layoutDirection() const { return m_layoutDirection; }

    /**
     * @brief Set spacing between children
     */
    void setSpacing(int8_t spacing) {
        m_spacing = spacing;
        markDirty(DirtyFlag::Layout);
    }

    int8_t spacing() const { return m_spacing; }

    /**
     * @brief Layout children according to direction
     */
    void layout() override;

    void renderContent() override;

protected:
    LayoutDirection m_layoutDirection;
    int8_t m_spacing;
};

//=============================================================================
// ScrollView Widget
//=============================================================================

/**
 * @brief Scrollable container for content larger than viewport
 */
class ScrollView : public Container {
public:
    explicit ScrollView(WidgetId id = INVALID_WIDGET_ID);
    virtual ~ScrollView() = default;

    //=========================================================================
    // Scroll
    //=========================================================================

    /**
     * @brief Get current scroll position
     */
    int16_t scrollX() const { return m_scrollX; }
    int16_t scrollY() const { return m_scrollY; }

    /**
     * @brief Set scroll position
     */
    void setScrollPosition(int16_t x, int16_t y);

    /**
     * @brief Scroll by delta
     */
    void scrollBy(int16_t dx, int16_t dy);

    /**
     * @brief Scroll to make widget visible
     */
    void scrollToWidget(Widget* widget);

    /**
     * @brief Scroll to top
     */
    void scrollToTop();

    /**
     * @brief Scroll to bottom
     */
    void scrollToBottom();

    /**
     * @brief Get content size
     */
    int16_t contentWidth() const { return m_contentWidth; }
    int16_t contentHeight() const { return m_contentHeight; }

    /**
     * @brief Set content size (for virtual scrolling)
     */
    void setContentSize(int16_t w, int16_t h);

    /**
     * @brief Check if scrollbars should be shown
     */
    bool canScrollHorizontally() const;
    bool canScrollVertically() const;

    //=========================================================================
    // Scrollbar
    //=========================================================================

    void setShowScrollbar(bool show) {
        m_showScrollbar = show;
        markDirty();
    }

    bool showScrollbar() const { return m_showScrollbar; }

    void setScrollbarWidth(uint8_t width) {
        m_scrollbarWidth = width;
        markDirty();
    }

    //=========================================================================
    // Event Handling
    //=========================================================================

    bool onKeyPress(char key, uint8_t modifiers) override;

    //=========================================================================
    // Rendering
    //=========================================================================

    void renderContent() override;
    void renderScrollbar();

    void layout() override;

protected:
    /**
     * @brief Calculate content size from children
     */
    void updateContentSize();

    /**
     * @brief Clamp scroll position to valid range
     */
    void clampScrollPosition();

private:
    int16_t m_scrollX;
    int16_t m_scrollY;
    int16_t m_contentWidth;
    int16_t m_contentHeight;
    bool m_showScrollbar;
    uint8_t m_scrollbarWidth;
};

//=============================================================================
// ListView Item
//=============================================================================

/**
 * @brief Data for a list item
 */
struct ListItem {
    const char* text;           // Primary text
    const char* subtext;        // Secondary text (optional)
    uint16_t textColor;         // Custom text color (0 = default)
    void* userData;             // User data pointer
    bool selectable;            // Can be selected
    bool enabled;               // Is enabled

    ListItem()
        : text(nullptr)
        , subtext(nullptr)
        , textColor(0)
        , userData(nullptr)
        , selectable(true)
        , enabled(true) {}

    ListItem(const char* t, const char* sub = nullptr, uint16_t color = 0)
        : text(t)
        , subtext(sub)
        , textColor(color)
        , userData(nullptr)
        , selectable(true)
        , enabled(true) {}
};

/**
 * @brief List item renderer callback
 */
using ListItemRenderer = std::function<void(int index, const ListItem& item,
                                            const Rect& bounds, bool selected,
                                            bool focused)>;

//=============================================================================
// ListView Widget
//=============================================================================

/**
 * @brief Optimized list widget with virtual scrolling
 *
 * Uses lazy rendering - only renders visible items.
 * Supports custom item renderers for complex items.
 */
class ListView : public Widget {
public:
    explicit ListView(WidgetId id = INVALID_WIDGET_ID);
    virtual ~ListView();

    //=========================================================================
    // Items
    //=========================================================================

    /**
     * @brief Set items (copies array)
     */
    void setItems(const ListItem* items, size_t count);

    /**
     * @brief Add single item
     */
    void addItem(const ListItem& item);

    /**
     * @brief Add item with just text
     */
    void addItem(const char* text, const char* subtext = nullptr,
                 uint16_t color = 0);

    /**
     * @brief Remove item at index
     */
    void removeItem(size_t index);

    /**
     * @brief Clear all items
     */
    void clearItems();

    /**
     * @brief Get item count
     */
    size_t itemCount() const { return m_itemCount; }

    /**
     * @brief Get item at index
     */
    const ListItem* itemAt(size_t index) const;

    /**
     * @brief Update item at index
     */
    void updateItem(size_t index, const ListItem& item);

    //=========================================================================
    // Selection
    //=========================================================================

    /**
     * @brief Get selected index (-1 if none)
     */
    int selectedIndex() const { return m_selectedIndex; }

    /**
     * @brief Set selected index
     */
    void setSelectedIndex(int index);

    /**
     * @brief Select next item
     */
    void selectNext();

    /**
     * @brief Select previous item
     */
    void selectPrevious();

    /**
     * @brief Get selected item
     */
    const ListItem* selectedItem() const;

    //=========================================================================
    // Item Properties
    //=========================================================================

    /**
     * @brief Set item height
     */
    void setItemHeight(int16_t height) {
        m_itemHeight = height;
        markDirty(DirtyFlag::Layout);
    }

    int16_t itemHeight() const { return m_itemHeight; }

    /**
     * @brief Set custom item renderer
     */
    void setItemRenderer(ListItemRenderer renderer) {
        m_itemRenderer = renderer;
    }

    //=========================================================================
    // Scrolling
    //=========================================================================

    /**
     * @brief Get scroll offset (in items)
     */
    int scrollOffset() const { return m_scrollOffset; }

    /**
     * @brief Set scroll offset
     */
    void setScrollOffset(int offset);

    /**
     * @brief Ensure selected item is visible
     */
    void scrollToSelected();

    /**
     * @brief Get number of visible items
     */
    int visibleItemCount() const;

    //=========================================================================
    // Scrollbar
    //=========================================================================

    void setShowScrollbar(bool show) {
        m_showScrollbar = show;
        markDirty();
    }

    bool showScrollbar() const { return m_showScrollbar; }

    //=========================================================================
    // Event Handling
    //=========================================================================

    bool onKeyPress(char key, uint8_t modifiers) override;
    bool onKeyHold(char key, uint32_t duration, uint8_t modifiers) override;

    //=========================================================================
    // Signals
    //=========================================================================

    SlotId onSelectionChanged(SlotFunction handler);
    SlotId onItemActivated(SlotFunction handler);

    //=========================================================================
    // Rendering
    //=========================================================================

    void renderContent() override;

protected:
    /**
     * @brief Render single item
     */
    virtual void renderItem(int index, const ListItem& item,
                            const Rect& bounds, bool selected, bool focused);

    /**
     * @brief Render scrollbar
     */
    void renderScrollbar();

    /**
     * @brief Emit selection changed
     */
    void emitSelectionChanged(int oldIndex, int newIndex);

    /**
     * @brief Emit item activated (Enter pressed)
     */
    void emitItemActivated(int index);

    /**
     * @brief Ensure capacity for items
     */
    bool ensureCapacity(size_t required);

private:
    // Items (dynamically allocated)
    ListItem* m_items;
    size_t m_itemCount;
    size_t m_itemCapacity;

    // Selection
    int m_selectedIndex;

    // Scrolling
    int m_scrollOffset;

    // Item properties
    int16_t m_itemHeight;
    ListItemRenderer m_itemRenderer;

    // Scrollbar
    bool m_showScrollbar;
    uint8_t m_scrollbarWidth;
};

//=============================================================================
// Container Colors
//=============================================================================

namespace ContainerColors {
    constexpr uint16_t ScrollbarTrack = 0x2104;   // Dark gray
    constexpr uint16_t ScrollbarThumb = 0x7BEF;   // Light gray
    constexpr uint16_t ItemBackground = 0x0000;   // Black
    constexpr uint16_t ItemSelected   = 0x000F;   // Dark blue
    constexpr uint16_t ItemHighlight  = 0x2104;   // Hover gray
}

} // namespace GUI

#endif // GUI_CONTAINER_H
