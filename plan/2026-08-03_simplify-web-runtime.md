# Simplify the Web Runtime

## Context

The current firmware reaches Web-ready with about 11 KiB of free internal heap, but HTTP bodies can stall after one TCP segment. Further queue and stack shaving has increased complexity without establishing reliable transport. The largest removable subsystem is Web event streaming: its 7,168-byte task, two queues (about 4.5 KiB of payload storage), client mutex/state, asynchronous HTTP requests, and browser timeline are optional presentation facilities rather than control-plane requirements.

The simplified target is a snapshot-only Web UI. It keeps bearer authentication, typed REST mutations, request deduplication, the serialized operation worker, OTA, embedded gzip assets, station-only Wi-Fi, and UART recovery. It removes SSE and the browser event console/timeline entirely. No event-polling ring replaces SSE.

## Architecture Decisions

- Keep the existing operation queue, worker, six retained records, request IDs, and `/api/v1/operations` polling. This protects the HTTP stack and serializes RF/Wi-Fi mutations; removing it is a separate change only if the first simplification is insufficient.
- Remove `/api/v1/events`, SSE clients, asynchronous request ownership, heartbeats, event serialization, Web event queues, the event worker, and the Web broker sink.
- Keep the shared broker for UART, subsystem lifecycle, automation, and Web-originated operation-completion audit events.
- Make `/api/v1/status` shallow: uptime plus Web/auth/operation health only. Do not call `bridge_control_get_status()` from the HTTP task. The frontend obtains radio, signals, rules, Wi-Fi, OTA, and last-frame state from their existing typed snapshot endpoints.
- Add only the retained state that polling cannot currently recover: a bounded Wi-Fi scan snapshot and a compact terminal learning outcome. Do not retain a general event history.
- Use one browser polling scheduler with at most one REST request in flight. Poll only the active view or an active workflow; pause routine polling while the document is hidden.
- Keep the 8 KiB HTTP task and six socket capacity for the first validation. Restore straightforward single-call gzip asset responses after reclaiming Web event memory; do not combine this change with Wi-Fi power-mode tuning.

## Implementation

### 1. Remove the Web streaming runtime

- In `components/web_ui/web_api.cpp`, delete event-client, compact-event, stream-parser/serializer, queue-merge, heartbeat, and broker-sink code; remove the RF/system event queues, client mutex, event task, stream counters, `/api/v1/events`, and all related initialization/rollback paths.
- In `components/web_ui/private_include/web_api.hpp`, remove stream lifecycle functions and event/client fields from `WebApiStatus`; retain operation, authorization, initialization, and queue-drop status.
- In `components/web_ui/web_ui.cpp`, remove stream start/stop calls from HTTPD startup, rollback, reconnect, and shutdown. Keep transactional server ownership and readiness gating.
- In `components/web_ui/web_api.cpp`, replace aggregate status collection with a shallow response that cannot enter radio command transport. Remove `bridge_control` from `components/web_ui/CMakeLists.txt` if no remaining Web source uses it.
- Restore the gzip asset handler to one `httpd_resp_send()` call and remove the experimental 1 KiB delay loop. Re-test only after the event task/queues are gone.

Expected internal-memory recovery: at least the 7,168-byte event stack plus roughly 4.5 KiB of queue storage and queue/client/task control structures. The Web-ready target is at least 20 KiB free internal heap with a largest block of at least 16 KiB.

### 2. Make workflows snapshot-driven

- Extend `RfSignalsStatus` in `components/rf_signals/include/rf_signals.hpp` with a monotonic learning revision and compact retained terminal outcome (completed, replaced, cancelled, timed out, or failed), name, and error. Update every terminal path in `components/rf_signals/rf_signals.cpp` and serialize it from the existing signals endpoint.
- Add a bounded Wi-Fi scan snapshot API in `components/network_wifi/include/network_wifi.hpp` and `components/network_wifi/network_wifi.cpp`. Retain at most 8 deduplicated AP records, scan revision/running/completed state, and terminal error; clear records when a new scan starts. Expose it as `GET /api/v1/wifi/scan`.
- Keep last RF activity on `GET /api/v1/frames/last`, operation completion on `GET /api/v1/operations`, network connection state on `GET /api/v1/wifi`, and OTA state on `GET /api/v1/ota/status`.
- Do not add general event storage, pagination, long polling, WebSockets, or a second HTTP server.

