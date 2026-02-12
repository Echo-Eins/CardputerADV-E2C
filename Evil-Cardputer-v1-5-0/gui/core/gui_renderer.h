/*
 * GUI Renderer - Async rendering task on Core 0
 *
 * This module provides the rendering engine that runs on a dedicated FreeRTOS task.
 * It consumes commands from the RenderQueue and executes them on the display hardware.
 *
 * Architecture:
 * - Runs on Core 0 (separate from main loop on Core 1)
 * - Blocks waiting for commands from RenderQueue
 * - Executes M5GFX drawing primitives
 * - Manages display state (cursor, colors, fonts)
 *
 * Phase 1 features:
 * - Basic command execution (no double buffering)
 * - Direct M5.Display rendering
 * - Synchronous display() calls after batch processing
 *
 * Phase 2 features:
 * - Double buffering with PSRAM framebuffers
 * - DMA transfers for async display updates
 * - Dirty region optimization (future)
 */

#ifndef GUI_RENDERER_H
#define GUI_RENDERER_H

#include "gui_render_queue.h"
#include "gui_framebuffer.h"
#include "gui_dma.h"
#include <M5Unified.h>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace GUI {

// ============================================================================
// Renderer State
// ============================================================================

enum class RendererState : uint8_t {
    Uninitialized = 0,
    Stopped,
    Starting,
    Running,
    Stopping
};

// Rendering mode
enum class RenderMode : uint8_t {
    Direct = 0,         // Phase 1: Direct M5GFX calls
    DoubleBuffered      // Phase 2: Double buffer + DMA
};

// ============================================================================
// Renderer Statistics
// ============================================================================

struct RendererStats {
    uint32_t commandsProcessed;     // Total commands executed
    uint32_t framesRendered;        // EndFrame commands seen
    uint32_t syncCommands;          // Sync commands processed
    uint32_t maxBatchSize;          // Largest batch processed

    uint32_t totalRenderTimeUs;     // Cumulative render time
    uint32_t maxRenderTimeUs;       // Worst-case render time
    uint32_t lastRenderTimeUs;      // Most recent render time

    uint32_t displayFlushCount;     // Times display() was called
    uint32_t idleTimeMs;            // Time spent waiting for commands

    void reset() {
        commandsProcessed = 0;
        framesRendered = 0;
        syncCommands = 0;
        maxBatchSize = 0;
        totalRenderTimeUs = 0;
        maxRenderTimeUs = 0;
        lastRenderTimeUs = 0;
        displayFlushCount = 0;
        idleTimeMs = 0;
    }

    // Calculate average render time per command
    uint32_t avgRenderTimeUs() const {
        return commandsProcessed > 0 ? totalRenderTimeUs / commandsProcessed : 0;
    }

    // Calculate average FPS
    float fps() const {
        if (framesRendered == 0 || totalRenderTimeUs == 0) return 0.0f;
        return framesRendered * 1000000.0f / totalRenderTimeUs;
    }
};

// ============================================================================
// Renderer Class
// ============================================================================

class Renderer {
public:
    // Singleton access
    static Renderer& instance();

    // ========================================================================
    // Lifecycle
    // ========================================================================

    // Initialize renderer (must be called after M5.begin())
    bool init();

    // Start the render task
    bool start();

    // Stop the render task
    void stop();

    // Shutdown completely
    void shutdown();

    // Get current state
    RendererState getState() const { return m_state; }

    // Check if running
    bool isRunning() const { return m_state == RendererState::Running; }

    // ========================================================================
    // Configuration
    // ========================================================================

    // Set batch size (commands to process before display())
    void setBatchSize(size_t size) { m_batchSize = size; }
    size_t getBatchSize() const { return m_batchSize; }

    // Set auto-flush mode (flush display after each batch)
    void setAutoFlush(bool enabled) { m_autoFlush = enabled; }
    bool getAutoFlush() const { return m_autoFlush; }

    // Set task priority (requires restart to take effect)
    void setTaskPriority(int priority) { m_taskPriority = priority; }
    int getTaskPriority() const { return m_taskPriority; }

    // Set rendering mode (Phase 1: Direct, Phase 2: DoubleBuffered)
    void setRenderMode(RenderMode mode) { m_renderMode = mode; }
    RenderMode getRenderMode() const { return m_renderMode; }

    // Check if using double buffering
    bool isDoubleBuffered() const { return m_renderMode == RenderMode::DoubleBuffered; }

    // ========================================================================
    // Statistics
    // ========================================================================

    // Get current statistics
    const RendererStats& getStats() const { return m_stats; }

    // Reset statistics
    void resetStats() { m_stats.reset(); }

    // ========================================================================
    // Display State Access (for compatibility)
    // ========================================================================

    // Get current display dimensions
    uint16_t displayWidth() const { return m_displayWidth; }
    uint16_t displayHeight() const { return m_displayHeight; }

    // Get reference to RenderQueue
    RenderQueue& queue() { return RenderQueue::instance(); }

private:
    Renderer();
    ~Renderer();

    // Prevent copying
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // ========================================================================
    // Internal Methods
    // ========================================================================

    // Main render loop (runs in task context)
    void renderLoop();

    // Process a single render command
    void executeCommand(const RenderOp& op);

    // Command handlers - Basic primitives
    void handleFillRect(const RenderOp& op);
    void handleDrawRect(const RenderOp& op);
    void handleDrawLine(const RenderOp& op);
    void handleDrawPixel(const RenderOp& op);

    // Command handlers - Extended primitives
    void handleDrawCircle(const RenderOp& op);
    void handleFillCircle(const RenderOp& op);
    void handleDrawRoundRect(const RenderOp& op);
    void handleFillRoundRect(const RenderOp& op);
    void handleDrawTriangle(const RenderOp& op);
    void handleFillTriangle(const RenderOp& op);

