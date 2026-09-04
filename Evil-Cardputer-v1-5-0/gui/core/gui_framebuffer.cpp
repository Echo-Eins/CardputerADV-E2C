/*
 * GUI Framebuffer Implementation - Double-buffered PSRAM management
 *
 * Memory allocation strategy:
 * - Primary: PSRAM (8MB available, fast enough for display)
 * - Fallback: Internal RAM (if PSRAM not available)
 *
 * Performance optimizations:
 * - 32-bit aligned memory access
 * - Horizontal line fill using memset (when color bytes are equal)
 * - DMA-safe memory alignment
 */

#include "gui_framebuffer.h"
#include "gui_dma.h"
#include "gui_display_target.h"
#if GUI_DIRTY_TRACKING
#include "gui_dirty_region.h"
#endif
#include <cstring>
#include <algorithm>
#include <Arduino.h>
#include <esp_timer.h>

namespace GUI {

portMUX_TYPE Framebuffer::s_statsLock = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// Singleton Instance
// ============================================================================

Framebuffer& Framebuffer::instance() {
    static Framebuffer instance;
    return instance;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

Framebuffer::Framebuffer()
    : m_buffer0(nullptr)
    , m_buffer1(nullptr)
    , m_frontBuffer(nullptr)
    , m_backBuffer(nullptr)
    , m_clipRect(Rect::make(0, 0, 0, 0))
    , m_dmaActive(false)
    , m_mutex(nullptr)
    , m_initialized(false)
{
    refreshRuntimeDisplayMetrics();
    const uint16_t defaultWidth = runtimeDisplayWidth() > 0
        ? runtimeDisplayWidth() : Config::DISPLAY_WIDTH;
    const uint16_t defaultHeight = runtimeDisplayHeight() > 0
        ? runtimeDisplayHeight() : Config::DISPLAY_HEIGHT;

    // Default config
    m_config.width = defaultWidth;
    m_config.height = defaultHeight;
    m_config.bitsPerPixel = 16;
    m_config.useDoubleBuffer = true;
    m_config.usePSRAM = true;

    m_stats.reset();
}

Framebuffer::~Framebuffer() {
    shutdown();
}

// ============================================================================
// Initialization
// ============================================================================

bool Framebuffer::init() {
    refreshRuntimeDisplayMetrics();
    const uint16_t defaultWidth = runtimeDisplayWidth() > 0
        ? runtimeDisplayWidth() : Config::DISPLAY_WIDTH;
    const uint16_t defaultHeight = runtimeDisplayHeight() > 0
        ? runtimeDisplayHeight() : Config::DISPLAY_HEIGHT;

    FramebufferConfig defaultConfig;
    defaultConfig.width = defaultWidth;
    defaultConfig.height = defaultHeight;
    defaultConfig.bitsPerPixel = 16;
    defaultConfig.useDoubleBuffer = true;
    defaultConfig.usePSRAM = true;

    return init(defaultConfig);
}

bool Framebuffer::init(const FramebufferConfig& config) {
    if (m_initialized) {
        GUI_LOG("Framebuffer already initialized");
        return true;
    }

    m_config = config;
    uint32_t bufferSize = m_config.bufferSize();

    GUI_LOG("Framebuffer init: %dx%d, %d bpp, size=%lu bytes",
            m_config.width, m_config.height, m_config.bitsPerPixel, bufferSize);

    // Allocate first buffer
    m_buffer0 = allocateBuffer(bufferSize, m_config.usePSRAM);
    if (!m_buffer0) {
        GUI_LOG_ERROR("Failed to allocate buffer 0");
        return false;
    }

    // Allocate second buffer if double-buffering enabled
    if (m_config.useDoubleBuffer) {
        m_buffer1 = allocateBuffer(bufferSize, m_config.usePSRAM);
        if (!m_buffer1) {
            GUI_LOG_ERROR("Failed to allocate buffer 1, falling back to single buffer");
            m_config.useDoubleBuffer = false;
        }
    }

    // Set up front/back pointers
    m_backBuffer = m_buffer0;
    m_frontBuffer = m_config.useDoubleBuffer ? m_buffer1 : m_buffer0;

    // Clear both buffers
    memset(m_buffer0, 0, bufferSize);
    if (m_buffer1) {
        memset(m_buffer1, 0, bufferSize);
    }

    // Initialize clip rect to full screen
    m_clipRect = Rect::make(0, 0, m_config.width, m_config.height);

    // Create mutex for cross-core buffer access safety
    m_mutex = xSemaphoreCreateMutex();
    if (!m_mutex) {
        GUI_LOG_ERROR("Failed to create Framebuffer mutex");
        freeBuffer(m_buffer0);
        m_buffer0 = nullptr;
        if (m_buffer1) { freeBuffer(m_buffer1); m_buffer1 = nullptr; }
        return false;
    }

    m_initialized = true;

    GUI_LOG("Framebuffer initialized: double=%d, PSRAM=%d",
            m_config.useDoubleBuffer, m_config.usePSRAM);

    return true;
}

void Framebuffer::shutdown() {
    if (!m_initialized) {
        return;
    }

    // Wait for DMA hardware to finish (not just the atomic flag).
    // DmaTransfer::waitComplete() blocks on the semaphore that the
    // actual SPI DMA transfer releases, so buffers are safe to free.
    DmaTransfer::instance().waitComplete(2000);
    waitForDma();

    // Free buffers
    if (m_buffer0) {
        freeBuffer(m_buffer0);
        m_buffer0 = nullptr;
    }
    if (m_buffer1) {
        freeBuffer(m_buffer1);
        m_buffer1 = nullptr;
    }

    m_frontBuffer = nullptr;
    m_backBuffer = nullptr;

    if (m_mutex) {
        vSemaphoreDelete(m_mutex);
        m_mutex = nullptr;
    }

    m_initialized = false;

    GUI_LOG("Framebuffer shutdown");
}

// ============================================================================
// Memory Allocation
// ============================================================================

uint16_t* Framebuffer::allocateBuffer(uint32_t size, bool usePSRAM) {
    uint16_t* buffer = nullptr;

    if (usePSRAM) {
        // Try PSRAM first (DMA-capable, 32-bit aligned)
        buffer = static_cast<uint16_t*>(
            heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_32BIT)
        );

        if (!buffer) {
            // Try PSRAM without DMA requirement
            buffer = static_cast<uint16_t*>(
                heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT)
            );
        }
    }

