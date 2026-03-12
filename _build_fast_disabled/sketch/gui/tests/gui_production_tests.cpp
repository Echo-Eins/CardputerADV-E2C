#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\tests\\gui_production_tests.cpp"
/*
 * GUI Production Tests - Unit and integration stress validation
 */

#include "gui_production_tests.h"

#if GUI_ENABLE_PRODUCTION_TESTS

#include "../core/gui_render_queue.h"
#include "../core/gui_renderer.h"
#include "../core/gui_framebuffer.h"
#include "../legacy/gui_legacy_bridge.h"
#include "../widgets/gui_display_adapter.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <cstring>
#include <cstdlib>

namespace GUI {
namespace {

inline size_t heapFree() {
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

inline size_t heapLargest() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

inline size_t heapMin() {
    return heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

static bool testQueueFifo() {
    RenderQueue& q = renderQueue();
    if (!q.init()) {
        return false;
    }

    q.clear();
    q.resetOverflowCount();
    q.resetBackpressureStats();

    RenderOp a = RenderOps::drawPixel(11, 1, Colors::White);
    RenderOp b = RenderOps::drawPixel(22, 1, Colors::White);
    RenderOp c = RenderOps::drawPixel(33, 1, Colors::White);

    if (!q.push(a) || !q.push(b) || !q.push(c)) {
        q.clear();
        return false;
    }

    RenderOp out;
    if (!q.pop(out, 10) || out.type != RenderOpType::DrawPixel || out.data.pixel.pos.x != 11) {
        q.clear();
        return false;
    }
    if (!q.pop(out, 10) || out.type != RenderOpType::DrawPixel || out.data.pixel.pos.x != 22) {
        q.clear();
        return false;
    }
    if (!q.pop(out, 10) || out.type != RenderOpType::DrawPixel || out.data.pixel.pos.x != 33) {
        q.clear();
        return false;
    }

    q.clear();
    return true;
}

static bool testBackpressureAccounting() {
    RenderQueue& q = renderQueue();
    if (!q.init()) {
        return false;
    }

    q.clear();
    q.resetOverflowCount();
    q.resetBackpressureStats();

    RenderOp fill = RenderOps::drawPixel(1, 1, Colors::White);
    size_t pushed = 0;
    while (q.push(fill)) {
        ++pushed;
    }
    if (pushed == 0) {
        q.clear();
        return false;
    }

    const bool pushResult = q.pushWithBackpressure(fill, 2);
    const RenderQueue::BackpressureStats stats = q.getBackpressureStats();
    const bool ok = (!pushResult) && (stats.droppedCommands > 0);

    q.clear();
    return ok;
}

static bool testOwnedPayloadRelease() {
    RenderQueue& q = renderQueue();
    if (!q.init()) {
        return false;
    }

    q.clear();

    const size_t before = heapFree();
    const size_t bytes = 2048;
    uint16_t* owned = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    if (!owned) {
        owned = static_cast<uint16_t*>(std::malloc(bytes));
    }
    if (!owned) {
        return false;
    }
    std::memset(owned, 0xAB, bytes);

    RenderOp op;
    std::memset(&op, 0, sizeof(op));
    op.type = RenderOpType::DrawBitmap;
    op.target = DisplayTarget::Internal;
    op.data.bitmap.rect = Rect::make(0, 0, 32, 32);
    op.data.bitmap.data = owned;
    op.data.bitmap.ownsData = 1;

    if (!q.push(op)) {
        std::free(owned);
        q.clear();
        return false;
    }

    q.clear();
    vTaskDelay(pdMS_TO_TICKS(2));

    const size_t after = heapFree();
    // Allow allocator noise while still catching real leaks.
    return (after + 512) >= before;
}

#if GUI_DOUBLE_BUFFER
static bool testFramebufferPool() {
    Framebuffer& fb = Framebuffer::instance();
    const bool wasInitialized = fb.isInitialized();
    if (!wasInitialized && !fb.init()) {
        return false;
    }

    uint16_t* frontBefore = fb.getFrontBuffer();
    uint16_t* backBefore = fb.getBackBuffer();
    if (!frontBefore || !backBefore) {
        if (!wasInitialized) {
            fb.shutdown();
        }
        return false;
    }

    const bool doubleBuffered = fb.getConfig().useDoubleBuffer;
    bool ok = true;
    if (doubleBuffered) {
        if (frontBefore == backBefore) {
            ok = false;
        } else {
            fb.swap();
            ok = (fb.getFrontBuffer() == backBefore) && (fb.getBackBuffer() == frontBefore);
        }
    }

    if (!wasInitialized) {
        fb.shutdown();
    }
    return ok;
}
#endif

static void updateQueueStats(ProductionTestReport& report) {
    RenderQueue& q = renderQueue();
    const RenderQueue::BackpressureStats bp = q.getBackpressureStats();
    report.queueOverflows += q.getOverflowCount();
    report.queueRetries += bp.retries;
    report.queueDropped += bp.droppedCommands;
    report.queueBlockTimeouts += bp.blockTimeouts;
}

static void updateMemoryStats(ProductionTestReport& report) {
    report.heapFreeAfter = heapFree();
    report.heapLargestAfter = heapLargest();
    report.heapMinAfter = heapMin();
    report.heapDelta = static_cast<int32_t>(report.heapFreeAfter) -
                       static_cast<int32_t>(report.heapFreeBefore);
    report.memoryLeakSuspected =
        (report.heapFreeAfter + Config::TEST_HEAP_LEAK_THRESHOLD_BYTES) < report.heapFreeBefore;
    report.fragmentationWarning =
        (report.heapLargestAfter + Config::TEST_HEAP_FRAGMENT_THRESHOLD_BYTES) < report.heapLargestBefore;
}

} // namespace

bool runGuiUnitTests(ProductionTestReport* report) {
    if (guiIsRunning()) {
        guiStop();
    }

    ProductionTestReport local;
    ProductionTestReport& out = report ? *report : local;

    const bool fifoOk = testQueueFifo();
    out.unitPassed += fifoOk ? 1 : 0;
    out.unitFailed += fifoOk ? 0 : 1;

    const bool pressureOk = testBackpressureAccounting();
    out.unitPassed += pressureOk ? 1 : 0;
    out.unitFailed += pressureOk ? 0 : 1;

    const bool ownershipOk = testOwnedPayloadRelease();
    out.unitPassed += ownershipOk ? 1 : 0;
    out.unitFailed += ownershipOk ? 0 : 1;

#if GUI_DOUBLE_BUFFER
    const bool poolOk = testFramebufferPool();
    out.unitPassed += poolOk ? 1 : 0;
    out.unitFailed += poolOk ? 0 : 1;
#endif

    return out.unitFailed == 0;
}

bool runGuiStressTests(uint32_t durationMs, uint32_t reconnectCycles, ProductionTestReport* report) {
    if (durationMs < 1000) {
        durationMs = 1000;
    }
    if (reconnectCycles == 0) {
        reconnectCycles = 1;
    }

    ProductionTestReport local;
    ProductionTestReport& out = report ? *report : local;

    out.heapFreeBefore = heapFree();
    out.heapLargestBefore = heapLargest();

    const uint32_t perCycleMs = durationMs / reconnectCycles;
    const uint32_t cycleBudgetMs = perCycleMs > 0 ? perCycleMs : 1;

    for (uint32_t cycle = 0; cycle < reconnectCycles; ++cycle) {
        if (!guiInit()) {
            return false;
        }
        if (!guiStart()) {
            guiShutdown();
            return false;
        }

        LegacyBridge::init();
        DisplayAdapter::instance().queryDisplayInfo();

        const uint32_t cycleStart = millis();
        while ((millis() - cycleStart) < cycleBudgetMs) {
            for (uint32_t i = 0; i < Config::TEST_BURST_OPS; ++i) {
                const int16_t x = static_cast<int16_t>((i * 7 + cycle) % Config::DISPLAY_WIDTH);
                const int16_t y = static_cast<int16_t>((i * 11 + cycle) % Config::DISPLAY_HEIGHT);
                const bool ok = renderQueue().pushWithBackpressure(RenderOps::drawPixel(x, y, Colors::White));
                if (ok) {
                    ++out.stressOpsPushed;
                } else {
                    ++out.stressOpsDropped;
                }
            }

            renderQueue().pushWithBackpressure(RenderOps::endFrame());
            LegacyBridge::setCursor(0, 0);
            LegacyBridge::print("stress");
            LegacyBridge::drawPixel(
                static_cast<int16_t>(esp_random() % Config::DISPLAY_WIDTH),
                static_cast<int16_t>(esp_random() % Config::DISPLAY_HEIGHT),
                Colors::White);
            ++out.stressBursts;

            vTaskDelay(pdMS_TO_TICKS(1));
        }

        renderQueue().sync(2000);
        updateQueueStats(out);

        guiStop();
        guiShutdown();
        ++out.reconnectCycles;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    updateMemoryStats(out);
    return !out.memoryLeakSuspected;
}

bool runGuiProductionValidation(ProductionTestReport* report) {
    ProductionTestReport local;
    ProductionTestReport& out = report ? *report : local;

    const bool unitsOk = runGuiUnitTests(&out);
    const bool stressOk = runGuiStressTests(Config::TEST_STRESS_DURATION_MS,
                                            Config::TEST_RECONNECT_CYCLES,
                                            &out);
    updateMemoryStats(out);

    return unitsOk && stressOk && !out.memoryLeakSuspected;
}

void printProductionTestReport(const ProductionTestReport& report, Stream& out) {
    out.printf("[GUI TEST] unit passed=%lu failed=%lu\n",
               static_cast<unsigned long>(report.unitPassed),
               static_cast<unsigned long>(report.unitFailed));
    out.printf("[GUI TEST] reconnect cycles=%lu bursts=%lu pushed=%lu dropped=%lu\n",
               static_cast<unsigned long>(report.reconnectCycles),
               static_cast<unsigned long>(report.stressBursts),
               static_cast<unsigned long>(report.stressOpsPushed),
               static_cast<unsigned long>(report.stressOpsDropped));
    out.printf("[GUI TEST] queue overflow=%lu retries=%lu drops=%lu blockTimeouts=%lu\n",
               static_cast<unsigned long>(report.queueOverflows),
               static_cast<unsigned long>(report.queueRetries),
               static_cast<unsigned long>(report.queueDropped),
               static_cast<unsigned long>(report.queueBlockTimeouts));
    out.printf("[GUI TEST] heap before=%lu after=%lu delta=%ld minAfter=%lu\n",
               static_cast<unsigned long>(report.heapFreeBefore),
               static_cast<unsigned long>(report.heapFreeAfter),
               static_cast<long>(report.heapDelta),
               static_cast<unsigned long>(report.heapMinAfter));
    out.printf("[GUI TEST] largest block before=%lu after=%lu leak=%s fragmentation=%s\n",
               static_cast<unsigned long>(report.heapLargestBefore),
               static_cast<unsigned long>(report.heapLargestAfter),
               report.memoryLeakSuspected ? "yes" : "no",
               report.fragmentationWarning ? "yes" : "no");
}

} // namespace GUI

#else

namespace GUI {

bool runGuiUnitTests(ProductionTestReport* report) {
    (void)report;
    return false;
}

bool runGuiStressTests(uint32_t durationMs, uint32_t reconnectCycles, ProductionTestReport* report) {
    (void)durationMs;
    (void)reconnectCycles;
    (void)report;
    return false;
}

bool runGuiProductionValidation(ProductionTestReport* report) {
    (void)report;
    return false;
}

void printProductionTestReport(const ProductionTestReport& report, Stream& out) {
    (void)report;
    out.println("[GUI TEST] disabled (set GUI_ENABLE_PRODUCTION_TESTS=1)");
}

} // namespace GUI

#endif

