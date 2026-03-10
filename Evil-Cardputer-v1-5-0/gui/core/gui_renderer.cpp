/*
 * GUI Renderer Implementation - FreeRTOS task on Core 0
 *
 * This task runs independently from the main loop, consuming render commands
 * from the lock-free queue and executing them via M5GFX.
 *
 * Core assignment:
 * - Renderer: Core 0 (RENDER_TASK_CORE = 0)
 * - Main loop: Core 1 (Arduino default)
 * - WiFi/BLE: Core 0 (ESP-IDF default), but lower priority
 */

#include "gui_renderer.h"
#include "gui_display_lock.h"
#include "gui_display_target.h"
#if GUI_DIRTY_TRACKING
#include "gui_dirty_region.h"
#endif
#include <Arduino.h>
#include <esp_timer.h>
#include <cstdlib>

namespace GUI {

portMUX_TYPE Renderer::s_statsLock = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// Singleton Instance
// ============================================================================

Renderer& Renderer::instance() {
    static Renderer instance;
    return instance;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

Renderer::Renderer()
    : m_state(RendererState::Uninitialized)
    , m_taskHandle(nullptr)
    , m_batchSize(16)           // Process 16 commands before flush
    , m_autoFlush(true)         // Auto-flush after batch
    , m_taskPriority(Config::RENDER_TASK_PRIORITY)
#if GUI_DOUBLE_BUFFER
    , m_renderMode(RenderMode::DoubleBuffered)
#else
    , m_renderMode(RenderMode::Direct)
#endif
    , m_displayWidth(0)
    , m_displayHeight(0)
    , m_clipRect(Rect::make(0, 0, 0, 0))
    , m_clipEnabled(false)
    , m_running(false)
    , m_startGate(nullptr)
    , m_lastFallbackTransferCount(0)
{
    m_stats.reset();
}

Renderer::~Renderer() {
    shutdown();
}

// ============================================================================
// Lifecycle Management
// ============================================================================

bool Renderer::init() {
    if (m_state != RendererState::Uninitialized) {
        GUI_LOG("Renderer already initialized");
        return true;
    }

    if (!initDisplayLock()) {
        GUI_LOG_ERROR("Failed to initialize display lock");
        return false;
    }

    // Validate that active runtime display is available.
    // width()/height() return 0 if panel is not initialized yet.
    refreshRuntimeDisplayMetrics();
    {
        DisplayLockGuard lockGuard;
        m_displayWidth = runtimeDisplayWidth();
        m_displayHeight = runtimeDisplayHeight();
        if (m_displayWidth == 0 || m_displayHeight == 0) {
            m_displayWidth = runtimeDisplay().width();
            m_displayHeight = runtimeDisplay().height();
        }
    }
    if (m_displayWidth == 0 || m_displayHeight == 0) {
        GUI_LOG_ERROR("Runtime display not initialized (width=%d, height=%d)",
                      m_displayWidth, m_displayHeight);
        return false;
    }

    // Initialize clip rect to full screen
    m_clipRect = Rect::make(0, 0, m_displayWidth, m_displayHeight);
    m_clipEnabled = false;

#if GUI_DOUBLE_BUFFER
    // Initialize framebuffer BEFORE queue so that the render buffer exists
    // before any commands can be enqueued and processed.
    if (!Framebuffer::instance().init()) {
        GUI_LOG_ERROR("Failed to initialize Framebuffer");
        return false;
    }

    // Initialize DMA transfer (Phase 2)
    if (!DmaTransfer::instance().init()) {
        GUI_LOG_ERROR("Failed to initialize DmaTransfer");
        return false;
    }

    // Initialize DisplayUpdater (Phase 2)
    if (!DisplayUpdater::instance().init()) {
        GUI_LOG_ERROR("Failed to initialize DisplayUpdater");
        return false;
    }
    DisplayUpdater::instance().resetTransferStats();
    m_lastFallbackTransferCount = 0;

#if GUI_DIRTY_TRACKING
    // Initialize DirtyRegionTracker (Phase 3)
    if (!DirtyRegionTracker::instance().init()) {
        GUI_LOG_ERROR("Failed to initialize DirtyRegionTracker");
        return false;
    }
    // Mark entire screen dirty so the first frame actually gets flushed.
    // Without this, the tracker starts clean and the first flush sees
    // "no changes" — resulting in a blank screen until something moves.
    DirtyRegionTracker::instance().markAllDirty();
#endif

    // Initialize M5Canvas sprite pointing to back buffer
    // This routes ALL draw operations (text, circles, etc.) into our framebuffer
    updateCanvasBuffer();

#if GUI_DIRTY_TRACKING
    GUI_LOG("Renderer initialized (%dx%d) [DoubleBuffered + DMA + DirtyTracking]", m_displayWidth, m_displayHeight);
#else
    GUI_LOG("Renderer initialized (%dx%d) [DoubleBuffered + DMA]", m_displayWidth, m_displayHeight);
#endif
#else
    GUI_LOG("Renderer initialized (%dx%d) [Direct]", m_displayWidth, m_displayHeight);
#endif

    // Initialize render queue LAST — once the queue is open, commands can be
    // pushed and the render task will try to process them. All subsystems
    // (Framebuffer, DMA, DirtyTracker) must be ready before this point.
    if (!RenderQueue::instance().init()) {
        GUI_LOG_ERROR("Failed to initialize RenderQueue");
        return false;
    }

    m_state = RendererState::Stopped;
    return true;
}

bool Renderer::start() {
    if (m_state == RendererState::Running) {
        GUI_LOG("Renderer already running");
        return true;
    }

    if (m_state == RendererState::Uninitialized) {
        if (!init()) {
            return false;
        }
    }

    m_state = RendererState::Starting;
    m_running = true;

    // Create startup gate — task will block on this until we signal it.
    // This prevents the render loop from processing commands before
    // all subsystems (Framebuffer, DMA, DirtyTracker) are fully ready
    // and m_state has been set to Running.
    m_startGate = xSemaphoreCreateBinary();

    // Create render task on Core 0
    BaseType_t result = xTaskCreatePinnedToCore(
        taskEntry,                          // Task function
        "GUI_Render",                       // Task name
        Config::RENDER_TASK_STACK_SIZE,     // Stack size
        this,                               // Task parameter
        m_taskPriority,                     // Priority
        &m_taskHandle,                      // Task handle output
        Config::RENDER_TASK_CORE            // Core ID
    );

    if (result != pdPASS) {
        m_running = false;
        m_state = RendererState::Stopped;
        if (m_startGate) { vSemaphoreDelete(m_startGate); m_startGate = nullptr; }
        GUI_LOG_ERROR("Failed to create render task (error: %d)", result);
        return false;
    }

    m_state = RendererState::Running;

    // Signal the render task that it's safe to start processing
    xSemaphoreGive(m_startGate);

    GUI_LOG("Renderer started on Core %d (priority: %d)",
            Config::RENDER_TASK_CORE, m_taskPriority);
    return true;
}

void Renderer::stop() {
    if (m_state != RendererState::Running) {
        return;
    }

    m_state = RendererState::Stopping;
    m_running = false;

    // Push a dummy command to wake up the task if it's blocking
    RenderOp nop;
    nop.type = RenderOpType::Nop;
    RenderQueue::instance().push(nop);

    // Wait for task to finish (with timeout)
    if (m_taskHandle != nullptr) {
        // Give task time to process final commands and exit
        uint32_t timeout = 1000;  // 1 second
        uint32_t start = millis();
        while (eTaskGetState(m_taskHandle) != eDeleted &&
               (millis() - start) < timeout) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // If task didn't exit cleanly, delete it
        if (eTaskGetState(m_taskHandle) != eDeleted) {
            vTaskDelete(m_taskHandle);
            GUI_LOG("Render task force-deleted");
        }

        m_taskHandle = nullptr;
    }

    m_state = RendererState::Stopped;
    GUI_LOG("Renderer stopped");
}

void Renderer::shutdown() {
    stop();

#if GUI_DOUBLE_BUFFER
    // Shutdown order matters: DMA must stop BEFORE freeing framebuffers,
    // otherwise DMA hardware may read freed memory.
    // Order: DirtyTracker → DisplayUpdater → DMA → Framebuffer
#if GUI_DIRTY_TRACKING
    DirtyRegionTracker::instance().shutdown();
#endif
    DisplayUpdater::instance().shutdown();
    DmaTransfer::instance().shutdown();
    Framebuffer::instance().shutdown();
#endif

    RenderQueue::instance().shutdown();

    m_state = RendererState::Uninitialized;
    GUI_LOG("Renderer shutdown complete");
}

// ============================================================================
// Task Entry Point
// ============================================================================

void Renderer::taskEntry(void* param) {
    Renderer* renderer = static_cast<Renderer*>(param);
    renderer->renderLoop();
    vTaskDelete(nullptr);  // Self-delete
}

// ============================================================================
// Main Render Loop
// ============================================================================

void Renderer::renderLoop() {
    // Wait for start() to finish initializing and set state to Running
    if (m_startGate) {
        xSemaphoreTake(m_startGate, portMAX_DELAY);
        vSemaphoreDelete(m_startGate);
        m_startGate = nullptr;
    }

    GUI_LOG("Render loop started");

    RenderQueue& queue = RenderQueue::instance();
    RenderOp op;
    uint32_t batchCount = 0;
    uint32_t lastFlushTime = millis();

    while (m_running) {
        // Wait for command from queue (blocking)
        uint32_t waitStart = millis();
        if (!queue.pop(op, Config::QUEUE_TIMEOUT_MS)) {
            // Timeout - flush if we have pending commands
            if (batchCount > 0 && m_autoFlush) {
                flushDisplay();
                batchCount = 0;
            }
            m_stats.idleTimeMs += millis() - waitStart;
            continue;
        }

        // Skip NOP commands
        if (op.type == RenderOpType::Nop) {
            continue;
        }

        // Measure execution time
        uint32_t startUs = esp_timer_get_time();

        // Execute the command — route to framebuffer when double-buffered
#if GUI_DOUBLE_BUFFER
        if (m_renderMode == RenderMode::DoubleBuffered) {
            executeCommandToFramebuffer(op);
        } else {
            executeCommand(op);
        }
#else
        executeCommand(op);
#endif

        // Update timing stats (under lock for cross-core safety)
        uint32_t elapsedUs = esp_timer_get_time() - startUs;
        portENTER_CRITICAL(&s_statsLock);
        m_stats.totalRenderTimeUs += elapsedUs;
        m_stats.lastRenderTimeUs = elapsedUs;
        if (elapsedUs > m_stats.maxRenderTimeUs) {
            m_stats.maxRenderTimeUs = elapsedUs;
        }
        m_stats.commandsProcessed++;
        portEXIT_CRITICAL(&s_statsLock);

        // Increment batch counter
        batchCount++;

        // Track max batch size
        portENTER_CRITICAL(&s_statsLock);
        if (batchCount > m_stats.maxBatchSize) {
            m_stats.maxBatchSize = batchCount;
        }
        portEXIT_CRITICAL(&s_statsLock);

        // Check if we should flush the display
        bool shouldFlush = false;

        // Flush conditions:
        // 1. Batch size reached
        // 2. EndFrame command
        // 3. Sync command
        // 4. Queue is empty and we have pending commands
        // 5. Time since last flush exceeds threshold (16ms = ~60fps)

        if (batchCount >= m_batchSize) {
            shouldFlush = true;
        } else if (op.type == RenderOpType::EndFrame) {
            shouldFlush = true;
        } else if (op.type == RenderOpType::Sync) {
            shouldFlush = true;
        } else if (queue.isEmpty() && batchCount > 0) {
            shouldFlush = true;
        } else if ((millis() - lastFlushTime) >= 16) {
            shouldFlush = true;
        }

        if (shouldFlush && m_autoFlush) {
            flushDisplay();
            batchCount = 0;
            lastFlushTime = millis();
        }
    }

    // Final flush before exit
    if (batchCount > 0) {
        flushDisplay();
    }

    GUI_LOG("Render loop exited (processed: %lu commands)", m_stats.commandsProcessed);
}

// ============================================================================
// Command Execution
// ============================================================================

void Renderer::executeCommand(const RenderOp& op) {
#if GUI_DOUBLE_BUFFER
    // Phase 2: Route certain operations to framebuffer
    if (m_renderMode == RenderMode::DoubleBuffered) {
        executeCommandToFramebuffer(op);
        return;
    }
#endif

    DisplayLockGuard displayLock;

    // Phase 1: Direct M5GFX rendering
    switch (op.type) {
        case RenderOpType::FillRect:
            handleFillRect(op);
            break;

        case RenderOpType::DrawRect:
            handleDrawRect(op);
            break;

        case RenderOpType::DrawLine:
            handleDrawLine(op);
            break;

        case RenderOpType::DrawPixel:
            handleDrawPixel(op);
            break;

        // Extended primitives
        case RenderOpType::DrawCircle:
            handleDrawCircle(op);
            break;

        case RenderOpType::FillCircle:
            handleFillCircle(op);
            break;

        case RenderOpType::DrawRoundRect:
            handleDrawRoundRect(op);
            break;

        case RenderOpType::FillRoundRect:
            handleFillRoundRect(op);
            break;

        case RenderOpType::DrawTriangle:
            handleDrawTriangle(op);
            break;

        case RenderOpType::FillTriangle:
            handleFillTriangle(op);
            break;

        case RenderOpType::DrawText:
            handleDrawText(op);
            break;

        case RenderOpType::DrawChar:
            handleDrawChar(op);
            break;

        case RenderOpType::DrawBitmap:
            handleDrawBitmap(op);
            break;

        case RenderOpType::DrawJpeg:
            handleDrawJpeg(op);
            break;

        case RenderOpType::SetClip:
            handleSetClip(op);
            break;

        case RenderOpType::ClearClip:
            handleClearClip(op);
            break;

        case RenderOpType::Clear:
            handleClear(op);
            break;

        case RenderOpType::FillScreen:
            handleFillScreen(op);
            break;

        case RenderOpType::SetBrightness:
            handleSetBrightness(op);
            break;

        case RenderOpType::Scroll:
            handleScroll(op);
            break;

        case RenderOpType::Sync:
            handleSync(op);
            break;

        case RenderOpType::EndFrame:
            handleEndFrame(op);
            break;

        case RenderOpType::Nop:
        default:
            // Ignore unknown commands
            break;
    }
}

// ============================================================================
// Command Handlers
// ============================================================================

void Renderer::handleFillRect(const RenderOp& op) {
    const auto& r = op.data.rect;

    // Apply clipping if enabled
    if (m_clipEnabled) {
        Rect clipped = r.rect.intersection(m_clipRect);
        if (clipped.isEmpty()) return;
        runtimeDisplay().fillRect(clipped.x, clipped.y, clipped.width, clipped.height, r.color);
    } else {
        runtimeDisplay().fillRect(r.rect.x, r.rect.y, r.rect.width, r.rect.height, r.color);
    }
}

void Renderer::handleDrawRect(const RenderOp& op) {
    const auto& r = op.data.rect;

    // Note: M5GFX doesn't support thickness for drawRect, so we draw multiple rects
    if (r.thickness <= 1) {
        runtimeDisplay().drawRect(r.rect.x, r.rect.y, r.rect.width, r.rect.height, r.color);
    } else {
        // Draw thick outline using multiple fillRects
        int16_t t = r.thickness;
        // Top
        runtimeDisplay().fillRect(r.rect.x, r.rect.y, r.rect.width, t, r.color);
        // Bottom
        runtimeDisplay().fillRect(r.rect.x, r.rect.y + r.rect.height - t, r.rect.width, t, r.color);
        // Left
        runtimeDisplay().fillRect(r.rect.x, r.rect.y + t, t, r.rect.height - 2 * t, r.color);
        // Right
        runtimeDisplay().fillRect(r.rect.x + r.rect.width - t, r.rect.y + t, t, r.rect.height - 2 * t, r.color);
    }
}

void Renderer::handleDrawLine(const RenderOp& op) {
    const auto& l = op.data.line;
    runtimeDisplay().drawLine(l.p1.x, l.p1.y, l.p2.x, l.p2.y, l.color);
}

void Renderer::handleDrawPixel(const RenderOp& op) {
    const auto& p = op.data.pixel;
    runtimeDisplay().drawPixel(p.pos.x, p.pos.y, p.color);
}

void Renderer::handleDrawText(const RenderOp& op) {
    const auto& t = op.data.text;

    // Configure text rendering
    runtimeDisplay().setTextColor(t.fg, t.bg);
    runtimeDisplay().setTextFont(t.font.font);
    runtimeDisplay().setTextSize(t.font.getSize());
    runtimeDisplay().setCursor(t.pos.x, t.pos.y);

    // Draw text (embedded in structure, max 12 chars)
    if (t.textLen > 0) {
        runtimeDisplay().print(t.text);
    }
}

void Renderer::handleDrawChar(const RenderOp& op) {
    const auto& c = op.data.chr;

    runtimeDisplay().setTextColor(c.fg, c.bg);
    runtimeDisplay().setTextFont(c.font.font);
    runtimeDisplay().setTextSize(c.font.getSize());
    runtimeDisplay().setCursor(c.pos.x, c.pos.y);
    runtimeDisplay().print(c.ch);
}

void Renderer::handleDrawBitmap(const RenderOp& op) {
    const auto& b = op.data.bitmap;

    if (b.data != nullptr) {
        // Use M5GFX pushImage for efficient bitmap transfer
        runtimeDisplay().pushImage(b.rect.x, b.rect.y, b.rect.width, b.rect.height, b.data);
    }

    if (b.ownsData && b.data) {
        std::free(const_cast<uint16_t*>(b.data));
    }
}

void Renderer::handleDrawJpeg(const RenderOp& op) {
    const auto& j = op.data.jpeg;

    if (j.data != nullptr && j.len > 0) {
        // Use M5GFX JPEG decoder
        runtimeDisplay().drawJpg(j.data, j.len, j.pos.x, j.pos.y, j.maxWidth, j.maxHeight);
    }

    if (j.ownsData && j.data) {
        std::free(const_cast<uint8_t*>(j.data));
    }
}

void Renderer::handleSetClip(const RenderOp& op) {
    m_clipRect = op.data.clip.clipRect;
    m_clipEnabled = true;

    // M5GFX supports setClipRect
    runtimeDisplay().setClipRect(m_clipRect.x, m_clipRect.y,
                           m_clipRect.width, m_clipRect.height);
}

void Renderer::handleClearClip(const RenderOp& op) {
    m_clipEnabled = false;
    m_clipRect = Rect::make(0, 0, m_displayWidth, m_displayHeight);

    // Clear clip in M5GFX
    runtimeDisplay().clearClipRect();
}

void Renderer::handleClear(const RenderOp& op) {
    runtimeDisplay().fillScreen(op.data.fill.color);
}

void Renderer::handleFillScreen(const RenderOp& op) {
    runtimeDisplay().fillScreen(op.data.fill.color);
}

void Renderer::handleSetBrightness(const RenderOp& op) {
    DisplayLockGuard displayLock;
    runtimeDisplay().setBrightness(op.data.brightness.level);
}

void Renderer::handleSync(const RenderOp& op) {
    // Flush display immediately
    flushDisplay();
    m_stats.syncCommands++;

    // Signal sync completion is handled in RenderQueue::pop()
}

void Renderer::handleEndFrame(const RenderOp& op) {
    m_stats.framesRendered++;
    // Frame end marker - flush will happen in main loop
}

// ============================================================================
// Extended Primitive Handlers
// ============================================================================

void Renderer::handleDrawCircle(const RenderOp& op) {
    const auto& c = op.data.circle;
    runtimeDisplay().drawCircle(c.center.x, c.center.y, c.radius, c.color);
}

void Renderer::handleFillCircle(const RenderOp& op) {
    const auto& c = op.data.circle;
    runtimeDisplay().fillCircle(c.center.x, c.center.y, c.radius, c.color);
}

void Renderer::handleDrawRoundRect(const RenderOp& op) {
    const auto& r = op.data.roundRect;
    runtimeDisplay().drawRoundRect(r.rect.x, r.rect.y, r.rect.width, r.rect.height,
                             r.radius, r.color);
}

void Renderer::handleFillRoundRect(const RenderOp& op) {
    const auto& r = op.data.roundRect;
    runtimeDisplay().fillRoundRect(r.rect.x, r.rect.y, r.rect.width, r.rect.height,
                             r.radius, r.color);
}

void Renderer::handleDrawTriangle(const RenderOp& op) {
    const auto& t = op.data.triangle;
    runtimeDisplay().drawTriangle(t.p1.x, t.p1.y, t.p2.x, t.p2.y, t.p3.x, t.p3.y, t.color);
}

void Renderer::handleFillTriangle(const RenderOp& op) {
    const auto& t = op.data.triangle;
    runtimeDisplay().fillTriangle(t.p1.x, t.p1.y, t.p2.x, t.p2.y, t.p3.x, t.p3.y, t.color);
}

void Renderer::handleScroll(const RenderOp& op) {
    const auto& s = op.data.scroll;
    runtimeDisplay().scroll(s.dx, s.dy);
}

// ============================================================================
// Canvas Buffer Management
// ============================================================================

#if GUI_DOUBLE_BUFFER
void Renderer::updateCanvasBuffer() {
    Framebuffer& fb = Framebuffer::instance();
    m_canvas.setBuffer(
        reinterpret_cast<void*>(fb.getBackBuffer()),
        fb.width(), fb.height(), 16
    );
}
#endif

// ============================================================================
// Display Flush
// ============================================================================

void Renderer::flushDisplay() {
#if GUI_DOUBLE_BUFFER
    if (m_renderMode == RenderMode::DoubleBuffered) {
        Framebuffer& fb = Framebuffer::instance();

#if GUI_DIRTY_TRACKING && GUI_PARTIAL_UPDATE
        // Phase 3: Use dirty region tracking for partial updates
        DirtyRegionTracker& dirty = DirtyRegionTracker::instance();

        if (!dirty.isDirty()) {
            // Nothing changed, skip update
            return;
        }

        // Swap front/back buffers and re-point canvas to new back buffer
        fb.swap();
        updateCanvasBuffer();

        if (dirty.shouldFullRefresh()) {
            // Too much changed - do full refresh
            DisplayUpdater::instance().pushFramebuffer();
            dirty.incrementFullRefresh();
        } else {
            // Partial update: only transfer dirty region
            Rect dirtyRect = dirty.getOptimalDirtyRect();
            if (!dirtyRect.isEmpty()) {
                DisplayUpdater::instance().pushRegion(dirtyRect);
                dirty.incrementPartialRefresh();

                // Calculate saved pixels
                uint32_t fullArea = static_cast<uint32_t>(fb.width()) * fb.height();
                uint32_t partialArea = dirtyRect.area();
                if (partialArea < fullArea) {
                    dirty.addSavedPixels(fullArea - partialArea);
                }
            }
        }

        // Clear dirty state after transfer
        dirty.markAllClean();
#else
        // Phase 2: Full buffer transfer
        fb.swap();
        updateCanvasBuffer();
        DisplayUpdater::instance().pushFramebuffer();
#endif

        const uint32_t fallbackCount = DisplayUpdater::instance().getFallbackTransferCount();
        if (fallbackCount > m_lastFallbackTransferCount) {
            m_stats.displayTransferFallbacks += (fallbackCount - m_lastFallbackTransferCount);
        }
        m_lastFallbackTransferCount = fallbackCount;

        m_stats.displayFlushCount++;
        return;
    }
#endif

    // Phase 1: Direct M5GFX display() call (blocking)
    DisplayLockGuard displayLock;
    if (!displayLock.locked()) {
        GUI_LOG_ERROR("Renderer: display lock acquisition failed during flush");
        return;
    }
    runtimeDisplay().display();
    m_stats.displayFlushCount++;
}

// ============================================================================
// Phase 2: Framebuffer Rendering
// ============================================================================

#if GUI_DOUBLE_BUFFER

void Renderer::executeCommandToFramebuffer(const RenderOp& op) {
    Framebuffer& fb = Framebuffer::instance();

    switch (op.type) {
        case RenderOpType::FillRect:
            handleFillRectFB(op);
            break;

        case RenderOpType::DrawRect:
            {
                const auto& r = op.data.rect;
                if (r.thickness <= 1) {
                    fb.drawHLine(r.rect.x, r.rect.y, r.rect.width, r.color);
                    fb.drawHLine(r.rect.x, r.rect.y + r.rect.height - 1, r.rect.width, r.color);
                    fb.drawVLine(r.rect.x, r.rect.y, r.rect.height, r.color);
                    fb.drawVLine(r.rect.x + r.rect.width - 1, r.rect.y, r.rect.height, r.color);
                } else {
                    int16_t t = r.thickness;
                    fb.fillRect(r.rect.x, r.rect.y, r.rect.width, t, r.color);
                    fb.fillRect(r.rect.x, r.rect.y + r.rect.height - t, r.rect.width, t, r.color);
                    fb.fillRect(r.rect.x, r.rect.y + t, t, r.rect.height - 2 * t, r.color);
                    fb.fillRect(r.rect.x + r.rect.width - t, r.rect.y + t, t, r.rect.height - 2 * t, r.color);
                }
            }
            break;

        case RenderOpType::DrawLine:
            handleDrawLineFB(op);
            break;

        case RenderOpType::DrawPixel:
            handleDrawPixelFB(op);
            break;

        // Extended primitives — render via M5Canvas into framebuffer
        case RenderOpType::DrawCircle:
            {
                const auto& c = op.data.circle;
                m_canvas.drawCircle(c.center.x, c.center.y, c.radius, c.color);
#if GUI_DIRTY_TRACKING
                int16_t r = c.radius;
                DirtyRegionTracker::instance().markDirty(
                    c.center.x - r, c.center.y - r, r * 2 + 1, r * 2 + 1);
#endif
            }
            break;

        case RenderOpType::FillCircle:
            {
                const auto& c = op.data.circle;
                m_canvas.fillCircle(c.center.x, c.center.y, c.radius, c.color);
#if GUI_DIRTY_TRACKING
                int16_t r = c.radius;
                DirtyRegionTracker::instance().markDirty(
                    c.center.x - r, c.center.y - r, r * 2 + 1, r * 2 + 1);
#endif
            }
            break;

        case RenderOpType::DrawRoundRect:
            {
                const auto& r = op.data.roundRect;
                m_canvas.drawRoundRect(r.rect.x, r.rect.y, r.rect.width, r.rect.height,
                                       r.radius, r.color);
#if GUI_DIRTY_TRACKING
                DirtyRegionTracker::instance().markDirty(
                    r.rect.x, r.rect.y, r.rect.width, r.rect.height);
#endif
            }
            break;

        case RenderOpType::FillRoundRect:
            {
                const auto& r = op.data.roundRect;
                m_canvas.fillRoundRect(r.rect.x, r.rect.y, r.rect.width, r.rect.height,
                                       r.radius, r.color);
#if GUI_DIRTY_TRACKING
                DirtyRegionTracker::instance().markDirty(
                    r.rect.x, r.rect.y, r.rect.width, r.rect.height);
#endif
            }
            break;

        case RenderOpType::DrawTriangle:
            {
                const auto& t = op.data.triangle;
                m_canvas.drawTriangle(t.p1.x, t.p1.y, t.p2.x, t.p2.y,
                                      t.p3.x, t.p3.y, t.color);
#if GUI_DIRTY_TRACKING
                int16_t minX = std::min(t.p1.x, std::min(t.p2.x, t.p3.x));
                int16_t minY = std::min(t.p1.y, std::min(t.p2.y, t.p3.y));
                int16_t maxX = std::max(t.p1.x, std::max(t.p2.x, t.p3.x));
                int16_t maxY = std::max(t.p1.y, std::max(t.p2.y, t.p3.y));
                DirtyRegionTracker::instance().markDirty(
                    minX, minY, maxX - minX + 1, maxY - minY + 1);
#endif
            }
            break;

        case RenderOpType::FillTriangle:
            {
                const auto& t = op.data.triangle;
                m_canvas.fillTriangle(t.p1.x, t.p1.y, t.p2.x, t.p2.y,
                                      t.p3.x, t.p3.y, t.color);
#if GUI_DIRTY_TRACKING
                int16_t minX = std::min(t.p1.x, std::min(t.p2.x, t.p3.x));
                int16_t minY = std::min(t.p1.y, std::min(t.p2.y, t.p3.y));
                int16_t maxX = std::max(t.p1.x, std::max(t.p2.x, t.p3.x));
                int16_t maxY = std::max(t.p1.y, std::max(t.p2.y, t.p3.y));
                DirtyRegionTracker::instance().markDirty(
                    minX, minY, maxX - minX + 1, maxY - minY + 1);
#endif
            }
            break;

        case RenderOpType::DrawText:
            {
                const auto& t = op.data.text;
                m_canvas.setTextColor(t.fg, t.bg);
                m_canvas.setTextFont(t.font.font);
                m_canvas.setTextSize(t.font.getSize());
                m_canvas.setCursor(t.pos.x, t.pos.y);
                if (t.textLen > 0) {
                    m_canvas.print(t.text);
                }
#if GUI_DIRTY_TRACKING
                int16_t tw = m_canvas.textWidth(t.text);
                int16_t th = m_canvas.fontHeight();
                DirtyRegionTracker::instance().markDirty(t.pos.x, t.pos.y, tw, th);
#endif
            }
            break;

        case RenderOpType::DrawChar:
            {
                const auto& c = op.data.chr;
                m_canvas.setTextColor(c.fg, c.bg);
                m_canvas.setTextFont(c.font.font);
                m_canvas.setTextSize(c.font.getSize());
                m_canvas.setCursor(c.pos.x, c.pos.y);
                m_canvas.print(c.ch);
#if GUI_DIRTY_TRACKING
                int16_t cw = m_canvas.textWidth(String(c.ch));
                int16_t ch = m_canvas.fontHeight();
                DirtyRegionTracker::instance().markDirty(c.pos.x, c.pos.y, cw, ch);
#endif
            }
            break;

        case RenderOpType::DrawBitmap:
            {
                const auto& b = op.data.bitmap;
                if (b.data != nullptr) {
                    fb.copyRect(b.rect.x, b.rect.y,
                                b.data, b.rect.width, b.rect.height);
                }
                if (b.ownsData && b.data) {
                    std::free(const_cast<uint16_t*>(b.data));
                }
            }
            break;

        case RenderOpType::DrawJpeg:
            {
                const auto& j = op.data.jpeg;
                if (j.data != nullptr && j.len > 0) {
                    m_canvas.drawJpg(j.data, j.len, j.pos.x, j.pos.y,
                                     j.maxWidth, j.maxHeight);
#if GUI_DIRTY_TRACKING
                    // JPEG decoded size unknown, mark full screen dirty
                    DirtyRegionTracker::instance().markAllDirty();
#endif
                }
                if (j.ownsData && j.data) {
                    std::free(const_cast<uint8_t*>(j.data));
                }
            }
            break;

        case RenderOpType::SetClip:
            {
                fb.setClipRect(op.data.clip.clipRect);
                m_canvas.setClipRect(op.data.clip.clipRect.x,
                                     op.data.clip.clipRect.y,
                                     op.data.clip.clipRect.width,
                                     op.data.clip.clipRect.height);
                m_clipRect = op.data.clip.clipRect;
                m_clipEnabled = true;
            }
            break;

        case RenderOpType::ClearClip:
            {
                fb.clearClipRect();
                m_canvas.clearClipRect();
                m_clipEnabled = false;
                m_clipRect = Rect::make(0, 0, m_displayWidth, m_displayHeight);
            }
            break;

        case RenderOpType::Clear:
            handleClearFB(op);
            break;

        case RenderOpType::FillScreen:
            handleClearFB(op);
            break;

        case RenderOpType::SetBrightness:
            // Brightness control goes directly to hardware
            handleSetBrightness(op);
            break;

        case RenderOpType::Scroll:
            {
                // Scroll within framebuffer using memmove
                const auto& s = op.data.scroll;
                uint16_t* buf = fb.getBackBuffer();
                uint16_t w = fb.width();
                uint16_t h = fb.height();

                if (s.dy > 0 && s.dy < h) {
                    // Scroll down
                    memmove(buf + s.dy * w, buf, (h - s.dy) * w * sizeof(uint16_t));
                    memset(buf, 0, s.dy * w * sizeof(uint16_t));
                } else if (s.dy < 0 && (-s.dy) < h) {
                    int16_t absdy = -s.dy;
                    memmove(buf, buf + absdy * w, (h - absdy) * w * sizeof(uint16_t));
                    memset(buf + (h - absdy) * w, 0, absdy * w * sizeof(uint16_t));
                }

                if (s.dx > 0 && s.dx < w) {
                    // Scroll right
                    for (int16_t row = 0; row < h; row++) {
                        uint16_t* rowPtr = buf + row * w;
                        memmove(rowPtr + s.dx, rowPtr, (w - s.dx) * sizeof(uint16_t));
                        memset(rowPtr, 0, s.dx * sizeof(uint16_t));
                    }
                } else if (s.dx < 0 && (-s.dx) < w) {
                    int16_t absdx = -s.dx;
                    for (int16_t row = 0; row < h; row++) {
                        uint16_t* rowPtr = buf + row * w;
                        memmove(rowPtr, rowPtr + absdx, (w - absdx) * sizeof(uint16_t));
                        memset(rowPtr + (w - absdx), 0, absdx * sizeof(uint16_t));
                    }
                }

#if GUI_DIRTY_TRACKING
                DirtyRegionTracker::instance().markAllDirty();
#endif
            }
            break;

        case RenderOpType::Sync:
            handleSync(op);
            break;

        case RenderOpType::EndFrame:
            handleEndFrame(op);
            break;

        case RenderOpType::Nop:
        default:
            break;
    }
}

void Renderer::handleFillRectFB(const RenderOp& op) {
    const auto& r = op.data.rect;
    Framebuffer::instance().fillRect(r.rect.x, r.rect.y, r.rect.width, r.rect.height, r.color);
}

void Renderer::handleDrawLineFB(const RenderOp& op) {
    const auto& l = op.data.line;
    Framebuffer& fb = Framebuffer::instance();
    const int16_t fbWidth = static_cast<int16_t>(fb.width());
    const int16_t fbHeight = static_cast<int16_t>(fb.height());

    int16_t x0 = l.p1.x, y0 = l.p1.y;
    int16_t x1 = l.p2.x, y1 = l.p2.y;

    auto csOutcode = [fbWidth, fbHeight](int16_t x, int16_t y) -> uint8_t {
        uint8_t code = 0;
        if (x < 0) code |= 1;
        else if (x >= fbWidth) code |= 2;
        if (y < 0) code |= 4;
        else if (y >= fbHeight) code |= 8;
        return code;
    };

    // Cohen-Sutherland pre-clipping — reject/clip before iterating pixels
    for (;;) {
        uint8_t c0 = csOutcode(x0, y0);
        uint8_t c1 = csOutcode(x1, y1);
        if (!(c0 | c1)) break;  // Both inside
        if (c0 & c1) return;     // Both outside same edge — trivial reject

        uint8_t out = c0 ? c0 : c1;
        int16_t x, y;
        int16_t dx = x1 - x0, dy = y1 - y0;
        if (out & 8) {
            x = x0 + dx * (fbHeight - 1 - y0) / dy;
            y = fbHeight - 1;
        } else if (out & 4) {
            x = x0 + dx * (-y0) / dy;
            y = 0;
        } else if (out & 2) {
            y = y0 + dy * (fbWidth - 1 - x0) / dx;
            x = fbWidth - 1;
        } else {
            y = y0 + dy * (-x0) / dx;
            x = 0;
        }
        if (out == c0) { x0 = x; y0 = y; }
        else           { x1 = x; y1 = y; }
    }

    // Bresenham on the clipped segment — all pixels are in-bounds
    int16_t dx = abs(x1 - x0);
    int16_t dy = -abs(y1 - y0);
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy;

    while (true) {
        fb.setPixel(x0, y0, l.color);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Renderer::handleDrawPixelFB(const RenderOp& op) {
    const auto& p = op.data.pixel;
    Framebuffer::instance().setPixel(p.pos.x, p.pos.y, p.color);
}

void Renderer::handleClearFB(const RenderOp& op) {
    Framebuffer::instance().clear(op.data.fill.color);
}

#endif // GUI_DOUBLE_BUFFER

// ============================================================================
// Convenience Functions
// ============================================================================

bool guiInit() {
    return Renderer::instance().init();
}

bool guiStart() {
    return Renderer::instance().start();
}

void guiStop() {
    Renderer::instance().stop();
}

void guiShutdown() {
    Renderer::instance().shutdown();
}

bool guiIsRunning() {
    return Renderer::instance().isRunning();
}

} // namespace GUI
