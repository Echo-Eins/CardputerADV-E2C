/*
 * GUI Render Queue Implementation - Lock-free SPSC ring buffer
 *
 * Thread safety model:
 * - Single producer (main loop on Core 1)
 * - Single consumer (renderer task on Core 0)
 * - Lock-free push/pop using atomic operations
 * - Memory barriers via std::atomic with appropriate memory orders
 *
 * Memory ordering:
 * - Producer: release semantics when updating head
 * - Consumer: acquire semantics when reading head
 * - This ensures all data written before head update is visible after acquire
 */

#include "gui_render_queue.h"
#include <Arduino.h>
#include <new>

namespace GUI {

// ============================================================================
// Singleton Instance
// ============================================================================

RenderQueue& RenderQueue::instance() {
    static RenderQueue instance;
    return instance;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

RenderQueue::RenderQueue()
    : m_buffer(nullptr)
    , m_head(0)
    , m_tail(0)
    , m_dataAvailable(nullptr)
    , m_syncComplete(nullptr)
    , m_syncPending(false)
    , m_overflowCount(0)
    , m_highWaterMark(0)
    // m_initialized is already initialized to false via in-class initializer
{}

RenderQueue::~RenderQueue() {
    shutdown();
}

// ============================================================================
// Initialization
// ============================================================================

bool RenderQueue::init() {
    if (m_initialized.load(std::memory_order_acquire)) {
        return true;  // Already initialized
    }

    m_buffer = new (std::nothrow) RenderOp[Config::QUEUE_SIZE];
    if (!m_buffer) {
        GUI_LOG_ERROR("Failed to allocate RenderQueue storage (%u bytes)",
                      static_cast<unsigned>(sizeof(RenderOp) *
                                            Config::QUEUE_SIZE));
        return false;
    }

    // Create semaphore for blocking consumer
    // Binary semaphore, initially empty (0)
    m_dataAvailable = xSemaphoreCreateCounting(Config::QUEUE_SIZE, 0);
    if (!m_dataAvailable) {
        delete[] m_buffer;
        m_buffer = nullptr;
        GUI_LOG_ERROR("Failed to create dataAvailable semaphore");
        return false;
    }

    // Create semaphore for sync operation
    m_syncComplete = xSemaphoreCreateBinary();
    if (!m_syncComplete) {
        vSemaphoreDelete(m_dataAvailable);
        m_dataAvailable = nullptr;
        delete[] m_buffer;
        m_buffer = nullptr;
        GUI_LOG_ERROR("Failed to create syncComplete semaphore");
        return false;
    }

    // Reset indices
    m_head.store(0, std::memory_order_relaxed);
    m_tail.store(0, std::memory_order_relaxed);

    // Reset statistics
    m_overflowCount.store(0, std::memory_order_relaxed);
    m_highWaterMark.store(0, std::memory_order_relaxed);
    resetBackpressureStats();

    m_initialized.store(true, std::memory_order_release);
    GUI_LOG("RenderQueue initialized (capacity: %d)", Config::QUEUE_SIZE);
    return true;
}

void RenderQueue::shutdown() {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return;
    }

    // Clear queue
    clear();

    // Delete semaphores
    if (m_dataAvailable) {
        vSemaphoreDelete(m_dataAvailable);
        m_dataAvailable = nullptr;
    }

    if (m_syncComplete) {
        vSemaphoreDelete(m_syncComplete);
        m_syncComplete = nullptr;
    }

    delete[] m_buffer;
    m_buffer = nullptr;

    m_initialized.store(false, std::memory_order_release);
    GUI_LOG("RenderQueue shutdown");
}

// ============================================================================
// Producer Interface (Lock-free push)
// ============================================================================

bool RenderQueue::push(const RenderOp& op) {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return false;
    }

    // Load current head and tail
    // Use relaxed order for head (we own it)
    // Use acquire order for tail (to see consumer's writes)
    const size_t currentHead = m_head.load(std::memory_order_relaxed);
    const size_t currentTail = m_tail.load(std::memory_order_acquire);

    // Calculate next head position
    const size_t nextHead = (currentHead + 1) & Config::QUEUE_MASK;

    // Check if queue is full
    if (nextHead == currentTail) {
        // Queue is full, increment overflow counter
        m_overflowCount.fetch_add(1, std::memory_order_relaxed);
        GUI_LOG_ERROR("RenderQueue overflow (pending: %d)", pending());
        return false;
    }

    // Copy command to buffer
    m_buffer[currentHead] = op;

    // Update head with release semantics
    // This ensures the command write is visible to consumer
    m_head.store(nextHead, std::memory_order_release);

