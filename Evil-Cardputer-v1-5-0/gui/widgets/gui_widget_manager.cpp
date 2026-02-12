/**
 * @file gui_widget_manager.cpp
 * @brief Widget manager implementation
 */

#include "gui_widget_manager.h"
#include "../gui_draw.h"

namespace GUI {

//=============================================================================
// Singleton
//=============================================================================

WidgetManager& WidgetManager::instance() {
    static WidgetManager s_instance;
    return s_instance;
}

WidgetManager::WidgetManager()
    : m_initialized(false)
    , m_root(nullptr)
    , m_focusedWidget(nullptr)
    , m_keyRepeatDelay(400)
    , m_keyRepeatInterval(50)
    , m_needsLayout(false)
    , m_fullRedrawPending(false)
    , m_renderCallback(nullptr)
    , m_displayInfoCallback(nullptr)
{
}

WidgetManager::~WidgetManager() {
    shutdown();
}

//=============================================================================
// Initialization
//=============================================================================

void WidgetManager::init(const DisplayInfo& displayInfo) {
    if (m_initialized) {
        return;
    }

    m_displayInfo = displayInfo;
    m_initialized = true;
    m_fullRedrawPending = true;

    // Emit display ready signal
    Event e = Events::displayReady(
        displayInfo.width,
        displayInfo.height,
        displayInfo.colorDepth,
        displayInfo.rotation,
        displayInfo.scaleFactor
    );
    m_globalSignal.emit(e);
}

void WidgetManager::shutdown() {
    clearRoot();

    // Clear screen stack
    for (Widget* screen : m_screenStack) {
        delete screen;
    }
    m_screenStack.clear();

    m_focusedWidget = nullptr;
    m_initialized = false;
}

//=============================================================================
// Display Info
//=============================================================================

void WidgetManager::setDisplayInfo(const DisplayInfo& info) {
    bool changed = (m_displayInfo.width != info.width ||
                    m_displayInfo.height != info.height ||
                    m_displayInfo.rotation != info.rotation ||
                    m_displayInfo.scaleFactor != info.scaleFactor);

    m_displayInfo = info;

    if (changed) {
        // Emit resolution changed signal
        Event e(SignalType::ResolutionChanged, SignalPriority::High);
        e.data.display.width = info.width;
        e.data.display.height = info.height;
        e.data.display.rotation = info.rotation;
        e.data.display.scaleFactor = info.scaleFactor;
        m_globalSignal.emit(e);

        // Adapt widgets to new display
        if (m_root) {
            m_root->adaptToDisplay(info.width, info.height, info.scaleFactor);
        }

        m_needsLayout = true;
        m_fullRedrawPending = true;
    }
}

void WidgetManager::queryDisplayInfo() {
    if (m_displayInfoCallback) {
        DisplayInfo info = m_displayInfoCallback();
        setDisplayInfo(info);
    }
}

//=============================================================================
// Root Widget
//=============================================================================

void WidgetManager::setRoot(Widget* root) {
    if (m_root == root) {
        return;
    }

    // Clear focus from old root
    if (m_focusedWidget && m_root) {
        clearFocus();
    }

    // Delete old root
    if (m_root) {
        delete m_root;
    }

    m_root = root;

    if (m_root) {
        // Set bounds to fill display
        m_root->setBounds(0, 0, m_displayInfo.width, m_displayInfo.height);

        // Adapt to display
        m_root->adaptToDisplay(m_displayInfo.width, m_displayInfo.height,
                               m_displayInfo.scaleFactor);

        // Layout
        m_needsLayout = true;

        // Find first focusable
        Widget* firstFocusable = findFirstFocusable(m_root);
        if (firstFocusable) {
            setFocus(firstFocusable);
        }
    }

    m_fullRedrawPending = true;
}

void WidgetManager::clearRoot() {
    if (m_root) {
        clearFocus();
        delete m_root;
        m_root = nullptr;
    }
}

//=============================================================================
// Screen Stack
//=============================================================================

void WidgetManager::pushScreen(Widget* screen) {
    if (!screen) return;

    // Save current root
    if (m_root) {
        m_screenStack.push_back(m_root);
        m_root = nullptr;
    }

    setRoot(screen);
}

Widget* WidgetManager::popScreen() {
    Widget* current = m_root;
    m_root = nullptr;  // Don't delete, caller takes ownership
    m_focusedWidget = nullptr;

    if (!m_screenStack.empty()) {
        Widget* previous = m_screenStack.back();
        m_screenStack.pop_back();
        setRoot(previous);
    }

    return current;
}

//=============================================================================
// Focus Management
//=============================================================================

bool WidgetManager::setFocus(Widget* widget) {
    if (widget && !widget->isFocusable()) {
        return false;
    }

    if (m_focusedWidget == widget) {
        return true;
    }

    // Blur old widget
    if (m_focusedWidget) {
        m_focusedWidget->setFocused(false);
    }

    // Focus new widget
    m_focusedWidget = widget;
    if (m_focusedWidget) {
        m_focusedWidget->setFocused(true);
    }

    return true;
}

void WidgetManager::clearFocus() {
    setFocus(nullptr);
}

void WidgetManager::collectFocusableWidgets(Widget* widget, std::vector<Widget*>& list) {
    if (!widget || !widget->isVisible() || !widget->isEnabled()) {
        return;
    }

    if (widget->isFocusable()) {
        list.push_back(widget);
    }

    for (Widget* child : widget->children()) {
        collectFocusableWidgets(child, list);
    }
}

Widget* WidgetManager::findFirstFocusable(Widget* root) {
    std::vector<Widget*> focusable;
    collectFocusableWidgets(root ? root : m_root, focusable);

    if (!focusable.empty()) {
        return focusable.front();
    }
    return nullptr;
}

Widget* WidgetManager::findNextFocusable(Widget* from) {
    std::vector<Widget*> focusable;
    collectFocusableWidgets(m_root, focusable);

    if (focusable.empty()) {
        return nullptr;
    }

    // Find current position
    auto it = std::find(focusable.begin(), focusable.end(), from);
    if (it == focusable.end()) {
        return focusable.front();
    }

    // Move to next
    ++it;
    if (it == focusable.end()) {
        return focusable.front();  // Wrap around
    }
    return *it;
}

Widget* WidgetManager::findPreviousFocusable(Widget* from) {
    std::vector<Widget*> focusable;
    collectFocusableWidgets(m_root, focusable);

    if (focusable.empty()) {
        return nullptr;
    }

    // Find current position
    auto it = std::find(focusable.begin(), focusable.end(), from);
    if (it == focusable.end() || it == focusable.begin()) {
        return focusable.back();  // Wrap around
    }

    return *(--it);
}

void WidgetManager::focusNext() {
    Widget* next = findNextFocusable(m_focusedWidget);
    if (next) {
        setFocus(next);
    }
}

void WidgetManager::focusPrevious() {
    Widget* prev = findPreviousFocusable(m_focusedWidget);
    if (prev) {
        setFocus(prev);
    }
}

//=============================================================================
// Input Handling
//=============================================================================

bool WidgetManager::processKeyInput(char key, uint8_t modifiers) {
    // Create event
    Event e = Events::keyPress(key, modifiers);

    // Navigation keys (handled by manager)
    if (modifiers & KeyEventData::MOD_FN) {
        // FN + Tab = next focus
        if (key == '\t') {
            focusNext();
            e.consume();
            return true;
        }
    } else {
        // Tab = next focus, Shift+Tab = previous
        if (key == '\t') {
            if (modifiers & KeyEventData::MOD_SHIFT) {
                focusPrevious();
            } else {
                focusNext();
            }
            e.consume();
            return true;
        }
    }

    // Send to focused widget
    if (m_focusedWidget) {
        e.target = m_focusedWidget;
        if (m_focusedWidget->handleEvent(e)) {
            return true;
        }
    }

    // Bubble up through parent chain
    Widget* current = m_focusedWidget ? m_focusedWidget->parent() : nullptr;
    while (current && !e.isConsumed()) {
        current->handleEvent(e);
        current = current->parent();
    }

    // Global handler
    if (!e.isConsumed()) {
        m_globalSignal.emit(e);
    }

    return e.isConsumed();
}

bool WidgetManager::processKeyHold(char key, uint32_t duration, uint8_t modifiers) {
    Event e = Events::keyHold(key, duration, modifiers);

    if (m_focusedWidget) {
        e.target = m_focusedWidget;
        if (m_focusedWidget->handleEvent(e)) {
            return true;
        }
    }

    return e.isConsumed();
}

void WidgetManager::updateInput() {
    // This would be called with actual keyboard input from M5Cardputer
    // For now, this is a placeholder for the input update loop

    uint32_t now = millis();

    // Handle key repeat
    if (m_inputState.lastKey != 0) {
        uint32_t holdTime = now - m_inputState.keyPressTime;

        if (holdTime > m_keyRepeatDelay) {
            // In repeat mode
            uint32_t repeatTime = holdTime - m_keyRepeatDelay;
            uint32_t repeatCount = repeatTime / m_keyRepeatInterval;

            if (repeatCount > 0) {
                m_inputState.keyHoldTime = holdTime;
                m_inputState.isRepeat = true;
                processKeyHold(m_inputState.lastKey, holdTime, m_inputState.modifiers);
            }
        }
    }
}

//=============================================================================
// Dirty Tracking
//=============================================================================

void WidgetManager::markDirty(const Rect& region) {
    if (region.isEmpty()) {
        return;
    }

    // Merge with existing dirty regions
    for (Rect& existing : m_dirtyRegions) {
        if (existing.intersects(region)) {
            existing = existing.united(region);
            return;
        }
    }

    m_dirtyRegions.push_back(region);
}

void WidgetManager::markDirty(Widget* widget) {
    if (widget) {
        markDirty(widget->absoluteBounds());
    }
}

void WidgetManager::markAllDirty() {
    m_dirtyRegions.clear();
    m_fullRedrawPending = true;
}

Rect WidgetManager::combinedDirtyRegion() const {
    Rect combined;
    for (const Rect& r : m_dirtyRegions) {
        combined = combined.united(r);
    }
    return combined;
}

void WidgetManager::clearDirtyRegions() {
    m_dirtyRegions.clear();
    m_fullRedrawPending = false;
}

//=============================================================================
// Rendering
//=============================================================================

void WidgetManager::renderWidgetIfDirty(Widget* widget) {
    if (!widget || !widget->isVisible()) {
        return;
    }

    bool needsRender = widget->isDirty();

    // Check if widget intersects with any dirty region
    if (!needsRender && !m_fullRedrawPending) {
        Rect widgetBounds = widget->absoluteBounds();
        for (const Rect& dirty : m_dirtyRegions) {
            if (widgetBounds.intersects(dirty)) {
                needsRender = true;
                break;
            }
        }
    }

    if (needsRender || m_fullRedrawPending) {
        widget->render();
        widget->onRendered();
        m_renderStats.widgetsRendered++;
    }

    // Render children
    for (Widget* child : widget->children()) {
        renderWidgetIfDirty(child);
    }
}

void WidgetManager::renderDirty() {
    if (!m_root) {
        return;
    }

    if (!hasDirtyRegions() && !m_fullRedrawPending) {
        return;
    }

    uint32_t startTime = micros();

    // Notify callback
    if (m_renderCallback) {
        Rect region = m_fullRedrawPending ?
            Rect(0, 0, m_displayInfo.width, m_displayInfo.height) :
            combinedDirtyRegion();
        m_renderCallback(region, m_fullRedrawPending);
    }

    m_renderStats.widgetsRendered = 0;

    // Render dirty widgets
    renderWidgetIfDirty(m_root);

    // Clear dirty state
    clearDirtyRegions();

    // Update stats
    uint32_t renderTime = micros() - startTime;
    m_renderStats.lastRenderTimeUs = renderTime;
    m_renderStats.framesRendered++;

    if (m_fullRedrawPending) {
        m_renderStats.fullRenders++;
    } else {
        m_renderStats.partialRenders++;
    }

    // Update average
    m_renderStats.avgRenderTimeUs =
        (m_renderStats.avgRenderTimeUs * 0.9f) + (renderTime * 0.1f);

    // Signal frame end to renderer
    Draw::endFrame();
}

void WidgetManager::renderAll() {
    m_fullRedrawPending = true;
    renderDirty();
}

void WidgetManager::update() {
    if (!m_initialized || !m_root) {
        return;
    }

    // Process input
    updateInput();

    // Layout if needed
    if (m_needsLayout) {
        layout();
    }

    // Render dirty regions
    renderDirty();
}

//=============================================================================
// Signals
//=============================================================================

void WidgetManager::emitRenderRequest(const Rect& region) {
    Event e = Events::render(region.x, region.y, region.width, region.height, false);
    m_globalSignal.emit(e);
}

void WidgetManager::emitInvalidate(const Rect& region) {
    Event e = Events::invalidate(region.x, region.y, region.width, region.height);
    m_globalSignal.emit(e);
    markDirty(region);
}

//=============================================================================
// Widget Lookup
//=============================================================================

Widget* WidgetManager::findWidgetByIdRecursive(Widget* widget, WidgetId id) {
    if (!widget) return nullptr;
    if (widget->id() == id) return widget;

    for (Widget* child : widget->children()) {
        Widget* found = findWidgetByIdRecursive(child, id);
        if (found) return found;
    }
    return nullptr;
}

Widget* WidgetManager::findWidgetByNameRecursive(Widget* widget, const char* name) {
    if (!widget) return nullptr;
    if (widget->name() && strcmp(widget->name(), name) == 0) {
        return widget;
    }

    for (Widget* child : widget->children()) {
        Widget* found = findWidgetByNameRecursive(child, name);
        if (found) return found;
    }
    return nullptr;
}

Widget* WidgetManager::findWidget(WidgetId id) {
    return findWidgetByIdRecursive(m_root, id);
}

Widget* WidgetManager::findWidget(const char* name) {
    return findWidgetByNameRecursive(m_root, name);
}

Widget* WidgetManager::widgetAtRecursive(Widget* widget, int16_t x, int16_t y) {
    if (!widget || !widget->isVisible()) {
        return nullptr;
    }

    Rect bounds = widget->absoluteBounds();
    if (!bounds.contains(x, y)) {
        return nullptr;
    }

    // Check children (front to back)
    for (auto it = widget->children().rbegin(); it != widget->children().rend(); ++it) {
        Widget* found = widgetAtRecursive(*it, x, y);
        if (found) return found;
    }

    return widget;
}

Widget* WidgetManager::widgetAt(int16_t x, int16_t y) {
    return widgetAtRecursive(m_root, x, y);
}

//=============================================================================
// Layout
//=============================================================================

void WidgetManager::layout() {
    if (!m_root) {
        return;
    }

    m_root->measure(m_displayInfo.width, m_displayInfo.height);
    m_root->layout();
    m_needsLayout = false;
}

void WidgetManager::requestLayout() {
    m_needsLayout = true;
}

//=============================================================================
// Statistics
//=============================================================================

size_t WidgetManager::countWidgets(Widget* widget) const {
    if (!widget) return 0;

    size_t count = 1;
    for (Widget* child : widget->children()) {
        count += countWidgets(child);
    }
    return count;
}

size_t WidgetManager::widgetCount() const {
    return countWidgets(m_root);
}

} // namespace GUI