    if (!buffer) {
        // Fallback to internal DMA-capable memory
        buffer = static_cast<uint16_t*>(
            heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_32BIT)
        );
    }

    if (!buffer) {
        // Last resort: any available memory
        buffer = static_cast<uint16_t*>(malloc(size));
    }

    if (buffer) {
        GUI_LOG("Allocated %lu bytes at %p (PSRAM: %d)",
                size, buffer, usePSRAM && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0);
    }

    return buffer;
}

void Framebuffer::freeBuffer(uint16_t* buffer) {
    if (buffer) {
        heap_caps_free(buffer);
    }
}

// ============================================================================
// Buffer Swap
// ============================================================================

void Framebuffer::swap() {
    if (!m_initialized || !m_config.useDoubleBuffer) {
        return;
    }

    uint32_t startUs = esp_timer_get_time();

    // Lock mutex to make DMA-active check + pointer swap atomic.
    // Without this, DMA could start between the check and the swap,
    // causing use-after-free on the front buffer.
    xSemaphoreTake(m_mutex, portMAX_DELAY);

    // Wait for DMA to finish with front buffer while holding mutex
    while (m_dmaActive.load(std::memory_order_acquire)) {
        // Release mutex while waiting so DMA completion can proceed
        xSemaphoreGive(m_mutex);
        vTaskDelay(1);
        xSemaphoreTake(m_mutex, portMAX_DELAY);
        m_stats.dmaWaitCount++;
    }

    // Swap pointers (protected by mutex — DMA cannot start here)
    uint16_t* temp = m_frontBuffer;
    m_frontBuffer = m_backBuffer;
    m_backBuffer = temp;

    xSemaphoreGive(m_mutex);

    // Update statistics
    uint32_t elapsedUs = esp_timer_get_time() - startUs;
    portENTER_CRITICAL(&s_statsLock);
    m_stats.swapCount++;
    m_stats.totalSwapTimeUs += elapsedUs;
    if (elapsedUs > m_stats.maxSwapTimeUs) {
        m_stats.maxSwapTimeUs = elapsedUs;
    }
    portEXIT_CRITICAL(&s_statsLock);
}