    // Update high water mark
    size_t currentPending = pending();
    size_t currentHWM = m_highWaterMark.load(std::memory_order_relaxed);
    while (currentPending > currentHWM) {
        if (m_highWaterMark.compare_exchange_weak(currentHWM, currentPending,
                                                   std::memory_order_relaxed)) {
            break;
        }
    }

    // Signal consumer that data is available
    xSemaphoreGive(m_dataAvailable);

    return true;
}

bool RenderQueue::pushWithBackpressure(const RenderOp& op, uint32_t maxWaitMs) {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return false;
    }

    if (push(op)) {
        return true;
    }

    // Deterministic retry loop with fixed backoff.
    const uint32_t retryDelayMs = (Config::QUEUE_PUSH_RETRY_DELAY_MS == 0)
        ? 1
        : Config::QUEUE_PUSH_RETRY_DELAY_MS;
    const TickType_t retryDelayTicks = pdMS_TO_TICKS(retryDelayMs);

    const bool blockProducer = (Config::QUEUE_OVERFLOW_POLICY == Config::QUEUE_OVERFLOW_BLOCK_PRODUCER);
    bool blockTimeout = false;
    uint32_t elapsedMs = 0;
    uint32_t retryAttempts = 0;
    while (elapsedMs < maxWaitMs || blockProducer) {
        m_retryCount.fetch_add(1, std::memory_order_relaxed);
        ++retryAttempts;

        // If blocking policy is enabled, keep waiting until success.
        // Otherwise stop at maxWaitMs and apply drop-newest.
        if (!blockProducer && elapsedMs >= maxWaitMs) {
            break;
        }

        vTaskDelay(retryDelayTicks);
        if (push(op)) {
            return true;
        }
        elapsedMs += retryDelayMs;

        // Guard against extremely long waits in block mode if maxWaitMs is 0.
        if (blockProducer && maxWaitMs > 0 && elapsedMs >= maxWaitMs) {
            m_blockTimeoutCount.fetch_add(1, std::memory_order_relaxed);
            blockTimeout = true;
            break;
        }
    }

    m_droppedCount.fetch_add(1, std::memory_order_relaxed);
    GUI_LOG_ERROR(
        "RenderQueue dropped op=%u after %lu ms (policy=%u, retries=%lu, reason=%s)",
        static_cast<unsigned>(op.type),
        static_cast<unsigned long>(elapsedMs),
        static_cast<unsigned>(Config::QUEUE_OVERFLOW_POLICY),
        static_cast<unsigned long>(retryAttempts),
        blockTimeout ? "block-timeout" : "drop-newest");
    return false;
}

size_t RenderQueue::pushBatch(const RenderOp* ops, size_t count) {
    if (!m_initialized.load(std::memory_order_acquire) || ops == nullptr || count == 0) {
        return 0;
    }

    size_t pushed = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!push(ops[i])) {
            break;  // Queue full, stop pushing
        }
        ++pushed;
    }
    return pushed;
}

// ============================================================================
// Consumer Interface (Lock-free pop)
// ============================================================================

bool RenderQueue::pop(RenderOp& op, uint32_t timeoutMs) {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return false;
    }

    // Wait for data to be available
    if (xSemaphoreTake(m_dataAvailable, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        return false;  // Timeout
    }

    // Load current tail and head
    // Use relaxed order for tail (we own it)
    // Use acquire order for head (to see producer's writes)
    const size_t currentTail = m_tail.load(std::memory_order_relaxed);
    const size_t currentHead = m_head.load(std::memory_order_acquire);

    // Double-check queue is not empty (should never happen after semaphore)
    if (currentTail == currentHead) {
        GUI_LOG_ERROR("Queue empty after semaphore (spurious wake?)");
        return false;
    }

    // Read command from buffer
    op = m_buffer[currentTail];

    // Calculate next tail position
    const size_t nextTail = (currentTail + 1) & Config::QUEUE_MASK;

    // Update tail with release semantics
    m_tail.store(nextTail, std::memory_order_release);

    return true;
}

bool RenderQueue::tryPop(RenderOp& op) {
    return pop(op, 0);  // Zero timeout = non-blocking
}

bool RenderQueue::peek(RenderOp& op) const {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return false;
    }

    const size_t currentTail = m_tail.load(std::memory_order_acquire);
    const size_t currentHead = m_head.load(std::memory_order_acquire);

    if (currentTail == currentHead) {
        return false;  // Empty
    }

    op = m_buffer[currentTail];
    return true;
}

// ============================================================================
// Queue Status
// ============================================================================

