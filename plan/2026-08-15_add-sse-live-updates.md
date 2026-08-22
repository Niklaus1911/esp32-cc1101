# Add SSE Live Updates

## Objective

Add a bounded Server-Sent Events transport for the trusted-LAN Web UI. SSE is the default live notification path for one visible browser tab, while `/api/live` remains the authoritative snapshot and the existing visibility-aware polling remains the compatibility and recovery path.

## Firmware

- Add `GET /api/events` with the existing device-Host and empty-body validation.
- Register one Web bridge-event sink and copy bounded typed envelopes into a fixed queue of depth 8; never perform socket I/O from the broker callback.
- Serve the stream through an asynchronous HTTP request worker, with one active client, deterministic bounded rejection of additional clients, 15-second comment heartbeats, and clean shutdown on disconnect, Wi-Fi/server stop, and OTA.
- Emit `hello`, `rx`, `automation`, `automation_config`, `catalog`, `learning`, `network`, `tx`, `hardware`, `ota`, and `resync` events. Use the bridge sequence as the SSE id and bounded JSON fields; do not expose the internal event union or full live snapshot.
- Detect local queue overflow and broker sink drops, clear stale queued events, and emit `resync` so the browser fetches a fresh snapshot.
- Add automation configuration-change events for successful rule add/remove, enable, and log-mode changes from Web, UART, and MQTT. Include `configuration_revision` in `/api/live` automation data and `revision` in `/api/rules`.
- Increase bridge sink capacity for UART, MQTT, and Web; reserve an HTTP socket for ordinary requests (`max_open_sockets = 3`) and add the SSE route handler budget. Add an enabled-by-default `CONFIG_WEB_SSE_ENABLE` gate for profile memory control.

## Browser

- Keep one `EventSource` while the page is visible and no conflicting action/OTA transition is active; close it while hidden.
- Fetch `/api/live` on `hello`, stream open, coalesced event refreshes, resync, reconnect, and a 30-second watchdog. Coalesce notifications for roughly 100 ms.
- Refresh `/api/recent` when RX activity affects the Signals view or the live revision changes. Pulse the exact Rules item immediately for automation events, then reconcile counters from `/api/live`; use the automation action ID to deduplicate a snapshot pulse across connecting and disconnecting SSE transitions.
- Reconnect with bounded backoff. On stream rejection, unsupported browser, disconnect, or repeated failure, continue the existing polling loop. A second tab remains usable through polling.

## Tests and Validation

- Add bounded SSE serialization and event/configuration revision coverage to host/Unity tests.
- Update Web asset tests for `/api/events`, socket/handler limits, EventSource startup/fallback/coalescing, target-page refresh, and removal of the old text/event-stream prohibition while preserving asset-size limits.
- Run clean native host tests and the dedicated Unity image build.
- Run Playwright against a deterministic mock for desktop/mobile screenshots, RX/automation/config/resync/disconnect/reconnect/second-client cases, no overflow, and no page/console errors; then perform read-only validation against the flashed device.
- Build the N16R8 profile, run the required unchanged-candidate production verifier, and flash only with the approved by-id path without erasing NVS.
