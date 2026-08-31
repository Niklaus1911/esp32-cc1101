# Fix Generic GPIO Reboot and RF Switching Defects

## Summary

Fix the confirmed review findings while retaining live RF backend switching. Generic GPIO changes remain NVS-backed and reboot-applied. Preserve all unrelated uncommitted work and plan files.

## Implementation Changes

- In RF cleanup, destroy RMT resources first, deinitialize the CC1101 while its SPI GPIOs are still configured, then release shared pins and establish the Generic TX-low/RX-pulldown state. If CC1101 deinitialization fails, preserve GPIO ownership, leave the service stopped/stopping, and propagate the error so existing switch rollback can restore the previous backend.
- Replace `request_system_reboot()` with `reserve_system_reboot()`, `cancel_system_reboot()`, and an infallible-after-reservation `commit_system_reboot()`. Reservation must atomically reject OTA uploads, pending OTA reboots, unavailable reboot infrastructure, and competing external reboot requests.
- In `POST /api/radio/generic-gpio`, reserve reboot ownership before validation/persistence. Cancel on validation failure, persistence failure, an already-applied mapping, or a persisted mapping that does not require reboot; commit only after a genuinely pending mapping is stored. A reservation failure must leave NVS untouched and must not return the previous misleading `saved:true` response.
- Send the successful `202 Accepted` response before committing the reboot notification so a slow client can enter browser reconnect handling; still commit the already-reserved reboot immediately after the response attempt.
- Exclude classic ESP32 input-only GPIO34-39 from Generic RX validation and option enumeration because they do not provide the internal pulldown used by the receiver path.
- Keep board remapping documentation consistent with runtime Generic GPIO selection and LED reservations.
- Replace the fixed three-second browser timer with a dedicated reboot attempt containing the expected TX/RX pair and a 60-second deadline. Poll `/api/live` independently of normal SSE/polling until `board.generic.tx/rx` match and `pending` is false.
- On confirmed recovery, clear reboot state, update live data, run the global control re-enable/render path, reopen SSE, resume polling, and replace the persistent reboot notice with a six-second success notice. On timeout or persistent mismatch, also restore controls and polling but show a persistent, actionable error.
- Delete `codex-session-01a054ba-0b9d-7330-8b5a-4f3e74e4eeae.md`. Preserve every unrelated uncommitted change and every file under `plan/`.

## Interface Changes

- `ota_update.hpp` exposes the reserve/cancel/commit reboot transaction instead of the one-step reboot request.
- Existing `/api/radio/generic-gpio` request fields and successful JSON response remain compatible.
- `/api/live` requires no schema change; browser confirmation uses the existing active GPIO and `pending` fields.
- RF backend selection remains a live operation and must not initiate a reboot.

## Test and Validation

- Extend host source-contract tests to enforce CC1101 deinitialization before shared-pin reset, reservation before persistence, cancellation on every non-commit path, and browser recovery through the global control-enabling path.
- Run all clean native host tests, both ESP32 and ESP32-S3 Unity image builds, `git diff --check`, and a final review of the complete uncommitted diff.
- Use Playwright MCP with a temporary localhost mock that simulates acceptance, temporary disconnection, stale/pending responses, exact recovery, mismatch, and timeout. Check mutation headers, resumed SSE/polling, control states, notices, console/network errors, horizontal overflow, and visually inspect desktop and mobile screenshots.
- Because firmware and embedded Web assets change, run `tools/verify-production.sh` once on the final unchanged candidate.
- If the approved device path is present, flash only with `tools/build-board.sh esp32s3-devkitc-n16r8 flash --port /dev/serial/by-id/usb-EXAMPLE_N16R8-if00`; do not erase NVS or open a serial monitor.
- On hardware, capture the original GPIO mapping, apply a distinct advertised non-CC1101-overlapping pair, verify reboot/reconnection and exact active mapping, then restore and reconfirm the original mapping.
- Validate `generic -> cc1101 -> generic` through the Web UI, checking successful active-backend reporting, RF service health, uninterrupted connectivity, and monotonically increasing uptime to prove backend switches remain live and do not reboot. Perform no RF transmissions and restore the original backend/configuration afterward.

## Assumptions

- GPIO mapping changes require reboot; RF backend changes intentionally do not.
- The connected board is the approved ESP32-S3 N16R8, and its existing NVS/network configuration must be preserved.