size_t RenderQueue::pending() const {
    const size_t head = m_head.load(std::memory_order_acquire);
    const size_t tail = m_tail.load(std::memory_order_acquire);

    // Handle wrap-around
    if (head >= tail) {
        return head - tail;
    } else {
        return Config::QUEUE_SIZE - tail + head;
    }
}

bool RenderQueue::isFull() const {
    const size_t head = m_head.load(std::memory_order_acquire);
    const size_t tail = m_tail.load(std::memory_order_acquire);
    const size_t nextHead = (head + 1) & Config::QUEUE_MASK;
    return nextHead == tail;
}

bool RenderQueue::isEmpty() const {
    const size_t head = m_head.load(std::memory_order_acquire);
    const size_t tail = m_tail.load(std::memory_order_acquire);
    return head == tail;
}

bool RenderQueue::hasPendingFrameBoundary() const {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return false;
    }

    size_t index = m_tail.load(std::memory_order_acquire);
    const size_t head = m_head.load(std::memory_order_acquire);
    while (index != head) {
        const RenderOpType type = m_buffer[index].type;
        if (type == RenderOpType::Sync) {
            // A Sync command is an explicit ordering barrier. Never coalesce
            // a frame across it.
            return false;
        }
        if (type == RenderOpType::EndFrame) {
            return true;
        }
        index = (index + 1) & Config::QUEUE_MASK;
    }
    return false;
}

// ============================================================================
// Synchronization
// ============================================================================

void RenderQueue::clear() {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return;
    }

    // Snapshot the current pending window and release owned payloads before
    // dropping commands. This prevents leaks when image commands are cleared.
    const size_t head = m_head.load(std::memory_order_acquire);
    const size_t tail = m_tail.load(std::memory_order_relaxed);
    size_t idx = tail;
    while (idx != head) {
        releaseOwnedPayload(m_buffer[idx]);
        idx = (idx + 1) & Config::QUEUE_MASK;
    }

    // Advance tail to match head, then drain the matching semaphore tokens.
    // This avoids losing tokens from concurrent pushes after the snapshot.
    m_tail.store(head, std::memory_order_release);

    // Drain only the tokens that corresponded to the cleared items
    size_t toDrain = (head >= tail)
        ? (head - tail)
        : (Config::QUEUE_SIZE - tail + head);
    for (size_t i = 0; i < toDrain; ++i) {
        xSemaphoreTake(m_dataAvailable, 0);
    }

    GUI_LOG("RenderQueue cleared");
}

bool RenderQueue::sync(uint32_t timeoutMs) {
    if (!m_initialized.load(std::memory_order_acquire)) {
        return false;
    }

    // Drain stale completion token from a timed-out previous sync.
    while (xSemaphoreTake(m_syncComplete, 0) == pdTRUE) {
    }

    // Set sync pending flag BEFORE pushing to avoid TOCTOU race.
    // The consumer checks this flag when it pops a Sync op.
    m_syncPending.store(true, std::memory_order_release);

    // Push sync command — this acts as a barrier in the queue.
    // Even if the queue was empty before, the consumer will process
    // this op and signal completion.
    RenderOp syncOp;
    syncOp.type = RenderOpType::Sync;
    syncOp.priority = RenderPriority::Urgent;

    const bool infiniteWait = (timeoutMs == portMAX_DELAY);
    const uint32_t startMs = millis();
    while (!push(syncOp)) {
        if (!infiniteWait) {
            const uint32_t elapsed = millis() - startMs;
            if (elapsed >= timeoutMs) {
                m_syncPending.store(false, std::memory_order_release);
                GUI_LOG_ERROR("Failed to enqueue sync command within %lu ms", timeoutMs);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Wait for sync completion
    TickType_t waitTicks = portMAX_DELAY;
    if (!infiniteWait) {
        const uint32_t elapsed = millis() - startMs;
        const uint32_t remaining = (elapsed >= timeoutMs) ? 0 : (timeoutMs - elapsed);
        waitTicks = pdMS_TO_TICKS(remaining);
    }

    if (xSemaphoreTake(m_syncComplete, waitTicks) != pdTRUE) {
        m_syncPending.store(false, std::memory_order_release);
        GUI_LOG_ERROR("Sync timeout after %lu ms", timeoutMs);
        return false;
    }
    return true;
}

void RenderQueue::signalProcessed() {
    // A Sync barrier completes only after the renderer has executed the Sync
    // command and flushed/waited for the display, never when it is merely
    // removed from the queue.
    if (m_syncPending.exchange(false, std::memory_order_acq_rel)) {
        xSemaphoreGive(m_syncComplete);
    }
}

} // namespace GUI
