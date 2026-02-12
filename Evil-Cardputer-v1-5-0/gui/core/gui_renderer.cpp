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
#if GUI_DIRTY_TRACKING
#include "gui_dirty_region.h"
#endif
#include <Arduino.h>
#include <esp_timer.h>

namespace GUI {

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
    , m_displayWidth(Config::DISPLAY_WIDTH)
    , m_displayHeight(Config::DISPLAY_HEIGHT)
    , m_clipRect(Rect::make(0, 0, Config::DISPLAY_WIDTH, Config::DISPLAY_HEIGHT))
    , m_clipEnabled(false)
    , m_running(false)
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

    // Initialize render queue
    if (!RenderQueue::instance().init()) {
        GUI_LOG_ERROR("Failed to initialize RenderQueue");
        return false;
    }

    // Get display dimensions from M5GFX
    m_displayWidth = M5.Display.width();
    m_displayHeight = M5.Display.height();

    // Initialize clip rect to full screen
    m_clipRect = Rect::make(0, 0, m_displayWidth, m_displayHeight);
    m_clipEnabled = false;

#if GUI_DOUBLE_BUFFER
    // Initialize framebuffer (Phase 2)
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

#if GUI_DIRTY_TRACKING
    // Initialize DirtyRegionTracker (Phase 3)
    if (!DirtyRegionTracker::instance().init()) {
        GUI_LOG_ERROR("Failed to initialize DirtyRegionTracker");
        return false;
    }
    GUI_LOG("Renderer initialized (%dx%d) [DoubleBuffered + DMA + DirtyTracking]", m_displayWidth, m_displayHeight);
#else
    GUI_LOG("Renderer initialized (%dx%d) [DoubleBuffered + DMA]", m_displayWidth, m_displayHeight);
#endif
#else
    GUI_LOG("Renderer initialized (%dx%d) [Direct]", m_displayWidth, m_displayHeight);
#endif

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
        GUI_LOG_ERROR("Failed to create render task (error: %d)", result);
        return false;
    }

    m_state = RendererState::Running;
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

    RenderQueue::instance().shutdown();

#if GUI_DOUBLE_BUFFER
    // Shutdown Phase 3 components
#if GUI_DIRTY_TRACKING
    DirtyRegionTracker::instance().shutdown();
#endif
    // Shutdown Phase 2 components
    DisplayUpdater::instance().shutdown();
    DmaTransfer::instance().shutdown();
    Framebuffer::instance().shutdown();
#endif

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

        // Execute the command
        executeCommand(op);

        // Update timing stats
        uint32_t elapsedUs = esp_timer_get_time() - startUs;
        m_stats.totalRenderTimeUs += elapsedUs;
        m_stats.lastRenderTimeUs = elapsedUs;
        if (elapsedUs > m_stats.maxRenderTimeUs) {
            m_stats.maxRenderTimeUs = elapsedUs;
        }
        m_stats.commandsProcessed++;

        // Increment batch counter
        batchCount++;

        // Track max batch size
        if (batchCount > m_stats.maxBatchSize) {
            m_stats.maxBatchSize = batchCount;
        }

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
        M5.Display.fillRect(clipped.x, clipped.y, clipped.width, clipped.height, r.color);
    } else {
        M5.Display.fillRect(r.rect.x, r.rect.y, r.rect.width, r.rect.height, r.color);
    }
}

void Renderer::handleDrawRect(const RenderOp& op) {
    const auto& r = op.data.rect;

    // Note: M5GFX doesn't support thickness for drawRect, so we draw multiple rects
    if (r.thickness <= 1) {
        M5.Display.drawRect(r.rect.x, r.rect.y, r.rect.width, r.rect.height, r.color);
    } else {
        // Draw thick outline using multiple fillRects
        int16_t t = r.thickness;
        // Top
        M5.Display.fillRect(r.rect.x, r.rect.y, r.rect.width, t, r.color);
        // Bottom
        M5.Display.fillRect(r.rect.x, r.rect.y + r.rect.height - t, r.rect.width, t, r.color);
        // Left
        M5.Display.fillRect(r.rect.x, r.rect.y + t, t, r.rect.height - 2 * t, r.color);
        // Right
        M5.Display.fillRect(r.rect.x + r.rect.width - t, r.rect.y + t, t, r.rect.height - 2 * t, r.color);
    }
}

void Renderer::handleDrawLine(const RenderOp& op) {
    const auto& l = op.data.line;
    M5.Display.drawLine(l.p1.x, l.p1.y, l.p2.x, l.p2.y, l.color);
}

void Renderer::handleDrawPixel(const RenderOp& op) {
    const auto& p = op.data.pixel;
    M5.Display.drawPixel(p.pos.x, p.pos.y, p.color);
}

void Renderer::handleDrawText(const RenderOp& op) {
    const auto& t = op.data.text;

    // Configure text rendering
    M5.Display.setTextColor(t.fg, t.bg);
    M5.Display.setTextFont(t.font.font);
    M5.Display.setTextSize(t.font.getSize());
    M5.Display.setCursor(t.pos.x, t.pos.y);

    // Draw text (embedded in structure, max 12 chars)
    if (t.textLen > 0) {
        M5.Display.print(t.text);
    }
}

void Renderer::handleDrawChar(const RenderOp& op) {
    const auto& c = op.data.chr;

    M5.Display.setTextColor(c.fg, c.bg);
    M5.Display.setTextFont(c.font.font);
    M5.Display.setTextSize(c.font.getSize());
    M5.Display.setCursor(c.pos.x, c.pos.y);
    M5.Display.print(c.ch);
}