    // Command handlers - Text
    void handleDrawText(const RenderOp& op);
    void handleDrawChar(const RenderOp& op);

    // Command handlers - Images
    void handleDrawBitmap(const RenderOp& op);
    void handleDrawJpeg(const RenderOp& op);

    // Command handlers - Display control
    void handleSetClip(const RenderOp& op);
    void handleClearClip(const RenderOp& op);
    void handleClear(const RenderOp& op);
    void handleFillScreen(const RenderOp& op);
    void handleSetBrightness(const RenderOp& op);
    void handleScroll(const RenderOp& op);
    void handleSync(const RenderOp& op);
    void handleEndFrame(const RenderOp& op);

    // Flush display to screen
    void flushDisplay();

    // Phase 2: Framebuffer rendering
    void executeCommandToFramebuffer(const RenderOp& op);
    void handleFillRectFB(const RenderOp& op);
    void handleDrawLineFB(const RenderOp& op);
    void handleDrawPixelFB(const RenderOp& op);
    void handleClearFB(const RenderOp& op);

    // Static task entry point
    static void taskEntry(void* param);

    // ========================================================================
    // State
    // ========================================================================

    RendererState m_state;
    TaskHandle_t m_taskHandle;

    // Configuration
    size_t m_batchSize;
    bool m_autoFlush;
    int m_taskPriority;
    RenderMode m_renderMode;

    // Display info
    uint16_t m_displayWidth;
    uint16_t m_displayHeight;

    // Current render state
    Rect m_clipRect;
    bool m_clipEnabled;

    // Statistics
    RendererStats m_stats;

    // Run flag for task loop
    volatile bool m_running;
};

// ============================================================================
// Convenience Functions (for simple API)
// ============================================================================

// Initialize GUI system (queue + renderer)
bool guiInit();

// Start GUI rendering
bool guiStart();

// Stop GUI rendering
void guiStop();

// Shutdown GUI system
void guiShutdown();

// Check if GUI is running
bool guiIsRunning();

// Get renderer instance
inline Renderer& renderer() { return Renderer::instance(); }

// Get queue instance
inline RenderQueue& renderQueue() { return RenderQueue::instance(); }

// ============================================================================
// Quick Draw Functions (push commands directly to queue)
// ============================================================================

namespace Draw {

    // Fill rectangle
    inline bool fillRect(int16_t x, int16_t y, uint16_t w, uint16_t h, Color color) {
        return renderQueue().push(RenderOps::fillRect(x, y, w, h, color));
    }

    // Draw rectangle outline
    inline bool drawRect(int16_t x, int16_t y, uint16_t w, uint16_t h, Color color) {
        return renderQueue().push(RenderOps::drawRect(x, y, w, h, color));
    }

    // Draw line
    inline bool drawLine(int16_t x1, int16_t y1, int16_t x2, int16_t y2, Color color) {
        return renderQueue().push(RenderOps::drawLine(x1, y1, x2, y2, color));
    }

    // Draw pixel
    inline bool drawPixel(int16_t x, int16_t y, Color color) {
        return renderQueue().push(RenderOps::drawPixel(x, y, color));
    }

    // Draw text
    inline bool drawText(int16_t x, int16_t y, const char* text, Color fg, Color bg = Colors::Black) {
        return renderQueue().push(RenderOps::drawText(x, y, text, fg, bg));
    }

    // Clear screen
    inline bool clear(Color color = Colors::Black) {
        return renderQueue().push(RenderOps::clear(color));
    }

    // Fill entire screen
    inline bool fillScreen(Color color) {
        return renderQueue().push(RenderOps::fillScreen(color));
    }

    // Set brightness
    inline bool setBrightness(uint8_t level) {
        return renderQueue().push(RenderOps::setBrightness(level));
    }

    // End frame (hint for vsync/buffer swap)
    inline bool endFrame() {
        return renderQueue().push(RenderOps::endFrame());
    }

    // Synchronize (wait for all commands to complete)
    inline void sync() {
        renderQueue().sync();
    }

    // ========================================================================
    // Extended Drawing Primitives
    // ========================================================================

    // Draw circle outline
    inline bool drawCircle(int16_t x, int16_t y, int16_t r, Color color) {
        return renderQueue().push(RenderOps::drawCircle(x, y, r, color));
    }

    // Fill circle
    inline bool fillCircle(int16_t x, int16_t y, int16_t r, Color color) {
        return renderQueue().push(RenderOps::fillCircle(x, y, r, color));
    }

    // Draw rounded rectangle outline
    inline bool drawRoundRect(int16_t x, int16_t y, uint16_t w, uint16_t h, int16_t r, Color color) {
        return renderQueue().push(RenderOps::drawRoundRect(x, y, w, h, r, color));
    }

    // Fill rounded rectangle
    inline bool fillRoundRect(int16_t x, int16_t y, uint16_t w, uint16_t h, int16_t r, Color color) {
        return renderQueue().push(RenderOps::fillRoundRect(x, y, w, h, r, color));
    }

    // Draw triangle outline
    inline bool drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, Color color) {
        return renderQueue().push(RenderOps::drawTriangle(x0, y0, x1, y1, x2, y2, color));
    }

    // Fill triangle
    inline bool fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, Color color) {
        return renderQueue().push(RenderOps::fillTriangle(x0, y0, x1, y1, x2, y2, color));
    }

    // Scroll display content
    inline bool scroll(int16_t dx, int16_t dy) {
        return renderQueue().push(RenderOps::scroll(dx, dy));
    }

} // namespace Draw

} // namespace GUI

#endif // GUI_RENDERER_H
