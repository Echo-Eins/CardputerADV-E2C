#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\docs\\GUI_PRODUCTION_MIGRATION.md"
# GUI Production Migration Guide

## Scope

This migration closes production gaps for the new GUI/video driver layer:

1. `DisplayAdapter` now reads real panel parameters and applies hardware rotation.
2. include/build hygiene in widget modules and draw wrappers.
3. deterministic queue backpressure policy with retry/drop metrics.
4. production validation suite (unit + stress + memory checks).
5. rollback/fallback controls and release checklist.

## Preconditions

- `M5.begin()` must be called before GUI start.
- Build target: Cardputer (`m5stack_cardputer`) with current GUI sources.
- For validation firmware, enable `GUI_ENABLE_PRODUCTION_TESTS=1`.

## Migration Steps

1. Build with default production settings.
2. Boot and run smoke checks:
   - `DisplayAdapter::queryDisplayInfo()` returns real `width/height/rotation`.
   - `DisplayAdapter::setRotation(n)` changes hardware rotation and dimensions.
   - invalid scale factors (`0`, `NaN`, `Inf`) are rejected.
3. Run unit tests:
   - `runGuiUnitTests()`
4. Run stress tests:
   - `runGuiStressTests(durationMs, reconnectCycles)`
5. Capture and review metrics:
   - queue overflows/retries/drops/block timeouts
   - heap delta and largest block trend
6. Promote to release profile when all acceptance gates pass.

## Acceptance Gates

- No compile errors on target profile.
- No new warning-critical diagnostics.
- Deterministic queue behavior under load:
  - explicit drop policy or block-timeout behavior
  - visible counters for retries/drops/timeouts
- Stress run completes without leak suspicion.

## Rollback and Safe Fallback

Use compile-time controls in `gui/gui_config.h`:

```cpp
// Force full direct passthrough mode for legacy bridge.
#define GUI_ROLLBACK_FORCE_PASSTHROUGH 1

// Disable partial updates and use full framebuffer updates.
#define GUI_ROLLBACK_DISABLE_PARTIAL_UPDATE 1

// Queue backpressure behavior:
// 0 = drop newest after bounded retry window
// 1 = block producer up to max wait (or indefinitely if max wait is 0)
#define GUI_QUEUE_OVERFLOW_POLICY 0
#define GUI_QUEUE_PUSH_MAX_WAIT_MS 6
#define GUI_QUEUE_PUSH_RETRY_DELAY_MS 1
```

Recommended safe fallback profile for incident mitigation:

- `GUI_ROLLBACK_FORCE_PASSTHROUGH=1`
- `GUI_ROLLBACK_DISABLE_PARTIAL_UPDATE=1`
- `GUI_QUEUE_OVERFLOW_POLICY=0`
- `GUI_QUEUE_PUSH_MAX_WAIT_MS=2`

## Operational Monitoring

Use `GUI::printStatus()` during validation and soak runs. It now reports:

- queue fill/high-water/overflow
- backpressure retries/drops/block timeouts
- renderer and dirty-tracking stats

