# GUI Known Limits

## DisplayAdapter

- Real display info is read from `M5.Display` only after panel initialization.
- If hardware lock is unavailable, rotation falls back to deterministic software state.

## Queue/Backpressure

- Queue is fixed-size SPSC ring buffer (`Config::QUEUE_SIZE`).
- Default overflow policy is `drop newest` after bounded retries.
- In block policy, `maxWaitMs=0` means unbounded wait by design.

## Text and Payload Limits

- `RenderOps::drawText` embeds short text in command payload (11 visible chars + null).
- Larger text should be chunked (already handled in legacy bridge path).
- Owned image payload must be released by consumer or by queue clear; do not reuse owned pointers after enqueue.

## Stress Validation

- Production stress suite is disabled in release builds by default.
- Enabling test suite increases runtime overhead and should be done only for validation firmware.

## Metrics Interpretation

- Non-zero `queueDropped` during synthetic overload may be expected in drop policy.
- A release candidate is rejected only if drops appear under expected production load profile or if memory leak/fragmentation flags trigger consistently.

