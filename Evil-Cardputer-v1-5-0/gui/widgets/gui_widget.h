/**
 * @file gui_widget.h
 * @brief Base widget class with lazy rendering and Signal/Slot support
 *
 * Features:
 * - Dirty flag system for lazy rendering (only redraw when changed)
 * - Dynamic memory allocation for efficiency
 * - Signal/Slot integration for event handling
 * - Adaptive display support (resolution queries, scaling)
 * - Hierarchical widget tree (parent/children)
 */

#ifndef GUI_WIDGET_H
#define GUI_WIDGET_H

#include <Arduino.h>
#include <vector>
#include <memory>
#include "gui_signal.h"
#include "../gui_types.h"
#include "../gui_theme.h"

namespace GUI {

// Forward declarations
class WidgetManager;
class Widget;

//=============================================================================
// Widget ID and Types
//=============================================================================

/**
 * @brief Unique widget identifier
 */
using WidgetId = uint16_t;
constexpr WidgetId INVALID_WIDGET_ID = 0;

/**
 * @brief Widget type enumeration
 */
enum class WidgetType : uint8_t {
    Base,               // Base widget (container only)
    Label,              // Static or dynamic text
    Button,             // Clickable button
    Input,              // Text input field
    Checkbox,           // Toggle checkbox
    RadioButton,        // Radio button (single selection)
    Slider,             // Value slider
    ProgressBar,        // Progress indicator
    StatusIndicator,    // Status light/icon
    Container,          // Generic container
    ScrollView,         // Scrollable container
    ListView,           // List of items
    MenuItem,           // Menu item
    Scrollbar,          // Scrollbar
    Divider,            // Horizontal/vertical divider
    Image,              // Image/bitmap display
    Custom              // User-defined widget
};

/**
 * @brief Widget variant for adaptive display
 */
enum class WidgetVariant : uint8_t {
    Full,               // Full-size variant
    Compact,            // Compact variant
    Minimal,            // Minimal variant (smallest)
    Icon,               // Icon-only variant
    Auto                // Auto-select based on display
};

/**
 * @brief Widget state flags
 */
enum class WidgetState : uint8_t {
    Normal      = 0x00,
    Focused     = 0x01,
    Pressed     = 0x02,
    Disabled    = 0x04,
    Hidden      = 0x08,
    Selected    = 0x10,
    Error       = 0x20,
    Highlight   = 0x40
};

// Allow bitwise operations on WidgetState
inline WidgetState operator|(WidgetState a, WidgetState b) {
    return static_cast<WidgetState>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline WidgetState operator&(WidgetState a, WidgetState b) {
    return static_cast<WidgetState>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline WidgetState operator~(WidgetState a) {
    return static_cast<WidgetState>(~static_cast<uint8_t>(a));
}

//=============================================================================
// Geometry Structures
//=============================================================================

/**
 * @brief Rectangle structure
 */
struct Rect {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;

    Rect() : x(0), y(0), width(0), height(0) {}
    Rect(int16_t x_, int16_t y_, int16_t w_, int16_t h_)
        : x(x_), y(y_), width(w_), height(h_) {}

    bool contains(int16_t px, int16_t py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }

    bool intersects(const Rect& other) const {
        return !(x + width <= other.x || other.x + other.width <= x ||
                 y + height <= other.y || other.y + other.height <= y);
    }

    Rect intersection(const Rect& other) const {
        int16_t nx = max(x, other.x);
        int16_t ny = max(y, other.y);
        int16_t nx2 = min(x + width, other.x + other.width);
        int16_t ny2 = min(y + height, other.y + other.height);
        if (nx2 > nx && ny2 > ny) {
            return Rect(nx, ny, nx2 - nx, ny2 - ny);
        }
        return Rect();
    }

    Rect united(const Rect& other) const {
        if (width == 0 || height == 0) return other;
        if (other.width == 0 || other.height == 0) return *this;
        int16_t nx = min(x, other.x);
        int16_t ny = min(y, other.y);
        int16_t nx2 = max(x + width, other.x + other.width);
        int16_t ny2 = max(y + height, other.y + other.height);
        return Rect(nx, ny, nx2 - nx, ny2 - ny);
    }

    bool isEmpty() const { return width <= 0 || height <= 0; }

    int16_t right() const { return x + width; }
    int16_t bottom() const { return y + height; }
    int16_t centerX() const { return x + width / 2; }
    int16_t centerY() const { return y + height / 2; }
};

/**
 * @brief Padding/Margin structure
 */
struct Insets {
    int8_t top;
    int8_t right;
    int8_t bottom;
    int8_t left;

    Insets() : top(0), right(0), bottom(0), left(0) {}
    Insets(int8_t all) : top(all), right(all), bottom(all), left(all) {}
    Insets(int8_t v, int8_t h) : top(v), right(h), bottom(v), left(h) {}
    Insets(int8_t t, int8_t r, int8_t b, int8_t l)
        : top(t), right(r), bottom(b), left(l) {}

    int16_t horizontal() const { return left + right; }
    int16_t vertical() const { return top + bottom; }
};

/**
 * @brief Size constraint
 */
struct SizeConstraint {
    int16_t minWidth;
    int16_t minHeight;
    int16_t maxWidth;
    int16_t maxHeight;
    int16_t preferredWidth;
    int16_t preferredHeight;

    SizeConstraint()
        : minWidth(0), minHeight(0)
        , maxWidth(32767), maxHeight(32767)
        , preferredWidth(-1), preferredHeight(-1) {}

    int16_t clampWidth(int16_t w) const {
        return constrain(w, minWidth, maxWidth);
    }

    int16_t clampHeight(int16_t h) const {
        return constrain(h, minHeight, maxHeight);
    }
};

//=============================================================================
// Dirty Region Tracking
//=============================================================================

/**
 * @brief Dirty flag bits
 */
enum class DirtyFlag : uint8_t {
    None        = 0x00,
    Content     = 0x01,     // Content changed, needs redraw
    Layout      = 0x02,     // Layout changed, needs recalculation
    Style       = 0x04,     // Style changed (colors, font)
    Children    = 0x08,     // Children changed
    All         = 0x0F      // Everything dirty
};

inline DirtyFlag operator|(DirtyFlag a, DirtyFlag b) {
    return static_cast<DirtyFlag>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline DirtyFlag operator&(DirtyFlag a, DirtyFlag b) {
    return static_cast<DirtyFlag>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool hasDirtyFlag(DirtyFlag flags, DirtyFlag test) {
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(test)) != 0;
}

/**
 * @brief Dirty region tracker
 */
class DirtyTracker {
public:
    DirtyTracker() : m_flags(DirtyFlag::All) {}

    void markDirty(DirtyFlag flag = DirtyFlag::Content) {
        m_flags = m_flags | flag;
        m_dirtyRegion = m_bounds;
    }

    void markDirty(const Rect& region) {
        m_flags = m_flags | DirtyFlag::Content;
        if (m_dirtyRegion.isEmpty()) {
            m_dirtyRegion = region;
        } else {
            m_dirtyRegion = m_dirtyRegion.united(region);
        }
    }

    void markClean() {
        m_flags = DirtyFlag::None;
        m_dirtyRegion = Rect();
    }

    bool isDirty() const {
        return m_flags != DirtyFlag::None;
    }

    bool isDirty(DirtyFlag flag) const {
        return hasDirtyFlag(m_flags, flag);
    }

    DirtyFlag flags() const { return m_flags; }
    const Rect& dirtyRegion() const { return m_dirtyRegion; }

    void setBounds(const Rect& bounds) {
        m_bounds = bounds;
    }

private:
    DirtyFlag m_flags;
    Rect m_bounds;
    Rect m_dirtyRegion;
};

//=============================================================================
// Widget Style
//=============================================================================

/**
 * @brief Widget style configuration
 */
struct WidgetStyle {
    // Colors (0 = use theme default)
    uint16_t backgroundColor;
    uint16_t foregroundColor;
    uint16_t borderColor;
    uint16_t focusColor;
    uint16_t disabledColor;

    // Spacing
    Insets padding;
    Insets margin;

    // Border
    uint8_t borderWidth;
    uint8_t borderRadius;

    // Text
    uint8_t textSize;
    uint8_t textAlign;  // 0=left, 1=center, 2=right

    // Flags
    bool opaque;        // Draw background
    bool focusable;     // Can receive focus
    bool visible;       // Is visible

    WidgetStyle()
        : backgroundColor(0)
        , foregroundColor(0)
        , borderColor(0)
        , focusColor(0)
        , disabledColor(0)
        , borderWidth(0)
        , borderRadius(0)
        , textSize(1)
        , textAlign(0)
        , opaque(true)
        , focusable(false)
        , visible(true) {}
};

//=============================================================================
// Base Widget Class
//=============================================================================

/**
 * @brief Base widget class
 *
 * All UI elements inherit from this class. Provides:
 * - Geometry management (bounds, constraints)
 * - Dirty tracking for lazy rendering
 * - Signal/Slot integration
 * - Hierarchical structure (parent/children)
 * - State management
 */
class Widget {
public:
    /**
     * @brief Constructor
     * @param type Widget type
     * @param id Unique ID (0 = auto-assign)
     */
    Widget(WidgetType type = WidgetType::Base, WidgetId id = INVALID_WIDGET_ID);

    /**
     * @brief Virtual destructor
     */
    virtual ~Widget();

    // Non-copyable
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    //=========================================================================
    // Identity
    //=========================================================================

    WidgetId id() const { return m_id; }
    WidgetType type() const { return m_type; }
    WidgetVariant variant() const { return m_variant; }
    void setVariant(WidgetVariant v) { m_variant = v; markDirty(); }

    const char* name() const { return m_name; }
    void setName(const char* name);

    //=========================================================================
    // Geometry
    //=========================================================================

    const Rect& bounds() const { return m_bounds; }
    void setBounds(const Rect& rect);
    void setBounds(int16_t x, int16_t y, int16_t w, int16_t h);

    void setPosition(int16_t x, int16_t y);
    void setSize(int16_t w, int16_t h);

    int16_t x() const { return m_bounds.x; }
    int16_t y() const { return m_bounds.y; }
    int16_t width() const { return m_bounds.width; }
    int16_t height() const { return m_bounds.height; }

    // Get absolute position (relative to screen)
    Rect absoluteBounds() const;
    int16_t absoluteX() const;
    int16_t absoluteY() const;

    const SizeConstraint& sizeConstraint() const { return m_sizeConstraint; }
    void setSizeConstraint(const SizeConstraint& c);
    void setMinSize(int16_t w, int16_t h);
    void setMaxSize(int16_t w, int16_t h);
    void setPreferredSize(int16_t w, int16_t h);

    //=========================================================================
    // Style
    //=========================================================================

    WidgetStyle& style() { return m_style; }
    const WidgetStyle& style() const { return m_style; }
    void setStyle(const WidgetStyle& style);

    void setBackgroundColor(uint16_t color);
    void setForegroundColor(uint16_t color);
    void setBorderColor(uint16_t color);
    void setPadding(const Insets& padding);
    void setMargin(const Insets& margin);
    void setTextSize(uint8_t size);
    void setTextAlign(uint8_t align);
    void setOpaque(bool opaque);
    void setFocusable(bool focusable);

    // Get effective colors (from style or theme)
    uint16_t effectiveBackgroundColor() const;
    uint16_t effectiveForegroundColor() const;
    uint16_t effectiveBorderColor() const;

    //=========================================================================
    // State
    //=========================================================================

    WidgetState state() const { return m_state; }
    void setState(WidgetState state);
    void addState(WidgetState state);
    void removeState(WidgetState state);
    bool hasState(WidgetState state) const;

    bool isVisible() const { return !hasState(WidgetState::Hidden); }
    void setVisible(bool visible);

    bool isEnabled() const { return !hasState(WidgetState::Disabled); }
    void setEnabled(bool enabled);

    bool isFocused() const { return hasState(WidgetState::Focused); }
    void setFocused(bool focused);

    bool isSelected() const { return hasState(WidgetState::Selected); }
    void setSelected(bool selected);

    bool isFocusable() const { return m_style.focusable && isEnabled(); }

    //=========================================================================
    // Dirty Tracking (Lazy Rendering)
    //=========================================================================

    bool isDirty() const { return m_dirty.isDirty(); }
    bool isDirty(DirtyFlag flag) const { return m_dirty.isDirty(flag); }
    DirtyFlag dirtyFlags() const { return m_dirty.flags(); }
    const Rect& dirtyRegion() const { return m_dirty.dirtyRegion(); }

    void markDirty(DirtyFlag flag = DirtyFlag::Content);
    void markDirty(const Rect& region);
    void markClean();

    // Called after rendering to clear dirty state
    void onRendered();

    //=========================================================================
    // Hierarchy
    //=========================================================================

    Widget* parent() const { return m_parent; }
    const std::vector<Widget*>& children() const { return m_children; }
    size_t childCount() const { return m_children.size(); }

    void addChild(Widget* child);
    void removeChild(Widget* child);
    void removeAllChildren();
    Widget* childAt(size_t index) const;
    Widget* findChild(WidgetId id) const;
    Widget* findChild(const char* name) const;

    // Get root widget
    Widget* root();
    const Widget* root() const;

    //=========================================================================
    // Signals
    //=========================================================================

    Signal& signal() { return m_signal; }
    const Signal& signal() const { return m_signal; }

    // Convenience methods for common signals
    SlotId onValueChanged(SlotFunction handler);
    SlotId onClick(SlotFunction handler);
    SlotId onFocus(SlotFunction handler);
    SlotId onBlur(SlotFunction handler);
    SlotId onKeyPress(SlotFunction handler);

    // Emit signals
    void emitValueChanged(int32_t oldVal, int32_t newVal, const char* str = nullptr);
    void emitClick();
    void emitRender();
    void emitInvalidate();
    void emitInvalidate(const Rect& region);

    //=========================================================================
    // Event Handling
    //=========================================================================

    /**
     * @brief Handle an event
     * @param event Event to handle
     * @return true if event was consumed
     */
    virtual bool handleEvent(Event& event);

    /**
     * @brief Handle key press
     * @param key Key code
     * @param modifiers Modifier flags
     * @return true if handled
     */
    virtual bool onKeyPress(char key, uint8_t modifiers);

    /**
     * @brief Handle key hold (repeat)
     */
    virtual bool onKeyHold(char key, uint32_t duration, uint8_t modifiers);

    /**
     * @brief Handle focus gained
     */
    virtual void onFocusGained();

    /**
     * @brief Handle focus lost
     */
    virtual void onFocusLost();

    //=========================================================================
    // Rendering
    //=========================================================================

    /**
     * @brief Render the widget
     *
     * This is called only when the widget is dirty. Override to implement
     * custom rendering. The base implementation:
     * 1. Draws background if opaque
     * 2. Calls renderContent()
     * 3. Draws border if borderWidth > 0
     * 4. Renders children
     */
    virtual void render();

    /**
     * @brief Render widget content
     *
     * Override this to render the actual widget content (text, graphics, etc.)
     * Called by render() after background is drawn.
     */
    virtual void renderContent();

    /**
     * @brief Render children
     */
    virtual void renderChildren();

    /**
     * @brief Get content rect (bounds minus padding)
     */
    Rect contentRect() const;

    //=========================================================================
    // Layout
    //=========================================================================

    /**
     * @brief Calculate preferred size
     */
    virtual void measure(int16_t availableWidth, int16_t availableHeight);

    /**
     * @brief Layout children
     */
    virtual void layout();

    /**
     * @brief Get measured width
     */
    int16_t measuredWidth() const { return m_measuredWidth; }

    /**
     * @brief Get measured height
     */
    int16_t measuredHeight() const { return m_measuredHeight; }

    //=========================================================================
    // Display Adaptation
    //=========================================================================

    /**
     * @brief Adapt to display parameters
     * @param displayWidth Display width
     * @param displayHeight Display height
     * @param scaleFactor Scale factor
     */
    virtual void adaptToDisplay(int16_t displayWidth, int16_t displayHeight,
                                float scaleFactor);

    /**
     * @brief Select appropriate variant for display size
     */
    virtual WidgetVariant selectVariant(int16_t displayWidth,
                                        int16_t displayHeight);

protected:
    // Set parent (called by addChild/removeChild)
    void setParent(Widget* parent);

    // Mark children dirty
    void markChildrenDirty();

    // Update measured size
    void setMeasuredSize(int16_t w, int16_t h);

    // Static ID generator
    static WidgetId s_nextId;

private:
    // Identity
    WidgetId m_id;
    WidgetType m_type;
    WidgetVariant m_variant;
    char* m_name;

    // Geometry
    Rect m_bounds;
    SizeConstraint m_sizeConstraint;
    int16_t m_measuredWidth;
    int16_t m_measuredHeight;

    // Style & State
    WidgetStyle m_style;
    WidgetState m_state;

    // Dirty tracking
    DirtyTracker m_dirty;

    // Hierarchy
    Widget* m_parent;
    std::vector<Widget*> m_children;

    // Signals
    Signal m_signal;
};

//=============================================================================
// Smart Pointer Alias
//=============================================================================

using WidgetPtr = std::unique_ptr<Widget>;

} // namespace GUI

#endif // GUI_WIDGET_H
