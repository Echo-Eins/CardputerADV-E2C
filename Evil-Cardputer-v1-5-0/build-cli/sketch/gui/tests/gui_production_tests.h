#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\tests\\gui_production_tests.h"
/*
 * GUI Production Tests - Unit + stress validation entrypoints
 *
 * These tests are intended for validation firmware builds and should be
 * disabled in normal release images.
 */

#ifndef GUI_PRODUCTION_TESTS_H
#define GUI_PRODUCTION_TESTS_H

#include <Arduino.h>
#include "../gui_config.h"

namespace GUI {

struct ProductionTestReport {
    uint32_t unitPassed;
    uint32_t unitFailed;
    uint32_t reconnectCycles;
    uint32_t stressBursts;
    uint32_t stressOpsPushed;
    uint32_t stressOpsDropped;

    uint32_t queueOverflows;
    uint32_t queueRetries;
    uint32_t queueDropped;
    uint32_t queueBlockTimeouts;

    size_t heapFreeBefore;
    size_t heapFreeAfter;
    size_t heapMinAfter;
    size_t heapLargestBefore;
    size_t heapLargestAfter;
    int32_t heapDelta;

    bool memoryLeakSuspected;
    bool fragmentationWarning;

    ProductionTestReport()
        : unitPassed(0)
        , unitFailed(0)
        , reconnectCycles(0)
        , stressBursts(0)
        , stressOpsPushed(0)
        , stressOpsDropped(0)
        , queueOverflows(0)
        , queueRetries(0)
        , queueDropped(0)
        , queueBlockTimeouts(0)
        , heapFreeBefore(0)
        , heapFreeAfter(0)
        , heapMinAfter(0)
        , heapLargestBefore(0)
        , heapLargestAfter(0)
        , heapDelta(0)
        , memoryLeakSuspected(false)
        , fragmentationWarning(false) {}
};

// Unit scope: ownership / pool / queue behavior.
bool runGuiUnitTests(ProductionTestReport* report = nullptr);

// Integration scope: long session with reconnects + burst producer traffic.
bool runGuiStressTests(uint32_t durationMs = Config::TEST_STRESS_DURATION_MS,
                       uint32_t reconnectCycles = Config::TEST_RECONNECT_CYCLES,
                       ProductionTestReport* report = nullptr);

// Full validation: unit + stress with final memory checks.
bool runGuiProductionValidation(ProductionTestReport* report = nullptr);

// Print report to serial/debug stream.
void printProductionTestReport(const ProductionTestReport& report, Stream& out = Serial);

} // namespace GUI

#endif // GUI_PRODUCTION_TESTS_H
