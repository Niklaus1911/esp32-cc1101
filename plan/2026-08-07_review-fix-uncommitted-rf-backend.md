# Full Review And Repair Of Uncommitted RF Backend Work

## Summary

Review every uncommitted code, test, asset, documentation, and tooling change as one candidate. Fix the confirmed lifecycle, persistence, status-consistency, UI, and documentation issues while preserving the existing CC1101 behavior and the exact Web/MQTT/serial contracts.

Confirmed review findings to address:

- RF start/switch can race because lifecycle and switch guards are acquired in separate phases.
- A stopped-service status read can overwrite an in-progress backend selection from NVS.
- RF-storage mutex allocation failure is not propagated to hardware-selection state.
- Reapplying a backend rewrites NVS even when the valid record already matches.
- Generic RX has no explicit safe pull state when a module is absent or tri-stated.
- Web and pretty-console diagnostics do not render switch error/count fields.
- Web selector handling is incorrect when an older firmware returns a live snapshot without `radio.hardware`.
- README wording does not match actual switch-state publication and persistence-repair behavior.

## Implementation Changes

### RF lifecycle, GPIO, and persistence

- Refactor internal start/stop helpers so a hardware-switch transaction owns one lifecycle guard from state validation through shutdown, target startup, persistence, and rollback.
- Reject switches during `kStarting` or `kStopping`; make normal start, stop, maintenance, and switch operations mutually exclusive under the same protocol.
- Preserve the transaction order: pause automation, stop and release the old backend, start the target, rearm RX, persist only after activation succeeds, and restore the previous backend on activation or persistence failure.
- Keep exactly one RMT backend active; force generic TX low after RMT teardown and configure a defined generic RX pull-down before capture.
- Make hardware/status getters observational: they may report persisted state while stopped but must not mutate shared backend state during a switch.
- Propagate storage initialization failures to every storage subservice.
- Avoid redundant NVS writes while still repairing missing, corrupt, or mismatched selection records.
- Preserve the versioned CRC-protected record format and the existing `RfHardware` values.

### Web, MQTT, serial, and shared events

- Keep the exact public controls:
  - UART: `radio hardware`, `radio hardware cc1101`, `radio hardware generic`
  - Web: `POST /api/radio/hardware` with exactly `hardware=cc1101|generic`
  - MQTT: `rfbridge/<12hex>/radio/hardware/set` with exact non-retained QoS 0 payloads.
- Keep all three surfaces routed through `bridge_control_set_rf_hardware()` and the same automation pause/switch/resume transaction.
- Add output locking and complete switch diagnostics to serial pretty/plain status.
- Normalize missing/unknown hardware fields in the Web UI for older firmware, prevent invalid selector submissions, show active backend/error/count feedback, and resynchronize the selector after failed requests.
- Include switch failures in Web RF health degradation and retain existing backward-compatible live fields.
- Verify MQTT discovery, retained state, command subscription, failure reconciliation, and retirement tombstones; change wire formats only where required by the reviewed behavior.

### Documentation and tests

- Correct README switch semantics, NVS repair wording, generic idle/pull behavior, and safety notes while retaining the board-specific GPIO tables.
- Extend host/source tests for lifecycle serialization, stopped-service status reads, storage initialization/error repair, no-op persistence, generic GPIO safety, old Web schemas, diagnostic rendering, MQTT reconciliation, and exact command rejection.
- Keep `AGENTS.md` tooling instructions isolated from the RF feature changes.

## Verification And Commits

1. Save this plan as `plan/2026-08-07_review-fix-uncommitted-rf-backend.md` before implementation begins.
2. Run clean native host tests, JavaScript syntax/source contracts, and focused diff checks.
3. Compile both ESP32 and ESP32-S3 Unity images.
4. Use Playwright against a deterministic mock and the Web UI to cover success, rollback/failure, old-schema fallback, polling/reconnect, desktop/mobile layout, overflow, and browser/page errors. Do not issue RF transmit requests.
5. Because firmware and embedded assets change, run `tools/verify-production.sh` once on the final unchanged candidate.
6. Flash and monitor only through the approved wrapper and port:
   `tools/build-board.sh esp32-devkit flash --port /dev/serial/by-id/usb-EXAMPLE_CLASSIC-if00`
   followed by the matching `monitor` command. Confirm boot, CC1101 initialization, and read-only serial status; do not switch the connected device to generic mode.
7. Review final `git status` and diff, then create two commits:
   - `fix: complete selectable RF backend review` for RF code, interfaces, README, tests, and the review plan.
   - `docs: require validated tooling for new sessions` for `AGENTS.md` and `plan/2026-08-07_require-tooling-guide-for-new-sessions.md`.

Completion requires a clean worktree, passing gates, consistent rollback/status behavior, and both commits created without push, erase, OTA, or on-device Unity execution.

## Assumptions

- All current uncommitted changes are in scope; unrelated future edits must remain untouched.
- The connected board is the classic `esp32-devkit` at the approved CP2102 by-id path.
- Hardware acceptance is limited to flashing, boot monitoring, and read-only CC1101 diagnostics; generic-module RF timing, range, and live switching remain unverified.