### 3. Reduce the frontend to operational views

- In `components/web_ui/assets/index.html`, remove the Console navigation/view, event timeline, stream status, event metric, timeline controls, and local autoscroll preference. Keep recent operations, controls, catalog, rules, network, OTA, settings, and last-frame views.
- In `components/web_ui/assets/app.js`, delete SSE parsing/reconnect/gap logic and the 500-entry browser event store. Implement a single-flight scheduler:
  - Poll pending operations every 1 second until terminal.
  - Poll last frame every 2 seconds only on Overview/Signals.
  - Poll signals every 2 seconds only while learning is armed.
  - Poll Wi-Fi/scan every 2 seconds only during scan/connect transitions.
  - Poll the active view every 10 seconds otherwise; keep manual refresh.
  - Stop routine polling while hidden and perform one refresh when visible again.
- Refresh operation rows from `/operations`; do not infer operation completion from events. Render learning and scan completion from the new retained snapshots.
- In `components/web_ui/assets/app.css`, remove timeline/console/stream-only styles and preserve the existing compact dashboard layout.

### 4. Update contracts and documentation

- Update `host_tests/test_web_assets.mjs` to require snapshot polling and to reject `text/event-stream`, `/api/v1/events`, and generic UART command bodies. Keep CSP, offline asset, typed mutation, gzip-size, and request-ID contracts.
- Add focused Unity coverage for learning-outcome retention and bounded/deduplicated Wi-Fi scan snapshots where component seams permit; otherwise add host-testable pure snapshot helpers and test those.
- Update `README.md` to document snapshot polling, removed live timeline/console, retained operation semantics, Wi-Fi scan retention, and UART as the full event/recovery interface.
- Remove stale SSE limits, metrics, assertions, comments, and test expectations. Do not undo the tagged `BridgeEvent` layout or shared broker work used outside Web UI.

## Verification

1. Run `git diff --check`, shell syntax checks, `node --check`, and the Web asset contract test.
2. Run clean host tests, the dedicated Unity build, and `tools/verify-production.sh`.
3. Inspect compiler stack usage for the HTTP dispatcher, shallow status handler, operation worker, and OTA handlers; keep at least the existing 2 KiB HTTPD margin.
4. Inspect the map/build logs to confirm `web_events`, SSE client state, RF/system Web queues, and stream symbols are absent.
5. Before hardware use, obtain renewed explicit authorization. Flash production without erasing NVS and keep one controlled serial session open to avoid CP2102 reset ambiguity.
6. Require Web-ready internal heap >=20 KiB and largest block >=16 KiB. If this is not met, stop and account for allocations before HTTP testing.
7. With RF receive temporarily off, fetch/decompress/compare `/`, `/app.css`, and `/app.js` repeatedly; verify unauthorized and authenticated snapshots, operation polling, learning status, Wi-Fi scan snapshots, OTA status, and no `/events` route.
8. Re-enable RF receive and repeat asset, status, and operation polling under sustained receive activity. Confirm no `EAGAIN`, reboot, queue leak, or declining heap/stack watermark.
9. With separate approval, exercise two Wi-Fi reconnect cycles and an authenticated OTA upload. Do not transmit RF or erase NVS as part of this plan.

## Stop Conditions

- Do not remove the operation worker in the same patch. If HTTP still stalls with the event runtime absent and the memory targets met, classify the remaining issue as Wi-Fi/lwIP transport behavior and investigate it independently.
- Do not reintroduce SSE, a polling event ring, or larger HTTP stacks to make acceptance pass.
- Do not declare hardware acceptance from compilation, one successful request, or a run performed while serial reset state is ambiguous.
