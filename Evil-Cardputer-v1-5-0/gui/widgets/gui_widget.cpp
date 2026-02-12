/**
 * @file gui_widget.cpp
 * @brief Base widget class implementation
 */

#include "gui_widget.h"
#include "gui_draw.h"

namespace GUI {

// Static ID generator
WidgetId Widget::s_nextId = 1;

//=============================================================================
// Constructor / Destructor
//=============================================================================

Widget::Widget(WidgetType type, WidgetId id)
    : m_id(id == INVALID_WIDGET_ID ? s_nextId++ : id)
    , m_type(type)
    , m_variant(WidgetVariant::Auto)
    , m_name(nullptr)
    , m_measuredWidth(0)
    , m_measuredHeight(0)
    , m_state(WidgetState::Normal)
    , m_parent(nullptr)
{
    m_dirty.markDirty(DirtyFlag::All);
}

Widget::~Widget() {
    // Disconnect all signal connections for this widget
    m_signal.disconnectAll();

    // Remove from parent
    if (m_parent) {
        m_parent->removeChild(this);
    }

    // Delete children (they will remove themselves from our list)
    while (!m_children.empty()) {
        delete m_children.back();
    }

    // Free name
    if (m_name) {
        free(m_name);
    }
}

//=============================================================================
// Identity
//=============================================================================

void Widget::setName(const char* name) {
    if (m_name) {
        free(m_name);
        m_name = nullptr;
    }
    if (name) {
        m_name = strdup(name);
    }
}

//=============================================================================
// Geometry
//=============================================================================

void Widget::setBounds(const Rect& rect) {
    if (m_bounds.x != rect.x || m_bounds.y != rect.y ||
        m_bounds.width != rect.width || m_bounds.height != rect.height) {
        m_bounds = rect;
        m_dirty.setBounds(rect);
        markDirty(DirtyFlag::Layout | DirtyFlag::Content);
    }
}

void Widget::setBounds(int16_t x, int16_t y, int16_t w, int16_t h) {
    setBounds(Rect(x, y, w, h));
}

void Widget::setPosition(int16_t x, int16_t y) {
    if (m_bounds.x != x || m_bounds.y != y) {
        m_bounds.x = x;
        m_bounds.y = y;
        markDirty(DirtyFlag::Layout);
    }
}

void Widget::setSize(int16_t w, int16_t h) {
    w = m_sizeConstraint.clampWidth(w);
    h = m_sizeConstraint.clampHeight(h);
    uint16_t uw = w > 0 ? static_cast<uint16_t>(w) : 0;
    uint16_t uh = h > 0 ? static_cast<uint16_t>(h) : 0;
    if (m_bounds.width != uw || m_bounds.height != uh) {
        m_bounds.width = uw;
        m_bounds.height = uh;
        m_dirty.setBounds(m_bounds);
        markDirty(DirtyFlag::Layout | DirtyFlag::Content);
    }
}

Rect Widget::absoluteBounds() const {
    Rect abs = m_bounds;
    const Widget* p = m_parent;
    while (p) {
        abs.x += p->m_bounds.x + p->m_style.padding.left;
        abs.y += p->m_bounds.y + p->m_style.padding.top;
        p = p->m_parent;
    }
    return abs;
}

int16_t Widget::absoluteX() const {
    int16_t ax = m_bounds.x;
    const Widget* p = m_parent;
    while (p) {
        ax += p->m_bounds.x + p->m_style.padding.left;
        p = p->m_parent;
    }
    return ax;
}

int16_t Widget::absoluteY() const {
    int16_t ay = m_bounds.y;
    const Widget* p = m_parent;
    while (p) {
        ay += p->m_bounds.y + p->m_style.padding.top;
        p = p->m_parent;
    }
    return ay;
}

void Widget::setSizeConstraint(const SizeConstraint& c) {
    m_sizeConstraint = c;
    markDirty(DirtyFlag::Layout);
}

void Widget::setMinSize(int16_t w, int16_t h) {
    m_sizeConstraint.minWidth = w;
    m_sizeConstraint.minHeight = h;
    markDirty(DirtyFlag::Layout);
}

void Widget::setMaxSize(int16_t w, int16_t h) {
    m_sizeConstraint.maxWidth = w;
    m_sizeConstraint.maxHeight = h;
    markDirty(DirtyFlag::Layout);
}

void Widget::setPreferredSize(int16_t w, int16_t h) {
    m_sizeConstraint.preferredWidth = w;
    m_sizeConstraint.preferredHeight = h;
    markDirty(DirtyFlag::Layout);
}

//=============================================================================
// Style
//=============================================================================

void Widget::setStyle(const WidgetStyle& style) {
    m_style = style;
    markDirty(DirtyFlag::Style | DirtyFlag::Content);
}

void Widget::setBackgroundColor(uint16_t color) {
    if (m_style.backgroundColor != color) {
        m_style.backgroundColor = color;
        markDirty(DirtyFlag::Style);
    }
}

void Widget::setForegroundColor(uint16_t color) {
    if (m_style.foregroundColor != color) {
        m_style.foregroundColor = color;
        markDirty(DirtyFlag::Style);
    }
}

void Widget::setBorderColor(uint16_t color) {
    if (m_style.borderColor != color) {
        m_style.borderColor = color;
        markDirty(DirtyFlag::Style);
    }
}

void Widget::setPadding(const Insets& padding) {
    m_style.padding = padding;
    markDirty(DirtyFlag::Layout);
}

void Widget::setMargin(const Insets& margin) {
    m_style.margin = margin;
    markDirty(DirtyFlag::Layout);
}

void Widget::setTextSize(uint8_t size) {
    if (m_style.textSize != size) {
        m_style.textSize = size;
        markDirty(DirtyFlag::Content);
    }
}

void Widget::setTextAlign(uint8_t align) {
    if (m_style.textAlign != align) {
        m_style.textAlign = align;
        markDirty(DirtyFlag::Content);
    }
}

void Widget::setOpaque(bool opaque) {
    if (m_style.opaque != opaque) {
        m_style.opaque = opaque;
        markDirty(DirtyFlag::Content);
    }
}

void Widget::setFocusable(bool focusable) {
    m_style.focusable = focusable;
}

uint16_t Widget::effectiveBackgroundColor() const {
    if (m_style.backgroundColor != 0) {
        return m_style.backgroundColor;
    }
    // Get from theme
    return ThemeManager::instance().theme().menuBackgroundColor();
}

uint16_t Widget::effectiveForegroundColor() const {
    if (m_style.foregroundColor != 0) {
        return m_style.foregroundColor;
    }
    // Get from theme based on state
    const auto& theme = ThemeManager::instance().theme();
    if (hasState(WidgetState::Focused)) {
        return theme.menuTextFocusedColor();
    }
    return theme.menuTextUnFocusedColor();
}

uint16_t Widget::effectiveBorderColor() const {
    if (m_style.borderColor != 0) {
        return m_style.borderColor;
    }
    return ThemeManager::instance().theme().taskbarDividerColor();
}

//=============================================================================
// State
//=============================================================================

void Widget::setState(WidgetState state) {
    if (m_state != state) {
        m_state = state;
        markDirty(DirtyFlag::Style);
    }
}

void Widget::addState(WidgetState state) {
    WidgetState newState = m_state | state;
    if (m_state != newState) {
        m_state = newState;
        markDirty(DirtyFlag::Style);
    }
}

void Widget::removeState(WidgetState state) {
    WidgetState newState = m_state & ~state;
    if (m_state != newState) {
        m_state = newState;
        markDirty(DirtyFlag::Style);
    }
}

bool Widget::hasState(WidgetState state) const {
    return (static_cast<uint8_t>(m_state) & static_cast<uint8_t>(state)) != 0;
}

void Widget::setVisible(bool visible) {
    if (visible) {
        removeState(WidgetState::Hidden);
    } else {
        addState(WidgetState::Hidden);
    }
}

void Widget::setEnabled(bool enabled) {
    if (enabled) {
        removeState(WidgetState::Disabled);
    } else {
        addState(WidgetState::Disabled);
        // Remove focus if disabled
        if (isFocused()) {
            setFocused(false);
        }
    }
}

void Widget::setFocused(bool focused) {
    if (focused && !isFocusable()) {
        return;  // Can't focus non-focusable widget
    }

    bool wasFocused = isFocused();
    if (focused) {
        addState(WidgetState::Focused);
        if (!wasFocused) {
            onFocusGained();
            Event e = Events::focus();
            e.sender = this;
            m_signal.emit(e);
        }
    } else {
        removeState(WidgetState::Focused);
        if (wasFocused) {
            onFocusLost();
            Event e = Events::blur();
            e.sender = this;
            m_signal.emit(e);
        }
    }
}

void Widget::setSelected(bool selected) {
    if (selected) {
        addState(WidgetState::Selected);
    } else {
        removeState(WidgetState::Selected);
    }
}

//=============================================================================
// Dirty Tracking
//=============================================================================

void Widget::markDirty(DirtyFlag flag) {
    m_dirty.markDirty(flag);

    // Propagate to parent if needed
    if (m_parent && hasDirtyFlag(flag, DirtyFlag::Layout)) {
        m_parent->markDirty(DirtyFlag::Children);
    }
}

void Widget::markDirty(const Rect& region) {
    m_dirty.markDirty(region);
}

void Widget::markClean() {
    m_dirty.markClean();
}

void Widget::onRendered() {
    markClean();
}

void Widget::markChildrenDirty() {
    for (Widget* child : m_children) {
        child->markDirty(DirtyFlag::All);
    }
}

//=============================================================================
// Hierarchy
//=============================================================================

void Widget::setParent(Widget* parent) {
    m_parent = parent;
}

void Widget::addChild(Widget* child) {
    if (!child || child->m_parent == this) {
        return;
    }

    // Remove from old parent
    if (child->m_parent) {
        child->m_parent->removeChild(child);
    }

    child->setParent(this);
    m_children.push_back(child);
    markDirty(DirtyFlag::Children | DirtyFlag::Layout);
}

void Widget::removeChild(Widget* child) {
    if (!child) return;

    auto it = std::find(m_children.begin(), m_children.end(), child);
    if (it != m_children.end()) {
        child->setParent(nullptr);
        m_children.erase(it);
        markDirty(DirtyFlag::Children | DirtyFlag::Layout);
    }
}

void Widget::removeAllChildren() {
    for (Widget* child : m_children) {
        child->setParent(nullptr);
    }
    m_children.clear();
    markDirty(DirtyFlag::Children | DirtyFlag::Layout);
}

Widget* Widget::childAt(size_t index) const {
    if (index < m_children.size()) {
        return m_children[index];
    }
    return nullptr;
}

Widget* Widget::findChild(WidgetId id) const {
    for (Widget* child : m_children) {
        if (child->id() == id) {
            return child;
        }
        // Recursive search
        Widget* found = child->findChild(id);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

Widget* Widget::findChild(const char* name) const {
    if (!name) return nullptr;

    for (Widget* child : m_children) {
        if (child->m_name && strcmp(child->m_name, name) == 0) {
            return child;
        }
        // Recursive search
        Widget* found = child->findChild(name);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

Widget* Widget::root() {
    Widget* w = this;
    while (w->m_parent) {
        w = w->m_parent;
    }
    return w;
}

const Widget* Widget::root() const {
    const Widget* w = this;
    while (w->m_parent) {
        w = w->m_parent;
    }
    return w;
}

//=============================================================================
// Signals
//=============================================================================

SlotId Widget::onValueChanged(SlotFunction handler) {
    return m_signal.connect(SignalType::ValueChanged, handler, this);
}

SlotId Widget::onClick(SlotFunction handler) {
    return m_signal.connect(SignalType::Click, handler, this);
}

SlotId Widget::onFocus(SlotFunction handler) {
    return m_signal.connect(SignalType::Focus, handler, this);
}

SlotId Widget::onBlur(SlotFunction handler) {
    return m_signal.connect(SignalType::Blur, handler, this);
}

SlotId Widget::onKeyPress(SlotFunction handler) {
    return m_signal.connect(SignalType::KeyPress, handler, this);
}

void Widget::emitValueChanged(int32_t oldVal, int32_t newVal, const char* str) {
    Event e = Events::valueChanged(oldVal, newVal, str);
    e.sender = this;
    m_signal.emit(e);
}

void Widget::emitClick() {
    Event e = Events::click();
    e.sender = this;
    m_signal.emit(e);
}

void Widget::emitRender() {
    Event e = Events::render(m_bounds.x, m_bounds.y,
                              m_bounds.width, m_bounds.height, false);
    e.sender = this;
    m_signal.emit(e);
}

void Widget::emitInvalidate() {
    emitInvalidate(m_bounds);
}

void Widget::emitInvalidate(const Rect& region) {
    Event e = Events::invalidate(region.x, region.y, region.width, region.height);
    e.sender = this;
    m_signal.emit(e);
}

//=============================================================================
// Event Handling
//=============================================================================

bool Widget::handleEvent(Event& event) {
    if (!isVisible() || !isEnabled()) {
        return false;
    }

    // Handle based on event type
    switch (event.type) {
        case SignalType::KeyPress:
            if (onKeyPress(event.data.key.key, event.data.key.modifiers)) {
                event.consume();
                return true;
            }
            break;

        case SignalType::KeyHold:
            if (onKeyHold(event.data.key.key, event.data.key.holdDuration,
                         event.data.key.modifiers)) {
                event.consume();
                return true;
            }
            break;

        case SignalType::Focus:
            if (isFocusable()) {
                setFocused(true);
                event.consume();
                return true;
            }
            break;

        case SignalType::Blur:
            setFocused(false);
            event.consume();
            return true;

        case SignalType::Click:
            emitClick();
            event.consume();
            return true;

        default:
            break;
    }

    // Emit to signal handlers
    m_signal.emit(event);

    return event.isConsumed();
}

bool Widget::onKeyPress(char key, uint8_t modifiers) {
    (void)key;
    (void)modifiers;
    return false;  // Not handled
}

bool Widget::onKeyHold(char key, uint32_t duration, uint8_t modifiers) {
    (void)key;
    (void)duration;
    (void)modifiers;
    return false;  // Not handled
}

void Widget::onFocusGained() {
    markDirty(DirtyFlag::Style);
}

void Widget::onFocusLost() {
    markDirty(DirtyFlag::Style);
}

//=============================================================================
// Rendering
//=============================================================================

void Widget::render() {
    if (!isVisible()) {
        return;
    }

    Rect abs = absoluteBounds();

    // Draw background if opaque
    if (m_style.opaque) {
        uint16_t bg = effectiveBackgroundColor();

        // Check if focused and draw focus highlight
        if (isFocused() && m_style.focusColor != 0) {
            bg = m_style.focusColor;
        }

        if (m_style.borderRadius > 0) {
            Draw::fillRoundRect(abs.x, abs.y, abs.width, abs.height,
                               m_style.borderRadius, bg);
        } else {
            Draw::fillRect(abs.x, abs.y, abs.width, abs.height, bg);
        }
    }

    // Render content
    renderContent();

    // Draw border
    if (m_style.borderWidth > 0) {
        uint16_t borderColor = effectiveBorderColor();
        if (isFocused()) {
            borderColor = m_style.focusColor != 0 ? m_style.focusColor :
                          ThemeManager::instance().theme().menuTextFocusedColor();
        }

        if (m_style.borderRadius > 0) {
            Draw::drawRoundRect(abs.x, abs.y, abs.width, abs.height,
                               m_style.borderRadius, borderColor);
        } else {
            Draw::drawRect(abs.x, abs.y, abs.width, abs.height, borderColor);
        }
    }

    // Render children
    renderChildren();
}

void Widget::renderContent() {
    // Base implementation does nothing
    // Derived classes override this
}

void Widget::renderChildren() {
    for (Widget* child : m_children) {
        if (child->isVisible()) {
            child->render();
        }
    }
}

Rect Widget::contentRect() const {
    int16_t padH = m_style.padding.horizontal();
    int16_t padV = m_style.padding.vertical();
    int16_t w = static_cast<int16_t>(m_bounds.width) - padH;
    int16_t h = static_cast<int16_t>(m_bounds.height) - padV;
    return Rect(
        m_bounds.x + m_style.padding.left,
        m_bounds.y + m_style.padding.top,
        w > 0 ? w : 0,
        h > 0 ? h : 0
    );
}

//=============================================================================
// Layout
//=============================================================================

void Widget::measure(int16_t availableWidth, int16_t availableHeight) {
    int16_t w = m_sizeConstraint.preferredWidth >= 0 ?
                m_sizeConstraint.preferredWidth : availableWidth;
    int16_t h = m_sizeConstraint.preferredHeight >= 0 ?
                m_sizeConstraint.preferredHeight : availableHeight;

    w = m_sizeConstraint.clampWidth(w);
    h = m_sizeConstraint.clampHeight(h);

    setMeasuredSize(w, h);
}

void Widget::layout() {
    // Base implementation: just measure each child
    for (Widget* child : m_children) {
        int16_t availW = m_bounds.width - m_style.padding.horizontal();
        int16_t availH = m_bounds.height - m_style.padding.vertical();
        child->measure(availW, availH);
        child->layout();
    }
}

void Widget::setMeasuredSize(int16_t w, int16_t h) {
    m_measuredWidth = w;
    m_measuredHeight = h;
}

//=============================================================================
// Display Adaptation
//=============================================================================

void Widget::adaptToDisplay(int16_t displayWidth, int16_t displayHeight,
                            float scaleFactor) {
    // Select appropriate variant
    if (m_variant == WidgetVariant::Auto) {
        m_variant = selectVariant(displayWidth, displayHeight);
    }

    // Scale dimensions
    if (scaleFactor != 1.0f) {
        m_bounds.x = static_cast<int16_t>(m_bounds.x * scaleFactor);
        m_bounds.y = static_cast<int16_t>(m_bounds.y * scaleFactor);
        m_bounds.width = static_cast<uint16_t>(m_bounds.width * scaleFactor);
        m_bounds.height = static_cast<uint16_t>(m_bounds.height * scaleFactor);
    }

    // Adapt children
    for (Widget* child : m_children) {
        child->adaptToDisplay(displayWidth, displayHeight, scaleFactor);
    }

    markDirty(DirtyFlag::All);
}

WidgetVariant Widget::selectVariant(int16_t displayWidth, int16_t displayHeight) {
    // Default heuristics based on display size
    int16_t area = displayWidth * displayHeight;

    if (area >= 240 * 320) {
        return WidgetVariant::Full;
    } else if (area >= 240 * 135) {
        return WidgetVariant::Compact;  // M5Cardputer
    } else if (area >= 128 * 64) {
        return WidgetVariant::Minimal;
    } else {
        return WidgetVariant::Icon;
    }
}

} // namespace GUI
