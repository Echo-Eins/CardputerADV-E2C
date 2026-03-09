# GUI Release Notes - 2026-03-08

## Summary

Production-hardening update for GUI/video driver integration and queue behavior.

## Fixed

### DisplayAdapter

- `queryDisplayInfo()` now reads actual `M5.Display` width/height/rotation.
- `setRotation()` now applies rotation to hardware, then refreshes effective dimensions.
- `setScaleFactor()` rejects invalid values (`<=0`, `NaN`, `Inf`).
- `calculateScaleFactor()` guards invalid target DPI.

### Build/Include Hygiene

- Fixed wrong include path in `gui_widget_manager.cpp`.
- Added missing standard includes in widget modules (`<algorithm>`, `<functional>`, `<cstring>`, `<cstdlib>`).
- `gui_draw.h` now includes `gui_renderer.h` for stable `Draw::*` and queue API visibility.

### Queue Backpressure

- Added deterministic policy configuration in `gui_config.h`:
  - overflow policy
  - bounded wait
  - retry delay
- Legacy bridge queue path migrated to unified `pushWithBackpressure()`.
- Added explicit drop/error logging with policy and counters.
- Added backpressure counters to `GUI::printStatus()`.

### Validation/Testing

- Added production test suite:
  - `gui/tests/gui_production_tests.{h,cpp}`
  - `gui/tests/README.md`
- Coverage focus:
  - queue ordering and saturation accounting
  - ownership cleanup path on queue clear
  - framebuffer pool swap sanity
  - reconnect + burst stress and heap trend checks

### Documentation

- `docs/GUI_PRODUCTION_MIGRATION.md`
- `docs/GUI_KNOWN_LIMITS.md`
- this release note

## Rollback/Fallback

Emergency toggles available in `gui/gui_config.h`:

- `GUI_ROLLBACK_FORCE_PASSTHROUGH`
- `GUI_ROLLBACK_DISABLE_PARTIAL_UPDATE`
- `GUI_QUEUE_OVERFLOW_POLICY`
- `GUI_QUEUE_PUSH_MAX_WAIT_MS`
- `GUI_QUEUE_PUSH_RETRY_DELAY_MS`

## Compatibility

- No API breaks for existing `GUI::Draw` or widget call sites.
- Behavior under queue overload is now explicit and measurable.