void Framebuffer::syncBackFromFront(const Rect& region) {
    if (!m_initialized || !m_config.useDoubleBuffer ||
        !m_frontBuffer || !m_backBuffer || region.isEmpty()) {
        return;
    }

    const Rect bounds = Rect::make(0, 0, m_config.width, m_config.height);
    const Rect clipped = region.intersection(bounds);
    if (clipped.isEmpty()) {
        return;
    }

    xSemaphoreTake(m_mutex, portMAX_DELAY);
    const uint16_t* src = m_frontBuffer + clipped.y * m_config.width + clipped.x;
    uint16_t* dst = m_backBuffer + clipped.y * m_config.width + clipped.x;
    const size_t rowBytes = static_cast<size_t>(clipped.width) * sizeof(uint16_t);
    for (uint16_t row = 0; row < clipped.height; ++row) {
        memcpy(dst, src, rowBytes);
        src += m_config.width;
        dst += m_config.width;
    }
    xSemaphoreGive(m_mutex);
}

// ============================================================================
// Drawing Operations
// ============================================================================

void Framebuffer::clear(Color color) {
    if (!m_initialized) return;

    // Optimize for common case where both bytes of color are the same
    uint8_t highByte = (color >> 8) & 0xFF;
    uint8_t lowByte = color & 0xFF;

    if (highByte == lowByte) {
        // Can use memset (fast)
        memset(m_backBuffer, lowByte, m_config.bufferSize());
    } else {
        // Fill pixel by pixel
        uint32_t pixelCount = getPixelCount();
        for (uint32_t i = 0; i < pixelCount; i++) {
            m_backBuffer[i] = color;
        }
    }

#if GUI_DIRTY_TRACKING
    // Mark entire screen dirty
    DirtyRegionTracker::instance().markAllDirty();
#endif
}

void Framebuffer::fillRect(int16_t x, int16_t y, uint16_t w, uint16_t h, Color color) {
    if (!m_initialized) return;

    // Clip to screen bounds
    if (!clipCoords(x, y, w, h)) return;

    // Fill line by line
    uint16_t* dst = m_backBuffer + y * m_config.width + x;
    uint32_t stride = m_config.width;

    for (uint16_t row = 0; row < h; row++) {
        // Fill this row
        for (uint16_t col = 0; col < w; col++) {
            dst[col] = color;
        }
        dst += stride;
    }

#if GUI_DIRTY_TRACKING
    // Mark affected region dirty
    DirtyRegionTracker::instance().markDirty(x, y, w, h);
#endif
}

void Framebuffer::drawHLine(int16_t x, int16_t y, uint16_t w, Color color) {
    if (!m_initialized) return;

    // Clip Y
    if (y < m_clipRect.y || y >= m_clipRect.bottom()) return;

    // Clip X
    int16_t x2 = x + w;
    if (x < m_clipRect.x) x = m_clipRect.x;
    if (x2 > m_clipRect.right()) x2 = m_clipRect.right();
    if (x >= x2) return;

    w = x2 - x;

    // Draw line
    uint16_t* dst = m_backBuffer + y * m_config.width + x;
    for (uint16_t i = 0; i < w; i++) {
        dst[i] = color;
    }

#if GUI_DIRTY_TRACKING
    DirtyRegionTracker::instance().markDirty(x, y, w, 1);
#endif
}

void Framebuffer::drawVLine(int16_t x, int16_t y, uint16_t h, Color color) {
    if (!m_initialized) return;

    // Clip X
    if (x < m_clipRect.x || x >= m_clipRect.right()) return;

    // Clip Y
    int16_t y2 = y + h;
    if (y < m_clipRect.y) y = m_clipRect.y;
    if (y2 > m_clipRect.bottom()) y2 = m_clipRect.bottom();
    if (y >= y2) return;

    h = y2 - y;

    // Draw line
    uint16_t* dst = m_backBuffer + y * m_config.width + x;
    uint32_t stride = m_config.width;
    for (uint16_t i = 0; i < h; i++) {
        *dst = color;
        dst += stride;
    }

#if GUI_DIRTY_TRACKING
    DirtyRegionTracker::instance().markDirty(x, y, 1, h);
#endif
}

