# Review, Repair, and Commit Pending RF Work

## Summary

Review the complete uncommitted candidate covering offline random RF generation, UART/HTTP/Web interfaces, tests, board documentation, and retained plans. Fix every actionable finding, repeat the review, commit the complete approved inventory, then verify and flash the exact committed N16R8 firmware without transmitting RF.

## Review and Fixes

- Audit RNG masking, protocol/timing construction, generated names, semantic and name collisions, bounded retries, NVS errors, catalog refresh, Home Assistant reconciliation, and the no-transmit invariant.
- Audit UART argument handling/output and `POST /api/signals/random` origin validation, empty-body enforcement, statuses, JSON bounds, and handler registration.
- Fix the observed Web refresh race: keep live polling suspended through post-save signal/rule refreshes so the two-socket HTTP server cannot receive a competing `/api/live` request.
- Review responsive Web behavior and README/tooling claims against board policy, GPIO defaults, OTA layouts, approved access paths, and actual offline entropy behavior.
- Add focused regressions for confirmed findings. Avoid adding dependency-injection machinery solely to test the RNG.

## Interface Contracts

- Preserve `random` as a no-argument UART command.
- Preserve `POST /api/signals/random` with an empty body, `201` success payload, and `503 random_generation_exhausted`.
- Preserve 24-bit protocol-1 signals named `random_%06X`, eight retries, create-only storage, catalog publication, and automatic Home Assistant replay-button discovery.
- Do not add MQTT generation, transmit, replay, backend switching, or NVS-format changes.

## Verification

- Run `git diff --check`, JavaScript syntax checks, the clean native host suite, and an ASan/UBSan host build.
- Compile the Unity images for ESP32 and ESP32-S3.
- Validate the deterministic Web UI on desktop and mobile, including success/error flows, refresh ordering, same-origin mutation headers, overflow, screenshots, and absence of console/page errors or RF requests.
- Stage the explicit reviewed inventory, inspect the complete cached diff and statistics, then commit with `feat: add offline random RF signals and refresh board docs`.
- Run `tools/verify-production.sh` after committing because the Git-derived firmware version changes.
- Flash and monitor only `esp32s3-devkitc-n16r8` through `tools/build-board.sh` and `/dev/serial/by-id/usb-EXAMPLE_N16R8-if00`.
- Smoke-test offline UART generation and, when the existing Web service is available, the physical Web action. Confirm persistence/catalog visibility, remove temporary signals, and restore the initial Wi-Fi state.
- If post-commit verification exposes a defect, fix it in a follow-up commit and repeat affected gates; do not amend or push.

## Assumptions

- Every currently modified or untracked source, test, documentation, and plan file belongs to this candidate and should be committed.
- Existing NVS records and network configuration must remain intact.
- Hardware validation must never replay or transmit a signal.
- Commit only with no unresolved review findings and a clean tracked working tree.
