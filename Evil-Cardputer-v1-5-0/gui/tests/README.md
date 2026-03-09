# GUI Production Tests

## Purpose

Validation suite for production readiness of the async GUI/video driver stack:

- unit tests for queue ordering, ownership cleanup, backpressure accounting, framebuffer pool swap
- integration stress with long render bursts, repeated reconnect cycles, and synthetic input bursts
- memory checks (free heap delta + largest block fragmentation trend)

## Enable Test Build

Set compile-time flag in `gui/gui_config.h` (or via build flags):

```cpp
#define GUI_ENABLE_PRODUCTION_TESTS 1
```

Optional tuning:

```cpp
#define GUI_TEST_STRESS_DURATION_MS 180000
#define GUI_TEST_RECONNECT_CYCLES 12
#define GUI_TEST_BURST_OPS 256
```

## Run

Call from your validation firmware path (after `M5.begin()`):

```cpp
GUI::ProductionTestReport report;
bool ok = GUI::runGuiProductionValidation(&report);
GUI::printProductionTestReport(report, Serial);
```

If you need split runs:

```cpp
GUI::runGuiUnitTests(&report);
GUI::runGuiStressTests(120000, 8, &report);
```

## Pass/Fail Expectations

- `unitFailed == 0`
- no unbounded growth of `queueDropped` under normal profile
- `memoryLeakSuspected == false`
- `fragmentationWarning == false` (or investigated with reproducible evidence)

## Notes

- The suite is disabled by default in release builds (`GUI_ENABLE_PRODUCTION_TESTS=0`).
- Stress run requires initialized display stack (`M5.begin()` before test invocation).

