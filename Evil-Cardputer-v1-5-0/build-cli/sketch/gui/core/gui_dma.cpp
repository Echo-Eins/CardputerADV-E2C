#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\core\\gui_dma.cpp"
/*
 * GUI DMA Implementation - display transfer wrapper
 *
 * Integration with M5GFX:
 * - M5GFX uses its own SPI bus configuration
 * - We use M5GFX's pushPixels/pushPixelsDMA for actual transfers
 * - This wrapper provides deterministic transfer semantics and double-buffer coordination
 *
 * Transfer strategy:
 * - Full frame: setWindow + pushPixelsDMA for framebuffer payload
 * - Partial: setWindow + pushPixelsDMA for dirty regions
 *
 * Performance characteristics:
 * - SPI clock: 40MHz (M5GFX default for ST7789V)
 * - Full frame transfer: ~13ms theoretical (64,800 * 8 / 40,000,000)
 * - Actual: ~15-20ms including setup overhead
 */

#include "gui_dma.h"
#include "gui_display_lock.h"
#include <M5Unified.h>
#include <Arduino.h>
#include <esp_timer.h>
#include <algorithm>
#include <cstring>

namespace GUI {

portMUX_TYPE DmaTransfer::s_statsLock = portMUX_INITIALIZER_UNLOCKED;

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

    const uint32_t fullFrameBytes = Config::FRAMEBUFFER_SIZE;
    if (size < fullFrameBytes || (size & 0x1u) != 0) {
        GUI_LOG_ERROR("DMA full transfer invalid size=%lu (expected >=%lu and even)",
                      static_cast<unsigned long>(size),
                      static_cast<unsigned long>(fullFrameBytes));
        portENTER_CRITICAL(&s_statsLock);
        m_stats.errorCount++;
        portEXIT_CRITICAL(&s_statsLock);
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
        portENTER_CRITICAL(&s_statsLock);
        m_stats.errorCount++;
        portEXIT_CRITICAL(&s_statsLock);
        return false;
    }

    m_transferStartUs = esp_timer_get_time();
    m_state.store(DmaState::Transferring, std::memory_order_release);

    const uint32_t pixelCount = fullFrameBytes / sizeof(uint16_t);
    const uint16_t transferWidth = Config::DISPLAY_WIDTH;
    const uint16_t transferHeight = static_cast<uint16_t>(pixelCount / transferWidth);

    {
        DisplayLockGuard displayLock;
        if (!displayLock.locked()) {
            GUI_LOG_ERROR("DMA full transfer failed: display lock not acquired");
            handleTransferComplete(false, 0);
            return false;
        }

        Framebuffer::instance().markDmaStarted();
        M5.Display.setAddrWindow(0, 0, transferWidth, transferHeight);
        M5.Display.pushPixelsDMA(buffer, pixelCount);

        // Deterministic completion barrier. Keep ownership simple.
        M5.Display.waitDMA();
    }

    handleTransferComplete(true, fullFrameBytes);

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
        portENTER_CRITICAL(&s_statsLock);
        m_stats.errorCount++;
        portEXIT_CRITICAL(&s_statsLock);
        return false;
    }

    m_transferStartUs = esp_timer_get_time();
    m_state.store(DmaState::Transferring, std::memory_order_release);

    // Set window and transfer
    uint32_t pixelCount = static_cast<uint32_t>(width) * height;
    {
        DisplayLockGuard displayLock;
        if (!displayLock.locked()) {
            GUI_LOG_ERROR("DMA partial transfer failed: display lock not acquired");
            handleTransferComplete(false, 0);
            return false;
        }

        Framebuffer::instance().markDmaStarted();
        M5.Display.setAddrWindow(x, y, width, height);
        M5.Display.pushPixelsDMA(buffer, pixelCount);
        M5.Display.waitDMA();
    }

    handleTransferComplete(true, pixelCount * sizeof(uint16_t));
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