void Framebuffer::copyRect(int16_t destX, int16_t destY,
                           const uint16_t* src, uint16_t srcW, uint16_t srcH,
                           int16_t srcX, int16_t srcY) {
    if (!m_initialized || !src) return;

    // Validate source coordinates before subtraction to prevent unsigned underflow
    if (srcX < 0 || srcY < 0 || srcX >= static_cast<int16_t>(srcW) || srcY >= static_cast<int16_t>(srcH)) return;

    // Calculate actual copy region (safe: srcX < srcW and srcY < srcH guaranteed)
    int16_t copyW = static_cast<int16_t>(srcW) - srcX;
    int16_t copyH = static_cast<int16_t>(srcH) - srcY;

    // Clip to clip rect
    if (destX < m_clipRect.x) {
        int16_t diff = m_clipRect.x - destX;
        srcX += diff;
        copyW -= diff;
        destX = m_clipRect.x;
    }
    if (destY < m_clipRect.y) {
        int16_t diff = m_clipRect.y - destY;
        srcY += diff;
        copyH -= diff;
        destY = m_clipRect.y;
    }
    if (destX + copyW > m_clipRect.right()) {
        copyW = m_clipRect.right() - destX;
    }
    if (destY + copyH > m_clipRect.bottom()) {
        copyH = m_clipRect.bottom() - destY;
    }

    if (copyW <= 0 || copyH <= 0) return;

    // Copy line by line
    const uint16_t* srcPtr = src + srcY * srcW + srcX;
    uint16_t* dstPtr = m_backBuffer + destY * m_config.width + destX;

    for (int16_t row = 0; row < copyH; row++) {
        memcpy(dstPtr, srcPtr, static_cast<uint16_t>(copyW) * sizeof(uint16_t));
        srcPtr += srcW;
        dstPtr += m_config.width;
    }

#if GUI_DIRTY_TRACKING
    DirtyRegionTracker::instance().markDirty(destX, destY, static_cast<uint16_t>(copyW), static_cast<uint16_t>(copyH));
#endif
}

// ============================================================================
// Clipping
// ============================================================================

void Framebuffer::setClipRect(const Rect& rect) {
    // Constrain to screen bounds
    m_clipRect.x = std::max<int16_t>(0, rect.x);
    m_clipRect.y = std::max<int16_t>(0, rect.y);

    int16_t right = std::min<int16_t>(m_config.width, rect.x + rect.width);
    int16_t bottom = std::min<int16_t>(m_config.height, rect.y + rect.height);

    m_clipRect.width = (right > m_clipRect.x) ? (right - m_clipRect.x) : 0;
    m_clipRect.height = (bottom > m_clipRect.y) ? (bottom - m_clipRect.y) : 0;
}

void Framebuffer::clearClipRect() {
    m_clipRect = Rect::make(0, 0, m_config.width, m_config.height);
}

bool Framebuffer::clipCoords(int16_t& x, int16_t& y, uint16_t& w, uint16_t& h) const {
    // Clip left
    if (x < m_clipRect.x) {
        int16_t diff = m_clipRect.x - x;
        if (diff >= w) return false;
        w -= diff;
        x = m_clipRect.x;
    }

    // Clip top
    if (y < m_clipRect.y) {
        int16_t diff = m_clipRect.y - y;
        if (diff >= h) return false;
        h -= diff;
        y = m_clipRect.y;
    }

    // Clip right
    if (x + w > m_clipRect.right()) {
        w = m_clipRect.right() - x;
    }

    // Clip bottom
    if (y + h > m_clipRect.bottom()) {
        h = m_clipRect.bottom() - y;
    }

    return w > 0 && h > 0;
}

// ============================================================================
// DMA Synchronization
// ============================================================================

void Framebuffer::markDmaStarted() {
    // Hold mutex to prevent swap() from racing between its check and pointer swap
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    m_dmaActive.store(true, std::memory_order_release);
    m_stats.dmaTransferCount++;
    xSemaphoreGive(m_mutex);
}

void Framebuffer::markDmaCompleted() {
    m_dmaActive.store(false, std::memory_order_release);
}

void Framebuffer::waitForDma() {
    while (m_dmaActive.load(std::memory_order_acquire)) {
        vTaskDelay(1);
    }
}

void Framebuffer::lock() {
    if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
}

void Framebuffer::unlock() {
    if (m_mutex) xSemaphoreGive(m_mutex);
}

} // namespace GUI
