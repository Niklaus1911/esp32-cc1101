# Full Wi-Fi Web UI

## Assumptions To Confirm

- Keep the existing HTTP port `8032` and the current station-only Wi-Fi model. The Web UI starts after DHCP succeeds and stops after IP loss.
- `wifi stop`, `wifi forget`, and connecting to another network are exposed, but they intentionally disconnect the browser after the response; UART remains the recovery path when Wi-Fi is offline. No fallback access point is added in this release.
- Use plaintext HTTP on the documented trusted LAN, but require a unique per-device bearer token for every API and OTA request. This does not protect against LAN sniffing; HTTPS, signed firmware, and Secure Boot remain separate hardening work.
- Support two simultaneous live-console clients. Keep browser assets dependency-free at runtime and embedded in the firmware; no CDN, SPIFFS, or external web service.
- Proposed UART learned annotation: `RX learned=gate RC ...` in plain mode and `[RF RX] learned=gate RC ...` in pretty mode. The Web console shows the learned name as a distinct badge.

## Architecture

Do not expose a generic endpoint that executes UART command strings. Extract shared typed behavior and keep UART and HTTP as adapters over the same validation and subsystem APIs.

1. Add `components/bridge_events/` as the sole structured event broker for accepted RX frames, direct TX, learning, automation, Wi-Fi, OTA, Web lifecycle, and asynchronous operation results. Producers perform only bounded zero-wait publication; UART and Web consumers have independent fixed queues and drop counters.
2. Add `components/rf_signals/` to own the single pending learn request and an in-memory learned-signal catalog. Move learning out of `rf_console.cpp`; preserve the current 30-second post-arm window, replacement behavior, queue-overflow cancellation, NVS safety, and no-overwrite rule.
3. Add `components/bridge_control/` as a typed facade for radio/RX operations, decoded and raw TX, RAM/named replay, learned-signal mutations, rule mutations, Wi-Fi commands, status snapshots, and operation IDs. UART invokes it synchronously; the Web adapter submits mutations to one bounded worker and returns quickly.
4. Add `components/web_ui/` as the sole owner of `httpd_handle_t`, static assets, authentication, REST handlers, SSE clients, server lifecycle, and Web-specific status counters.
5. Refactor `ota_update` so it retains upload validation, streamed writes, maintenance, rollback confirmation, progress events, and reboot ownership, but registers its status/upload routes on the shared Web server instead of starting a separate HTTP server.
6. Change `main/main.cpp` so the Wi-Fi online sink notifies `web_ui`. On `GOT_IP`, the Web service task starts HTTPD, registers every route transactionally, and prints the URL to UART. On disconnect it rejects new work, closes SSE clients, waits for any active OTA cleanup, and stops HTTPD. Startup or Web failures must not stop RF, UART, storage, automation, or Wi-Fi.

## Learned RX Identification

1. Add `CONFIG_RF_MAX_LEARNED_SIGNALS` with a bounded default of 32. Load names and decoded/raw records into a catalog outside the RF callback; never scan NVS on each frame.
2. Use the existing canonical `rf_stored_signals_equivalent()` matching so decoded/raw aliases behave exactly like automation matching. Return `none`, `unique`, or `ambiguous` rather than silently choosing one of multiple equivalent learned records.
3. Refresh the catalog transactionally after successful learn or forget. If NVS contains more records than the configured bound, preserve all data and keep list/load/replay/forget working, but mark annotation unavailable and reject additional learning until the count is reduced.
4. The RF callback remains short: enqueue automation as today, then enqueue the frame to `rf_signals`. Its worker handles pending learning, performs learned lookup, and publishes an enriched RX event.
5. If a frame completes learning successfully, commit it first and publish that same RX event with the new learned name. Failed commits remain unannotated and emit a learn error.
6. Extend UART and Web output with `learned_name`, `learned_match_count`, and ambiguity state. Apply the same annotation to `last`/latest-frame views. Unmatched UART records remain byte-for-byte unchanged.

## Command And API Parity

Create a compile-time capability map and host test requiring every custom UART command to have a typed Web capability:

- `status` -> aggregate dashboard/status API.
- `console style` -> inspect/change UART output style from Settings.
- `wifi status/connect/start/stop/forget/scan` -> Network view.
- `ota status` and HTTP upload -> Update view.
- `radio info/start/reset` and `rx on/off` -> Radio controls.
- `last` -> latest-frame view.
- `learn`, `learn list`, and `forget` -> Signals view and learn workflow.
- `rule add/list/remove/enable/disable/log` -> Rules view.
- `replay` latest or named -> Transmit view.
- `send` -> validated decoded-transmit form.
- `raw begin/append/show/send/clear` -> browser-local raw editor that submits one complete bounded raw signal; UART staging remains UART-local.

Suggested exact API surface:

- `POST /api/v1/auth` and `GET /api/v1/auth/status`
- `GET /api/v1/status`, `GET /api/v1/events`, `GET /api/v1/operations?id=...`
- `GET/PATCH /api/v1/console`
- `GET /api/v1/radio`, `POST /api/v1/radio/start`, `POST /api/v1/radio/reset`, `PUT /api/v1/rx`
- `GET /api/v1/frames/last`
- `GET/POST/DELETE /api/v1/signals`, `POST/DELETE /api/v1/learn`
- `POST /api/v1/replay`, `POST /api/v1/tx/decoded`, `POST /api/v1/tx/raw`
- `GET/POST/DELETE /api/v1/rules`, `PATCH /api/v1/rules/settings`
- `GET /api/v1/wifi`, `POST /api/v1/wifi/scan`, `POST /api/v1/wifi/connect`, `POST /api/v1/wifi/start`, `POST /api/v1/wifi/stop`, `POST /api/v1/wifi/forget`
- Preserve `GET /api/v1/ota/status` and `POST /api/v1/ota`.

Use exact HTTP methods, strict content types, bounded request bodies, typed JSON fields, and clear status codes. Parse JSON with ESP-IDF's cJSON using length-bounded input and exact field validation. Never log passwords, bearer tokens, raw Authorization headers, or Wi-Fi credential bodies.

All potentially disruptive or long operations return `202 Accepted` plus an operation ID. Completion is available through SSE and the operation-status endpoint. Delay Web-originated Wi-Fi stop/forget/replacement until after the HTTP response is sent. Reject mutations with `409` while OTA maintenance or reboot is active.

## Live Console And Event Semantics

1. Use authenticated Server-Sent Events at `GET /api/v1/events`, implemented with `httpd_req_async_handler_begin()` and one dedicated bounded streaming worker. Use authenticated `fetch()` streaming in the browser rather than native `EventSource`, because the browser must send an Authorization header.
2. Emit monotonically sequenced `rx`, `tx.started`, `tx.completed`, `learn.*`, `rule.*`, `wifi.*`, `ota.*`, `operation.*`, `web.*`, and `gap` events. Include timestamps, source (`uart`, `web`, or `automation`), result/error, and learned metadata where relevant.
3. Preserve the persisted automation log-mode policy for rule runtime events so UART and Web consoles show the same action/verbose/off behavior.
4. Send a heartbeat every 15 seconds. Disconnect slow or failed clients after bounded send timeouts, complete every async request exactly once, and report queue/client drops in aggregate status.
5. Keep only a small firmware-side replay window or no history; the browser retains a capped local timeline (for example 500 rows). Sequence gaps are explicit and trigger a state refresh rather than unbounded ESP32 buffering.

## Security

1. Add a versioned NVS Web-auth record containing only a SHA-256 verifier for a random 256-bit token. Generate the first token once and print it once on UART; add `web auth status` and `web auth rotate` for physical recovery without putting secrets in command arguments/history.
2. The login page keeps the entered token only in tab-scoped `sessionStorage`. Every API, SSE stream, and OTA upload sends `Authorization: Bearer ...`; compare verifiers in constant time and rate-limit failures.
3. Serve static assets without credentials, but expose no device data until authentication succeeds. Emit no CORS headers. For browser mutations require same-origin `Origin`/`Host` consistency and a custom JSON request header; permit authenticated non-browser OTA clients without `Origin`.
4. Add CSP, `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`, `frame-ancestors 'none'`, no inline event handlers, no third-party scripts, and correct JSON/HTML escaping for SSIDs and learned names.
5. Update `tools/push-ota.sh` to require the token from a non-command-line source such as `RFBRIDGE_TOKEN` and send the Authorization header. Extend its host tests for missing, invalid, and present credentials.
6. Document clearly that bearer auth over HTTP prevents casual/cross-origin control but not packet capture on an untrusted LAN.