void DmaTransfer::handleTransferComplete(bool success, uint32_t bytesTransferred) {
    uint32_t elapsedUs = esp_timer_get_time() - m_transferStartUs;

    // Update statistics (under lock for cross-core safety)
    portENTER_CRITICAL(&s_statsLock);
    if (success) {
        m_stats.transferCount++;
        m_stats.bytesTransferred += bytesTransferred > 0
            ? bytesTransferred : Config::FRAMEBUFFER_SIZE;
        m_stats.totalTransferTimeUs += elapsedUs;
        if (elapsedUs > m_stats.maxTransferTimeUs) {
            m_stats.maxTransferTimeUs = elapsedUs;
        }
    } else {
        m_stats.errorCount++;
    }
    portEXIT_CRITICAL(&s_statsLock);

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
    const uint32_t fullFrameBytes = Config::FRAMEBUFFER_SIZE;
    uint32_t transferBytes = std::min<uint32_t>(size, fullFrameBytes);
    if ((transferBytes & 0x1u) != 0) {
        transferBytes -= 1;
    }
    if (transferBytes == 0) {
        GUI_LOG_ERROR("DMA blocking full transfer skipped: empty payload");
        portENTER_CRITICAL(&s_statsLock);
        m_stats.errorCount++;
        portEXIT_CRITICAL(&s_statsLock);
        return;
    }
    const uint32_t pixelCount = transferBytes / sizeof(uint16_t);

    // Use M5GFX blocking transfer
    {
        DisplayLockGuard displayLock;
        if (!displayLock.locked()) {
            GUI_LOG_ERROR("DMA blocking full transfer failed: display lock not acquired");
            portENTER_CRITICAL(&s_statsLock);
            m_stats.errorCount++;
            portEXIT_CRITICAL(&s_statsLock);
            return;
        }
        M5.Display.setAddrWindow(0, 0, Config::DISPLAY_WIDTH, Config::DISPLAY_HEIGHT);
        M5.Display.pushPixels(buffer, pixelCount);
    }

    uint32_t elapsedUs = esp_timer_get_time() - startUs;

    // Update statistics
    portENTER_CRITICAL(&s_statsLock);
    m_stats.transferCount++;
    m_stats.bytesTransferred += transferBytes;
    m_stats.totalTransferTimeUs += elapsedUs;
    if (elapsedUs > m_stats.maxTransferTimeUs) {
        m_stats.maxTransferTimeUs = elapsedUs;
    }
    portEXIT_CRITICAL(&s_statsLock);
}

