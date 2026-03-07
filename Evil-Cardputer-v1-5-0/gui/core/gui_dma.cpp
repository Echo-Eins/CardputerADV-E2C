/*
 * GUI DMA Implementation - Asynchronous display transfer
 *
 * Integration with M5GFX:
 * - M5GFX uses its own SPI bus configuration
 * - We use M5GFX's pushImage/pushImageDMA for actual transfers
 * - This wrapper provides async semantics and double-buffer coordination
 *
 * Transfer strategy:
 * - Full frame: pushImageDMA for entire 240x135 buffer
 * - Partial: setWindow + pushImageDMA for dirty regions
 *
 * Performance characteristics:
 * - SPI clock: 40MHz (M5GFX default for ST7789V)
 * - Full frame transfer: ~13ms theoretical (64,800 * 8 / 40,000,000)
 * - Actual: ~15-20ms including setup overhead
 */

#include "gui_dma.h"
#include <M5Unified.h>
#include <Arduino.h>
#include <esp_timer.h>
#include <algorithm>
#include <cstring>

namespace GUI {

// ============================================================================
// DmaTransfer Singleton
// ============================================================================

DmaTransfer& DmaTransfer::instance() {
    static DmaTransfer instance;
    return instance;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

DmaTransfer::DmaTransfer()
    : m_state(DmaState::Idle)
    , m_completeSemaphore(nullptr)
    , m_callback(nullptr)
    , m_callbackUserData(nullptr)
    , m_transferStartUs(0)
    , m_enabled(true)
    , m_dmaAvailable(false)
    , m_initialized(false)
{
    m_stats.reset();
}

DmaTransfer::~DmaTransfer() {
    shutdown();
}

// ============================================================================
// Initialization
// ============================================================================

bool DmaTransfer::init() {
    if (m_initialized) {
        return true;
    }

    // Create completion semaphore
    m_completeSemaphore = xSemaphoreCreateBinary();
    if (!m_completeSemaphore) {
        GUI_LOG_ERROR("Failed to create DMA completion semaphore");
        return false;
    }

    // Give semaphore initially (no transfer in progress)
    xSemaphoreGive(m_completeSemaphore);

    // Check if M5GFX supports DMA
    // M5GFX on ESP32-S3 with ST7789V supports DMA transfers
    m_dmaAvailable = true;  // Assume available; M5GFX handles fallback internally

    m_initialized = true;
    m_state.store(DmaState::Idle, std::memory_order_release);

    GUI_LOG("DMA transfer initialized (available: %d)", m_dmaAvailable);
    return true;
}

void DmaTransfer::shutdown() {
    if (!m_initialized) {
        return;
    }

    // Wait for any pending transfer
    waitComplete(1000);

    if (m_completeSemaphore) {
        vSemaphoreDelete(m_completeSemaphore);
        m_completeSemaphore = nullptr;
    }

    m_initialized = false;
    m_state.store(DmaState::Idle, std::memory_order_release);

    GUI_LOG("DMA transfer shutdown");
}

// ============================================================================
// Transfer Operations
// ============================================================================

bool DmaTransfer::startFullTransfer(const uint16_t* buffer, uint32_t size) {
    if (!m_initialized || !buffer) {
        return false;
    }

    // Check if already transferring
    DmaState expected = DmaState::Idle;
    if (!m_state.compare_exchange_strong(expected, DmaState::Preparing,
                                          std::memory_order_acq_rel)) {
        GUI_LOG_ERROR("DMA transfer already in progress");
        return false;
    }

    // Take semaphore (will block if previous transfer not complete)
    if (xSemaphoreTake(m_completeSemaphore, pdMS_TO_TICKS(100)) != pdTRUE) {
        m_state.store(DmaState::Idle, std::memory_order_release);
        m_stats.errorCount++;
        return false;
    }

    m_transferStartUs = esp_timer_get_time();

    // Use M5GFX DMA transfer
    // M5GFX handles the actual DMA setup internally
    m_state.store(DmaState::Transferring, std::memory_order_release);

    // Notify framebuffer that DMA started
    Framebuffer::instance().markDmaStarted();

    // Start the transfer
    // M5GFX.Display.pushImageDMA is async on ESP32 when DMA is available
    // For full frame, we set window to entire screen first
    M5.Display.setAddrWindow(0, 0, Config::DISPLAY_WIDTH, Config::DISPLAY_HEIGHT);
    M5.Display.pushPixelsDMA(buffer, Config::DISPLAY_WIDTH * Config::DISPLAY_HEIGHT);

    // Note: M5GFX's pushPixelsDMA returns after queuing, not after completion
    // We need to wait for it to finish before allowing buffer swap

    // For now, we wait synchronously because M5GFX doesn't provide
    // a true async completion callback
    M5.Display.waitDMA();

    // Transfer complete
    handleTransferComplete(true);

    return true;
}

bool DmaTransfer::startPartialTransfer(const uint16_t* buffer,
                                        int16_t x, int16_t y,
                                        uint16_t width, uint16_t height) {
    if (!m_initialized || !buffer) {
        return false;
    }

    // Check if already transferring
    DmaState expected = DmaState::Idle;
    if (!m_state.compare_exchange_strong(expected, DmaState::Preparing,
                                          std::memory_order_acq_rel)) {
        return false;
    }

    if (xSemaphoreTake(m_completeSemaphore, pdMS_TO_TICKS(100)) != pdTRUE) {
        m_state.store(DmaState::Idle, std::memory_order_release);
        m_stats.errorCount++;
        return false;
    }

    m_transferStartUs = esp_timer_get_time();
    m_state.store(DmaState::Transferring, std::memory_order_release);

    Framebuffer::instance().markDmaStarted();

    // Set window and transfer
    M5.Display.setAddrWindow(x, y, width, height);
    M5.Display.pushPixelsDMA(buffer, width * height);
    M5.Display.waitDMA();

    handleTransferComplete(true);
    return true;
}

void DmaTransfer::waitComplete(uint32_t timeoutMs) {
    if (!m_initialized) {
        return;
    }

    DmaState currentState = m_state.load(std::memory_order_acquire);
    if (currentState == DmaState::Idle) {
        return;  // Nothing to wait for
    }

    // Wait for semaphore and immediately give it back
    if (xSemaphoreTake(m_completeSemaphore, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
        xSemaphoreGive(m_completeSemaphore);
    }
}

void DmaTransfer::handleTransferComplete(bool success) {
    uint32_t elapsedUs = esp_timer_get_time() - m_transferStartUs;

    // Update statistics
    if (success) {
        m_stats.transferCount++;
        m_stats.bytesTransferred += Config::FRAMEBUFFER_SIZE;
        m_stats.totalTransferTimeUs += elapsedUs;
        if (elapsedUs > m_stats.maxTransferTimeUs) {
            m_stats.maxTransferTimeUs = elapsedUs;
        }
    } else {
        m_stats.errorCount++;
    }

    // Notify framebuffer that DMA completed
    Framebuffer::instance().markDmaCompleted();

    // Update state
    m_state.store(DmaState::Idle, std::memory_order_release);

    // Release semaphore
    xSemaphoreGive(m_completeSemaphore);

    // Call user callback if set
    if (m_callback) {
        m_callback(success, m_callbackUserData);
    }
}

void DmaTransfer::setCallback(TransferCallback callback, void* userData) {
    m_callback = callback;
    m_callbackUserData = userData;
}

// ============================================================================
// Blocking Transfers (Fallback)
// ============================================================================

void DmaTransfer::blockingTransfer(const uint16_t* buffer, uint32_t size) {
    if (!buffer) return;

    uint32_t startUs = esp_timer_get_time();

    // Use M5GFX blocking transfer
    M5.Display.setAddrWindow(0, 0, Config::DISPLAY_WIDTH, Config::DISPLAY_HEIGHT);
    M5.Display.pushPixels(buffer, Config::DISPLAY_WIDTH * Config::DISPLAY_HEIGHT);

    uint32_t elapsedUs = esp_timer_get_time() - startUs;

    // Update statistics
    m_stats.transferCount++;
    m_stats.bytesTransferred += size;
    m_stats.totalTransferTimeUs += elapsedUs;
    if (elapsedUs > m_stats.maxTransferTimeUs) {
        m_stats.maxTransferTimeUs = elapsedUs;
    }
}

void DmaTransfer::blockingPartialTransfer(const uint16_t* buffer,
                                           int16_t x, int16_t y,
                                           uint16_t width, uint16_t height) {
    if (!buffer) return;

    uint32_t startUs = esp_timer_get_time();

    M5.Display.setAddrWindow(x, y, width, height);
    M5.Display.pushPixels(buffer, width * height);

    uint32_t elapsedUs = esp_timer_get_time() - startUs;
    uint32_t size = width * height * 2;

    m_stats.transferCount++;
    m_stats.bytesTransferred += size;
    m_stats.totalTransferTimeUs += elapsedUs;
    if (elapsedUs > m_stats.maxTransferTimeUs) {
        m_stats.maxTransferTimeUs = elapsedUs;
    }
}

// ============================================================================
// DisplayUpdater Singleton
// ============================================================================

DisplayUpdater& DisplayUpdater::instance() {
    static DisplayUpdater instance;
    return instance;
}

DisplayUpdater::DisplayUpdater()
    : m_useDma(true)
    , m_useDoubleBuffer(true)
    , m_initialized(false)
{
}

DisplayUpdater::~DisplayUpdater() {
    shutdown();
}

bool DisplayUpdater::init() {
    if (m_initialized) {
        return true;
    }

    // Framebuffer and DmaTransfer should already be initialized
    if (!Framebuffer::instance().isInitialized()) {
        GUI_LOG_ERROR("DisplayUpdater: Framebuffer not initialized");
        return false;
    }

    if (!DmaTransfer::instance().isInitialized()) {
        GUI_LOG_ERROR("DisplayUpdater: DmaTransfer not initialized");
        return false;
    }

    m_initialized = true;
    GUI_LOG("DisplayUpdater initialized (DMA: %d, DoubleBuffer: %d)",
            m_useDma, m_useDoubleBuffer);
    return true;
}

void DisplayUpdater::shutdown() {
    m_initialized = false;
}

void DisplayUpdater::pushFramebuffer() {
    if (!m_initialized) return;

    Framebuffer& fb = Framebuffer::instance();
    DmaTransfer& dma = DmaTransfer::instance();

    // Get buffer to transfer (front buffer if double-buffered)
    const uint16_t* buffer = m_useDoubleBuffer ? fb.getFrontBuffer() : fb.getBackBuffer();
    uint32_t size = fb.getBufferSize();

    if (m_useDma && dma.isAvailable()) {
        // Async DMA transfer
        dma.startFullTransfer(buffer, size);
    } else {
        // Blocking transfer
        dma.blockingTransfer(buffer, size);
    }
}

void DisplayUpdater::pushFramebufferSync() {
    pushFramebuffer();

    // Wait for completion
    if (m_useDma) {
        DmaTransfer::instance().waitComplete();
    }
}

void DisplayUpdater::pushRegion(const Rect& region) {
    if (!m_initialized || region.isEmpty()) return;

    Framebuffer& fb = Framebuffer::instance();
    DmaTransfer& dma = DmaTransfer::instance();

    // Clamp region to display bounds
    int16_t x = std::max<int16_t>(0, region.x);
    int16_t y = std::max<int16_t>(0, region.y);

    // Bail out if origin is already beyond the screen — prevents
    // unsigned underflow in the subtraction below.
    if (x >= Config::DISPLAY_WIDTH || y >= Config::DISPLAY_HEIGHT) return;

    uint16_t w = std::min<uint16_t>(region.width, Config::DISPLAY_WIDTH - x);
    uint16_t h = std::min<uint16_t>(region.height, Config::DISPLAY_HEIGHT - y);

    if (w == 0 || h == 0) return;

    // If region covers most of the screen, just do a full transfer
    uint32_t regionPixels = static_cast<uint32_t>(w) * h;
    uint32_t totalPixels = Config::DISPLAY_WIDTH * Config::DISPLAY_HEIGHT;
    if (regionPixels > totalPixels * 3 / 4) {
        pushFramebuffer();
        return;
    }

    // Extract the region into a contiguous buffer for DMA transfer.
    // The framebuffer is laid out as full-width rows, so a sub-rectangle
    // is not contiguous in memory — we must copy row by row.
    uint32_t regionBytes = regionPixels * sizeof(uint16_t);
    uint16_t* tempBuffer = static_cast<uint16_t*>(
        heap_caps_malloc(regionBytes, MALLOC_CAP_DMA | MALLOC_CAP_32BIT)
    );

    if (!tempBuffer) {
        // Allocation failed — fall back to full frame transfer
        pushFramebuffer();
        return;
    }

    const uint16_t* srcBuffer = m_useDoubleBuffer
        ? fb.getFrontBuffer() : fb.getBackBuffer();
    const uint16_t* srcRow = srcBuffer + y * Config::DISPLAY_WIDTH + x;

    for (uint16_t row = 0; row < h; row++) {
        memcpy(tempBuffer + row * w, srcRow, w * sizeof(uint16_t));
        srcRow += Config::DISPLAY_WIDTH;
    }

    // Transfer the contiguous region buffer to the display
    if (m_useDma && dma.isAvailable()) {
        dma.startPartialTransfer(tempBuffer, x, y, w, h);
    } else {
        dma.blockingPartialTransfer(tempBuffer, x, y, w, h);
    }

    heap_caps_free(tempBuffer);
}

} // namespace GUI