## Frontend

Build the operational application as the first screen, not a landing page. Use authored `index.html`, `app.css`, and modular browser JavaScript embedded with `EMBED_TXTFILES`; bundle a small local Lucide icon subset or sprite. No Node toolchain is required for the firmware build.

- **Overview:** RF, RX, automation, Wi-Fi, OTA, storage, queue/drop health, latest frame, and safe quick actions.
- **Console:** dense live timeline with RX/TX/RULE/LEARN/WIFI/OTA/SYSTEM filters, pause/autoscroll, clear-local, export JSONL, expandable raw timings, gap indicators, and learned-name badges.
- **Signals:** bounded learned list, decoded/raw details, replay, delete confirmations, and an armed-learning modal with countdown and completion state.
- **Transmit:** decoded form with decimal/hex code, bits, protocol, pulse width, repeats; named/latest replay; raw textarea/table editor with start-level control, validation, pulse count/duration, and a canvas waveform preview.
- **Rules:** table of trigger -> target, repeats, cooldown, validity, add/remove controls, global enable toggle, and log-mode segmented control.
- **Network:** live state, IP/RSSI/retry/drop data, scan results, password-safe connect dialog, start/stop/forget confirmations, and explicit warning that disruptive actions end the Web session.
- **Update:** OTA status, image picker, XHR upload progress, maintenance state, result, and reboot/reconnect view.
- **Settings:** UART console style, Web-auth rotation, server/client counters, and firmware metadata.

Use a restrained operational layout with full-width work areas, compact panels/tables, semantic cyan/green/magenta/amber/red accents matching the UART console, 8 px or smaller radii, stable control dimensions, responsive navigation, keyboard focus, accessible dialogs, and no decorative marketing sections.

## Resource Limits

- Keep normal JSON requests at or below 4 KiB; OTA remains streamed in 2 KiB chunks.
- Cap URI handlers, sockets, async requests, SSE clients, operation slots, event queues, and authentication attempts explicitly.
- Keep embedded HTML/CSS/JS/icons under a reviewed 96 KiB flash budget and retain the production verifier's 25% OTA-slot growth margin.
- Avoid per-event heap allocation and queued JSON strings. Queue typed fixed-size structs and serialize in the Web worker.
- Do not add SPIFFS or change `partitions_ota.csv`; UI and API must update atomically with the application image.

## Verification

1. Add host tests for shared validation, command/API capability parity, learned catalog unique/ambiguous matching, learn deadlines/overflow, event sequencing and gap handling, operation state, JSON escaping, auth verification/rate limits, and deterministic asset manifests.
2. Add Unity tests for component startup rollback, sink registration/removal, Web online/offline transitions, queue exhaustion, low-memory cleanup, SSE admission/disconnect, async-request completion, OTA route registration, active-upload shutdown deferral, and maintenance mutation rejection.
3. Add lightweight frontend tests with mocked REST/SSE responses plus Playwright screenshots at desktop and mobile widths. Verify no overlap, bounded timeline growth, forms, dialogs, raw waveform rendering, reconnect states, and every command capability.
4. Run host CMake/ctest, `tools/verify-production.sh`, image/size inspection, and the dedicated Unity image build. Assert the embedded-asset budget and route/capability manifest in production verification.
5. With explicit board/port approval, test DHCP loss/reconnect, two browsers, stalled clients, UART and Web commands concurrently, high-rate RF RX, direct/automation TX, learning from both interfaces, ambiguous learned aliases, rule mutations, Wi-Fi replacement/disconnect, browser OTA plus CLI OTA, failed uploads, maintenance recovery, reboot, and rollback.
6. Do not claim RF timing, Wi-Fi recovery, OTA concurrency, NVS persistence, or browser behavior validated until those hardware/browser tests are actually run.

## Expected File Areas

- New `components/bridge_events/`, `components/bridge_control/`, `components/rf_signals/`, and `components/web_ui/` components with focused host/Unity tests.
- Refactors in `components/rf_console/`, `components/ota_update/`, `components/network_wifi/`, `main/main.cpp`, and unit-test component wiring.
- Embedded assets under `components/web_ui/assets/`.
- Auth storage/policy code, OTA uploader updates, production-verifier assertions, and `README.md` documentation.
