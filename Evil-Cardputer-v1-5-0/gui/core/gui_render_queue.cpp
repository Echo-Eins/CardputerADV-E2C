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
    : m_head(0)
    , m_tail(0)
    , m_dataAvailable(nullptr)
    , m_syncComplete(nullptr)
    , m_syncPending(false)
    , m_overflowCount(0)
    , m_highWaterMark(0)
    , m_initialized(false)
{
    // Clear buffer
    memset(m_buffer, 0, sizeof(m_buffer));
}

RenderQueue::~RenderQueue() {
    shutdown();
}

// ============================================================================
// Initialization
// ============================================================================

bool RenderQueue::init() {
    if (m_initialized) {
        return true;  // Already initialized
    }

    // Create semaphore for blocking consumer
    // Binary semaphore, initially empty (0)
    m_dataAvailable = xSemaphoreCreateCounting(Config::QUEUE_SIZE, 0);
    if (!m_dataAvailable) {
        GUI_LOG_ERROR("Failed to create dataAvailable semaphore");
        return false;
    }

    // Create semaphore for sync operation
    m_syncComplete = xSemaphoreCreateBinary();
    if (!m_syncComplete) {
        vSemaphoreDelete(m_dataAvailable);
        m_dataAvailable = nullptr;
        GUI_LOG_ERROR("Failed to create syncComplete semaphore");
        return false;
    }

    // Reset indices
    m_head.store(0, std::memory_order_relaxed);
    m_tail.store(0, std::memory_order_relaxed);

    // Reset statistics
    m_overflowCount.store(0, std::memory_order_relaxed);
    m_highWaterMark.store(0, std::memory_order_relaxed);

    m_initialized = true;
    GUI_LOG("RenderQueue initialized (capacity: %d)", Config::QUEUE_SIZE);
    return true;
}

void RenderQueue::shutdown() {
    if (!m_initialized) {
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

    m_initialized = false;
    GUI_LOG("RenderQueue shutdown");
}

// ============================================================================
// Producer Interface (Lock-free push)
// ============================================================================

bool RenderQueue::push(const RenderOp& op) {
    if (!m_initialized) {
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

size_t RenderQueue::pushBatch(const RenderOp* ops, size_t count) {
    if (!m_initialized || ops == nullptr || count == 0) {
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
    if (!m_initialized) {
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

    // Check if this was a sync command
    if (op.type == RenderOpType::Sync && m_syncPending.load(std::memory_order_acquire)) {
        // Signal sync completion
        m_syncPending.store(false, std::memory_order_release);
        xSemaphoreGive(m_syncComplete);
    }

    return true;
}

bool RenderQueue::tryPop(RenderOp& op) {
    return pop(op, 0);  // Zero timeout = non-blocking
}

bool RenderQueue::peek(RenderOp& op) const {
    if (!m_initialized) {
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

// ============================================================================
// Synchronization
// ============================================================================

void RenderQueue::clear() {
    if (!m_initialized) {
        return;
    }

    // Reset indices (both producer and consumer see empty queue)
    m_tail.store(m_head.load(std::memory_order_acquire), std::memory_order_release);

    // Clear semaphore counts
    while (xSemaphoreTake(m_dataAvailable, 0) == pdTRUE) {
        // Drain semaphore
    }

    GUI_LOG("RenderQueue cleared");
}

void RenderQueue::sync(uint32_t timeoutMs) {
    if (!m_initialized) {
        return;
    }

    // If queue is already empty, nothing to sync
    if (isEmpty()) {
        return;
    }

    // Set sync pending flag
    m_syncPending.store(true, std::memory_order_release);

    // Push sync command
    RenderOp syncOp;
    syncOp.type = RenderOpType::Sync;
    syncOp.priority = RenderPriority::Urgent;

    if (!push(syncOp)) {
        m_syncPending.store(false, std::memory_order_release);
        GUI_LOG_ERROR("Failed to push sync command");
        return;
    }

    // Wait for sync completion
    if (xSemaphoreTake(m_syncComplete, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        m_syncPending.store(false, std::memory_order_release);
        GUI_LOG_ERROR("Sync timeout after %lu ms", timeoutMs);
    }
}

void RenderQueue::signalProcessed() {
    // This method can be called by renderer after processing each command
    // Currently used for sync operation detection in pop()
    // Reserved for future use (e.g., back-pressure signaling)
}

} // namespace GUI