void Renderer::handleDrawBitmap(const RenderOp& op) {
    const auto& b = op.data.bitmap;

    if (b.data == nullptr) return;

    // Use M5GFX pushImage for efficient bitmap transfer
    M5.Display.pushImage(b.rect.x, b.rect.y, b.rect.width, b.rect.height, b.data);
}

void Renderer::handleDrawJpeg(const RenderOp& op) {
    const auto& j = op.data.jpeg;

    if (j.data == nullptr || j.len == 0) return;

    // Use M5GFX JPEG decoder
    M5.Display.drawJpg(j.data, j.len, j.pos.x, j.pos.y, j.maxWidth, j.maxHeight);
}

void Renderer::handleSetClip(const RenderOp& op) {
    m_clipRect = op.data.clip.clipRect;
    m_clipEnabled = true;

    // M5GFX supports setClipRect
    M5.Display.setClipRect(m_clipRect.x, m_clipRect.y,
                           m_clipRect.width, m_clipRect.height);
}

void Renderer::handleClearClip(const RenderOp& op) {
    m_clipEnabled = false;
    m_clipRect = Rect::make(0, 0, m_displayWidth, m_displayHeight);

    // Clear clip in M5GFX
    M5.Display.clearClipRect();
}

void Renderer::handleClear(const RenderOp& op) {
    M5.Display.fillScreen(op.data.fill.color);
}

void Renderer::handleFillScreen(const RenderOp& op) {
    M5.Display.fillScreen(op.data.fill.color);
}

void Renderer::handleSetBrightness(const RenderOp& op) {
    M5.Display.setBrightness(op.data.brightness.level);
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
    M5.Display.drawCircle(c.center.x, c.center.y, c.radius, c.color);
}

void Renderer::handleFillCircle(const RenderOp& op) {
    const auto& c = op.data.circle;
    M5.Display.fillCircle(c.center.x, c.center.y, c.radius, c.color);
}

void Renderer::handleDrawRoundRect(const RenderOp& op) {
    const auto& r = op.data.roundRect;
    M5.Display.drawRoundRect(r.rect.x, r.rect.y, r.rect.width, r.rect.height,
                             r.radius, r.color);
}

void Renderer::handleFillRoundRect(const RenderOp& op) {
    const auto& r = op.data.roundRect;
    M5.Display.fillRoundRect(r.rect.x, r.rect.y, r.rect.width, r.rect.height,
                             r.radius, r.color);
}

void Renderer::handleDrawTriangle(const RenderOp& op) {
    const auto& t = op.data.triangle;
    M5.Display.drawTriangle(t.p1.x, t.p1.y, t.p2.x, t.p2.y, t.p3.x, t.p3.y, t.color);
}

void Renderer::handleFillTriangle(const RenderOp& op) {
    const auto& t = op.data.triangle;
    M5.Display.fillTriangle(t.p1.x, t.p1.y, t.p2.x, t.p2.y, t.p3.x, t.p3.y, t.color);
}

void Renderer::handleScroll(const RenderOp& op) {
    const auto& s = op.data.scroll;
    M5.Display.scroll(s.dx, s.dy);
}

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

        // Swap front/back buffers
        fb.swap();

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
                uint32_t fullArea = Config::DISPLAY_WIDTH * Config::DISPLAY_HEIGHT;
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
        DisplayUpdater::instance().pushFramebuffer();
#endif

        m_stats.displayFlushCount++;
        return;
    }
#endif

    // Phase 1: Direct M5GFX display() call (blocking)
    M5.Display.display();
    m_stats.displayFlushCount++;
}

// ============================================================================
// Phase 2: Framebuffer Rendering
// ============================================================================

#if GUI_DOUBLE_BUFFER

void Renderer::executeCommandToFramebuffer(const RenderOp& op) {
    switch (op.type) {
        case RenderOpType::FillRect:
            handleFillRectFB(op);
            break;

        case RenderOpType::DrawRect:
            // DrawRect uses multiple fillRects
            {
                const auto& r = op.data.rect;
                Framebuffer& fb = Framebuffer::instance();
                if (r.thickness <= 1) {
                    // Draw outline with single-pixel lines
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

        // Extended primitives - fallback to M5GFX for complex shapes
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
            // Text rendering still uses M5GFX (complex font handling)
            // We render to M5.Display which acts as a sprite pointing to our buffer
            handleDrawText(op);
            break;

        case RenderOpType::DrawChar:
            handleDrawChar(op);
            break;

        case RenderOpType::DrawBitmap:
            // Bitmap: copy directly to framebuffer
            {
                const auto& b = op.data.bitmap;
                if (b.data != nullptr) {
                    Framebuffer::instance().copyRect(
                        b.rect.x, b.rect.y,
                        b.data, b.rect.width, b.rect.height
                    );
                }
            }
            break;

        case RenderOpType::DrawJpeg:
            // JPEG: still uses M5GFX decoder
            handleDrawJpeg(op);
            break;

        case RenderOpType::SetClip:
            {
                Framebuffer::instance().setClipRect(op.data.clip.clipRect);
                m_clipRect = op.data.clip.clipRect;
                m_clipEnabled = true;
            }
            break;

        case RenderOpType::ClearClip:
            {
                Framebuffer::instance().clearClipRect();
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
            // Brightness control goes directly to display
            handleSetBrightness(op);
            break;

        case RenderOpType::Scroll:
            // Scroll uses direct M5.Display call
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

    // Bresenham's line algorithm for framebuffer
    int16_t x0 = l.p1.x, y0 = l.p1.y;
    int16_t x1 = l.p2.x, y1 = l.p2.y;

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
