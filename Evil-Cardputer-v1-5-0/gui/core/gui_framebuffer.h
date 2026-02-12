/*
 * GUI Framebuffer - Double-buffered framebuffer management in PSRAM
 *
 * Provides two framebuffers for tear-free rendering:
 * - Back buffer: Renderer draws here
 * - Front buffer: DMA transfers this to display
 *
 * Memory allocation:
 * - Uses PSRAM (8MB available on ESP32-S3)
 * - Each buffer: 240 * 135 * 2 = 64,800 bytes
 * - Total: ~130KB for double buffering
 *
 * Buffer swap:
 * - After rendering complete, swap() exchanges front/back pointers
 * - DMA continues transferring from (now) back buffer
 * - Renderer starts drawing to (now) front buffer
 */

#ifndef GUI_FRAMEBUFFER_H
#define GUI_FRAMEBUFFER_H

#include "../gui_types.h"
#include "../gui_config.h"
#include <cstdint>
#include <atomic>

// ESP32 PSRAM allocation
#include "esp_heap_caps.h"

// Phase 3: Dirty region tracking
#if GUI_DIRTY_TRACKING
#include "gui_dirty_region.h"
#endif

namespace GUI {

// ============================================================================
// Framebuffer Configuration
// ============================================================================

struct FramebufferConfig {
    uint16_t width;
    uint16_t height;
    uint8_t bitsPerPixel;       // 16 for RGB565
    bool useDoubleBuffer;
    bool usePSRAM;

    // Calculate buffer size in bytes
    uint32_t bufferSize() const {
        return static_cast<uint32_t>(width) * height * (bitsPerPixel / 8);
    }
};

// ============================================================================
// Framebuffer Statistics
// ============================================================================

struct FramebufferStats {
    uint32_t swapCount;             // Number of buffer swaps
    uint32_t totalSwapTimeUs;       // Cumulative swap time
    uint32_t maxSwapTimeUs;         // Worst-case swap time
    uint32_t dmaTransferCount;      // DMA transfers initiated
    uint32_t dmaWaitCount;          // Times we had to wait for DMA

    void reset() {
        swapCount = 0;
        totalSwapTimeUs = 0;
        maxSwapTimeUs = 0;
        dmaTransferCount = 0;
        dmaWaitCount = 0;
    }

    uint32_t avgSwapTimeUs() const {
        return swapCount > 0 ? totalSwapTimeUs / swapCount : 0;
    }
};

// ============================================================================
// Framebuffer Manager Class
// ============================================================================

class Framebuffer {
public:
    // Singleton access
    static Framebuffer& instance();

    // ========================================================================
    // Lifecycle
    // ========================================================================

    // Initialize framebuffers with given configuration
    bool init(const FramebufferConfig& config);

    // Initialize with default configuration (240x135, RGB565, double-buffered)
    bool init();

    // Release allocated memory
    void shutdown();

    // Check if initialized
    bool isInitialized() const { return m_initialized; }

    // ========================================================================
    // Buffer Access
    // ========================================================================

    // Get pointer to back buffer (for rendering)
    uint16_t* getBackBuffer() { return m_backBuffer; }
    const uint16_t* getBackBuffer() const { return m_backBuffer; }

    // Get pointer to front buffer (for DMA transfer)
    uint16_t* getFrontBuffer() { return m_frontBuffer; }
    const uint16_t* getFrontBuffer() const { return m_frontBuffer; }

    // Get buffer size in bytes
    uint32_t getBufferSize() const { return m_config.bufferSize(); }

    // Get buffer size in pixels
    uint32_t getPixelCount() const { return m_config.width * m_config.height; }

    // ========================================================================
    // Buffer Operations
    // ========================================================================

    // Swap front and back buffers
    // Call this after rendering is complete and before starting DMA
    void swap();

    // Clear back buffer with specified color
    void clear(Color color = Colors::Black);

    // Fill rectangle in back buffer
    void fillRect(int16_t x, int16_t y, uint16_t w, uint16_t h, Color color);

    // Draw horizontal line (optimized)
    void drawHLine(int16_t x, int16_t y, uint16_t w, Color color);

    // Draw vertical line
    void drawVLine(int16_t x, int16_t y, uint16_t h, Color color);

    // Set pixel in back buffer
    void setPixel(int16_t x, int16_t y, Color color);

    // Get pixel from back buffer
    Color getPixel(int16_t x, int16_t y) const;

    // Copy rectangle from source buffer to back buffer
    void copyRect(int16_t destX, int16_t destY,
                  const uint16_t* src, uint16_t srcW, uint16_t srcH,
                  int16_t srcX = 0, int16_t srcY = 0);

    // ========================================================================
    // Clipping
    // ========================================================================

    // Set clipping rectangle (affects all draw operations)
    void setClipRect(const Rect& rect);

    // Clear clipping (full screen)
    void clearClipRect();

    // Get current clip rectangle
    const Rect& getClipRect() const { return m_clipRect; }

    // ========================================================================
    // Configuration Access
    // ========================================================================

    const FramebufferConfig& getConfig() const { return m_config; }
    uint16_t width() const { return m_config.width; }
    uint16_t height() const { return m_config.height; }

    // ========================================================================
    // DMA Synchronization
    // ========================================================================

    // Mark that DMA transfer has started (back buffer is being read)
    void markDmaStarted();

    // Mark that DMA transfer has completed
    void markDmaCompleted();

    // Check if DMA is currently transferring
    bool isDmaActive() const { return m_dmaActive.load(std::memory_order_acquire); }

    // Wait for DMA to complete (blocking)
    void waitForDma();

    // ========================================================================
    // Statistics
    // ========================================================================

    const FramebufferStats& getStats() const { return m_stats; }
    void resetStats() { m_stats.reset(); }

private:
    Framebuffer();
    ~Framebuffer();

    // Prevent copying
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    // Allocate buffer in PSRAM or regular heap
    uint16_t* allocateBuffer(uint32_t size, bool usePSRAM);

    // Free buffer
    void freeBuffer(uint16_t* buffer);

    // Clip coordinates to valid range
    bool clipCoords(int16_t& x, int16_t& y, uint16_t& w, uint16_t& h) const;

    // ========================================================================
    // State
    // ========================================================================

    FramebufferConfig m_config;

    // Double buffer pointers
    uint16_t* m_buffer0;        // First buffer (allocated memory)
    uint16_t* m_buffer1;        // Second buffer (allocated memory)
    uint16_t* m_frontBuffer;    // Points to current front buffer
    uint16_t* m_backBuffer;     // Points to current back buffer

    // Clipping
    Rect m_clipRect;

    // DMA synchronization
    std::atomic<bool> m_dmaActive{false};

    // Statistics
    FramebufferStats m_stats;

    // Initialization flag
    bool m_initialized;
};

// ============================================================================
// Inline Pixel Operations (for performance)
// ============================================================================

inline void Framebuffer::setPixel(int16_t x, int16_t y, Color color) {
    if (x >= m_clipRect.x && x < m_clipRect.right() &&
        y >= m_clipRect.y && y < m_clipRect.bottom()) {
        m_backBuffer[y * m_config.width + x] = color;
#if GUI_DIRTY_TRACKING
        DirtyRegionTracker::instance().markDirty(x, y);
#endif
    }
}

inline Color Framebuffer::getPixel(int16_t x, int16_t y) const {
    if (x >= 0 && x < m_config.width && y >= 0 && y < m_config.height) {
        return m_backBuffer[y * m_config.width + x];
    }
    return 0;
}

} // namespace GUI

#endif // GUI_FRAMEBUFFER_H