void DmaTransfer::blockingPartialTransfer(const uint16_t* buffer,
                                           int16_t x, int16_t y,
                                           uint16_t width, uint16_t height) {
    if (!buffer) return;

    uint32_t startUs = esp_timer_get_time();

    {
        DisplayLockGuard displayLock;
        if (!displayLock.locked()) {
            GUI_LOG_ERROR("DMA blocking partial transfer failed: display lock not acquired");
            portENTER_CRITICAL(&s_statsLock);
            m_stats.errorCount++;
            portEXIT_CRITICAL(&s_statsLock);
            return;
        }
        M5.Display.setAddrWindow(x, y, width, height);
        M5.Display.pushPixels(buffer, width * height);
    }

    uint32_t elapsedUs = esp_timer_get_time() - startUs;
    uint32_t size = width * height * 2;

    portENTER_CRITICAL(&s_statsLock);
    m_stats.transferCount++;
    m_stats.bytesTransferred += size;
    m_stats.totalTransferTimeUs += elapsedUs;
    if (elapsedUs > m_stats.maxTransferTimeUs) {
        m_stats.maxTransferTimeUs = elapsedUs;
    }
    portEXIT_CRITICAL(&s_statsLock);
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
    resetTransferStats();
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
        // DMA path with deterministic fallback.
        if (!dma.startFullTransfer(buffer, size)) {
            dma.waitComplete(50);
            if (!dma.startFullTransfer(buffer, size)) {
                m_dmaStartFailures.fetch_add(1, std::memory_order_relaxed);
                m_fallbackTransfers.fetch_add(1, std::memory_order_relaxed);
                GUI_LOG_ERROR("DisplayUpdater: full DMA start failed, fallback to blocking transfer");
                dma.blockingTransfer(buffer, size);
            }
        }
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
    if (!m_initialized || region.isEmpty()) {
        return;
    }

    Framebuffer& fb = Framebuffer::instance();
    DmaTransfer& dma = DmaTransfer::instance();

    // Clamp region to display bounds.
    const int16_t x = std::max<int16_t>(0, region.x);
    const int16_t y = std::max<int16_t>(0, region.y);

    // Bail out if origin is already beyond the screen to prevent underflow.
    if (x >= Config::DISPLAY_WIDTH || y >= Config::DISPLAY_HEIGHT) {
        return;
    }

    const uint16_t w = std::min<uint16_t>(region.width, Config::DISPLAY_WIDTH - x);
    const uint16_t h = std::min<uint16_t>(region.height, Config::DISPLAY_HEIGHT - y);
    if (w == 0 || h == 0) {
        return;
    }

    // If region covers most of the screen, a full transfer is cheaper.
    const uint32_t regionPixels = static_cast<uint32_t>(w) * h;
    const uint32_t totalPixels = Config::DISPLAY_WIDTH * Config::DISPLAY_HEIGHT;
    if (regionPixels > (totalPixels * 3u) / 4u) {
        pushFramebuffer();
        return;
    }

    // Extract sub-rectangle into contiguous DMA-capable memory.
    const uint32_t regionBytes = regionPixels * sizeof(uint16_t);
    uint16_t* tempBuffer = static_cast<uint16_t*>(
        heap_caps_malloc(regionBytes, MALLOC_CAP_DMA | MALLOC_CAP_32BIT)
    );
    if (!tempBuffer) {
        m_partialAllocFailures.fetch_add(1, std::memory_order_relaxed);
        m_fallbackTransfers.fetch_add(1, std::memory_order_relaxed);
        GUI_LOG_ERROR("DisplayUpdater: partial buffer alloc failed (%lu bytes), fallback full transfer",
                      static_cast<unsigned long>(regionBytes));
        pushFramebuffer();
        return;
    }

    const uint16_t* srcBuffer = m_useDoubleBuffer ? fb.getFrontBuffer() : fb.getBackBuffer();
    const uint16_t* srcRow = srcBuffer + y * Config::DISPLAY_WIDTH + x;
    for (uint16_t row = 0; row < h; ++row) {
        memcpy(tempBuffer + row * w, srcRow, w * sizeof(uint16_t));
        srcRow += Config::DISPLAY_WIDTH;
    }

    // Transfer contiguous region to display.
    if (m_useDma && dma.isAvailable()) {
        if (!dma.startPartialTransfer(tempBuffer, x, y, w, h)) {
            dma.waitComplete(50);
            if (!dma.startPartialTransfer(tempBuffer, x, y, w, h)) {
                m_dmaStartFailures.fetch_add(1, std::memory_order_relaxed);
                m_fallbackTransfers.fetch_add(1, std::memory_order_relaxed);
                GUI_LOG_ERROR("DisplayUpdater: partial DMA start failed (%d,%d %ux%u), fallback to blocking transfer",
                              x, y, static_cast<unsigned>(w), static_cast<unsigned>(h));
                dma.blockingPartialTransfer(tempBuffer, x, y, w, h);
            } else {
                // Keep tempBuffer alive until transfer completion.
                dma.waitComplete();
            }
        } else {
            // Keep tempBuffer alive until transfer completion.
            dma.waitComplete();
        }
    } else {
        dma.blockingPartialTransfer(tempBuffer, x, y, w, h);
    }

    heap_caps_free(tempBuffer);
}

} // namespace GUI
