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

    m_state = RendererState::Stopped;
    GUI_LOG("Renderer initialized (%dx%d)", m_displayWidth, m_displayHeight);
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
// Display Flush
// ============================================================================

void Renderer::flushDisplay() {
    // In Phase 1, we just call display() which blocks until SPI transfer completes
    // In Phase 2, this will initiate DMA transfer and swap buffers
    M5.Display.display();
    m_stats.displayFlushCount++;
}

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
