# Rebuild a Barebone Web UI

## Purpose

This plan supersedes `plan/2026-08-03_simplify-web-runtime.md`. The current Web UI is not reduced incrementally. Its streaming, retained-operation, dashboard, and background-refresh architecture is removed and the Web component is rebuilt as a small authenticated control surface.

The work is bounded to two serial-flash gates. A transport proof must pass before the operational UI is built. A failed gate stops the work; it does not trigger another queue, stack, timeout, chunk-size, or Wi-Fi tuning cycle.

## Final Scope

The browser will provide only:

- Bearer-token login using the token provisioned or recovered through UART.
- Radio status and receiver enable/disable.
- Manual refresh of the last accepted RF frame.
- Learned-signal catalog, arm/cancel learning, and forget signal.
- OTA status and authenticated firmware upload.

UART remains the interface for Wi-Fi scan/configuration, radio start/reset, replay/transmit, automation rules, console formatting, token rotation, detailed diagnostics, and event output.

The Web UI will not contain SSE, WebSockets, event history, a console mirror, operation records, request deduplication, background workers, background polling, Wi-Fi management, RF transmit/replay, automation management, token rotation, or aggregate system status.

## Resource Contract

- HTTPD is the only Web request task. `web_service` remains only for network-driven start/stop ownership.
- Web API initialization allocates no queues, mutexes, tasks, client records, event storage, or operation tables.
- Keep the HTTPD stack at 8,192 bytes and the service stack at 4,608 bytes for the proof; do not tune either during this plan.
- Use four HTTP sockets, strict route matching, one request at a time from the browser, and straightforward bounded responses.
- Mutation bodies are capped at 256 bytes. JSON response workspaces are capped at 2 KiB; the compact signal catalog omits raw waveform payloads.
- Final embedded assets are `index.html` and `app.js` only. CSS is embedded in the HTML. Total deterministic gzip size must remain below 12 KiB and each response below 8 KiB compressed.
- Hardware must reach at least 28 KiB free internal heap with a largest block of at least 24 KiB after Web-ready. Failure to meet either threshold stops hardware HTTP testing.

## Gate 1: Transport Proof

### Rewrite the runtime

1. Create an AFT checkpoint for all current `components/web_ui`, Web test, CMake, README, and configuration files before destructive edits.
2. Replace `components/web_ui/web_api.cpp` and its private header with a minimal dispatcher containing only:
   - `GET /api/v1/auth/status` to verify a bearer token.
   - `GET /api/v1/radio` to exercise one real typed status service.
   - A temporary authenticated `GET /api/v1/probe` returning an 8 KiB deterministic flash-resident payload for multi-segment TCP verification.
3. Delete all SSE, event-client, broker-sink, event-queue, event-worker, operation-queue, operation-worker, request-ID, fingerprint, retained-result, aggregate-status, and stream lifecycle code.
4. Simplify `components/web_ui/web_ui.cpp` to register static assets, OTA handlers, and the minimal API transactionally. Remove stream start/stop handling and restore a single `httpd_resp_send()` per gzip asset.
5. Replace the frontend with a minimal authenticated page that shows radio status and has no timer, workflow, or mutation controls. The page must issue only one request at a time.
6. Reduce `components/web_ui/CMakeLists.txt` dependencies to those actually required by HTTPD, network lifecycle, OTA, radio status, and authentication.

### Prove before continuing

- Run host tests, Unity compilation, production verification, JS/shell syntax checks, compressed-asset contracts, `git diff --check`, and compiler stack-usage inspection.
- Confirm through the map/symbol output that `web_events`, `web_operation`, `/api/v1/events`, event queues, operation queues, and stream-client state are absent.
- With renewed explicit hardware authorization, perform one serial flash without erasing NVS. Keep one controlled monitor session open so CP2102 modem-line resets cannot invalidate results.
- Verify the heap thresholds, then byte-compare 100 authenticated probe responses and 100 radio-status responses with RF receive disabled. Repeat 100 of each under normal receive activity.
- Require zero timeout, truncated body, `EAGAIN`, reboot, stack violation, or declining steady heap.

If Gate 1 fails, stop. Preserve the logs and report that the fault remains in the Wi-Fi/lwIP/board transport path despite removal of Web runtime pressure. Do not proceed to Gate 2 and do not modify transport settings under this plan.

## Gate 2: Minimal Operational UI

Proceed only after the user reviews the Gate 1 evidence and gives an explicit go-ahead.

### API

- Remove the temporary probe endpoint.
- Keep `GET /api/v1/auth/status` and `GET /api/v1/radio`.
- Add only these strict typed routes:
  - `PUT /api/v1/rx` with `{ "enabled": true|false }`.
  - `GET /api/v1/frames/last`.
  - `GET /api/v1/signals` returning names, encoding type, learned count, and current learning state only.
  - `POST /api/v1/learn` with a validated signal name.
  - `DELETE /api/v1/learn`.
  - `DELETE /api/v1/signals` with a validated signal name.
  - Existing `GET /api/v1/ota/status` and `POST /api/v1/ota` handlers.
- Execute these short typed mutations synchronously and return their actual result. Do not retry automatically. Disable the initiating browser control until its request completes.
- Keep all parsers route-specific with exact-field validation. Do not add a generic UART command endpoint or generic operation schema.

### Frontend

- Build one quiet, compact page with three views: Receive, Signals, and Update.
- Use explicit Refresh controls; do not poll automatically. Refresh the affected snapshot once after a successful mutation.
- Store the bearer token in browser session memory only. Provide logout, but leave token rotation to UART.
- Show bounded error text from typed API responses. Do not recreate timelines, event badges, operation tables, diagnostics panels, or local display preferences.
- Remove all unused styles, icons, API constants, and DOM contracts rather than hiding old features.

### Documentation and tests

- Update `README.md` with the exact barebone Web scope and the UART-only feature list.
- Rewrite `host_tests/test_web_assets.mjs` to assert the reduced endpoint allowlist, no background timers, no SSE/WebSocket/event/operation/request-ID strings, no generic command body, CSP, offline assets, and the 12 KiB compressed budget.
- Add focused parser/handler-helper tests for RX, learning, and signal deletion. Preserve existing auth and OTA tests.
- Re-run all Gate 1 compiler and production checks. Review the final diff specifically for orphaned streaming/operation code and unnecessary component dependencies.

## Final Hardware Acceptance

With renewed authorization, perform one final serial flash without erasing NVS:

1. Confirm the same heap and stack thresholds at Web-ready.
2. Load and byte-compare both assets 100 times with receive off and 100 times with receive on.
3. Verify unauthorized rejection and authenticated radio, RX toggle, frame, signal list, learning arm/cancel, and signal-forget behavior.
4. Leave the page idle for 30 minutes and require no heap decline, reconnect loop, HTTPD warning, or reboot.
5. Validate OTA status and upload the same approved production image through the Web route; confirm reboot and return to the UI.
6. Verify UART recovery and excluded commands still work. Do not transmit RF, erase NVS, run on-device Unity, or change partitions.

## Hard Stop Rules

- Maximum two serial development flashes: Gate 1 and final Gate 2. The final OTA upload is acceptance of the already approved image, not another code iteration.
- Any Gate 1 transport failure ends this implementation attempt on the current runtime architecture.
- Any Gate 2 failure gets one diagnostic capture and a written assessment, not another patch cycle.
- Excluded features are not added back during this plan. Future features require separate proposals and must be added one at a time against the proven memory baseline.
