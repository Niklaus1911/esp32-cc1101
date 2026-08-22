# Second-Pass Review of Uncommitted SSE Work

## Summary

Perform a findings-first review of every staged, unstaged, and untracked source, test, documentation, and plan file. Trace the complete event path from automation and RF producers through the bridge, MQTT/UART, SSE worker, browser refresh logic, and polling fallback. Fix every confirmed defect, add focused regressions, and revalidate proportionally.

Before editing, refresh the inventory so newly added uncommitted files are included.

## Review and Fix Pass

- Inspect every diff line and untracked file. Record each finding with severity, evidence, affected behavior, and file/line before fixing it. Avoid unrelated refactoring.
- Audit automation configuration events for lock boundaries, sink/context lifetime, reentrancy, persistence failures, no-op changes, exactly-once delivery, revision monotonicity, and add/remove/enable/log-mode consistency.
- Audit bridge fan-out for three-sink service combinations, tagged-union copying, exhaustive event handling, queue and sink drop accounting, sink removal quiescence, and MQTT/UART behavior.
- Audit SSE ownership and concurrency: start rollback, repeated start/stop, worker-creation failure, disconnect, server shutdown, sink registration failure/retry, queue deletion races, async request completion exactly once, and restart after Wi-Fi loss.
- Check resource bounds and protocol correctness: socket/LRU behavior, queue and stack costs, maximum escaped payload size, all event families, sequence gaps, queue overflow/resync, heartbeat behavior, Host/body validation, second-client rejection, and disabled-SSE behavior.
- Audit the browser state machine across visible/hidden, busy, OTA, unsupported EventSource, HTTP 404/409, disconnect, reconnect, watchdog, and resync states. Eliminate duplicate or overlapping refreshes and ensure Overview, Signals, and Rules receive the intended targeted updates.
- Verify rule highlighting fires once only for `automation.kind === "triggered"`, never restarts for completion/cooldown, remains polling-compatible, and survives refresh/reconnect timing without stale DOM state.
- Review tests and documents for assertions that merely match source text, missing failure/concurrency coverage, stale architectural claims, and disagreement with the implemented API.

## Interfaces

Preserve these intended contracts unless the review proves one unsafe:

- `RfAutomationConfigEvent` and its sink report successful persisted configuration changes only.
- `BridgeEventType::kAutomationConfig` remains bounded and trivially copyable across all consumers.
- `GET /api/events` remains a one-client typed notification stream with 15-second heartbeats, deterministic `409` rejection, bounded resync, and `404` when disabled.
- `/api/live` remains authoritative; `/api/live.automation.configuration_revision` and `/api/rules.revision` support reconciliation.
- SSE remains the default for the first visible client, while polling remains the fallback and recovery path.

Any necessary contract correction must update producers, consumers, browser handling, tests, and documentation together.

## Test and Validation Plan

- Add focused host or Unity regressions for every confirmed defect, including configuration success/no-op/failure cases, revision counts, three-sink capacity, payload boundaries, event-family serialization, overflow/resync, and lifecycle failure paths where practical.
- Run JavaScript syntax checks and the clean native host suite with CMake and CTest.
- Compile the dedicated Unity image for both ESP32 and ESP32-S3 targets; do not run on-device Unity tests.
- Use Playwright MCP against a deterministic local mock first. Cover SSE success, malformed or missed events, 404/409 fallback, second tab, disconnect/reconnect, hidden/visible transitions, watchdog resync, targeted page refreshes, and one rule animation for a triggered/completed/cooldown sequence.
- Inspect desktop `1440x900` and mobile `390x844` screenshots, horizontal overflow, console/page errors, request concurrency, and visual regressions. Intercept transmit, deletion, hardware switching, OTA, and other destructive mutations.
- If Playwright MCP remains transport-closed, leave its installation untouched, run the equivalent checks with the installed Playwright library, and report that fallback explicitly.
- Run a focused N16R8 firmware build after firmware or embedded-asset fixes. Run `tools/verify-production.sh` once on the final unchanged candidate whenever production output changed; reuse the prior successful result only if the production inputs remain identical.
- When production output changed, flash only `esp32s3-devkitc-n16r8` through `tools/build-board.sh` and `/dev/serial/by-id/usb-EXAMPLE_N16R8-if00`, without erasing NVS. Then perform read-only device checks for stream heartbeat, API responsiveness, reconnects, repeated open/close resource stability, and second-tab polling.
- Finish with `git diff --check`, a complete final diff review, and `git status --short`. Report findings first, then fixes, validation evidence, and residual risks.

## Assumptions

- All confirmed defects, regardless of severity, will be fixed; if none are found, no cosmetic code churn will be introduced.
- Existing flash permission applies to the confirmed N16R8 board and exact approved path only. It does not authorize RF transmission, destructive Web actions, serial monitoring, NVS erasure, or on-device Unity tests.
- No commit or push is included in this pass.

## Completion

- Reviewed every tracked diff and untracked source, test, documentation, and plan file.
- Fixed SSE teardown/admission races, sinkless-stream fallback, configuration-sink lifetime, OTA transport recovery, refresh serialization, learning-request duplication, rule-action deduplication, and SSE startup snapshot overlap.
- Added or updated host and Unity regressions for SSE framing, event payloads, sink admission, callback quiescence, Web lifecycle ordering, API fields, and browser contracts.
- Passed JavaScript syntax and asset checks, all 8 host tests, both ESP32 and ESP32-S3 Unity compile-only images, and the clean four-profile production verifier.
- The Playwright MCP browser transport remained closed after a reset and retry. The installed Playwright fallback passed the deterministic desktop/mobile mock matrix and read-only device matrix, including one animation per action ID, 404/409 fallback, reconnect, second-tab takeover, serialized delayed refreshes, OTA recovery, same-origin mutation headers on the mock, no overflow, and no unexpected page errors.
- Flashed the verified `esp32s3-devkitc-n16r8` image through the approved persistent by-id path without erasing NVS. On-device checks confirmed the live action-ID schema, hello and heartbeat delivery, deterministic `409`, responsive snapshots during a stream, reconnect/takeover, and zero free-heap delta after 12 short stream cycles.
