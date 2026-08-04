# Rebuild the Web UI as Server-Rendered HTML

## Decision

This plan supersedes both earlier Web simplification plans. WebSockets are rejected because they require persistent client state, asynchronous sends, buffering, ping/reconnect logic, and another long-lived socket. The replacement is classic HTTP with server-rendered HTML and strict form posts.

The Web surface has no authentication for now. Because an unauthenticated OTA endpoint would let any LAN client install firmware, Web OTA is disabled in this baseline. Firmware updates use the serial flashing path until authentication and OTA are proposed together as a later feature.

## Final Web Scope

One `GET /` page shows:

- Radio availability and receiver state.
- Last accepted RF frame summary.
- Current learning state.
- Compact learned-signal names and encoding types.

The page provides only these forms:

- `POST /rx` with `enabled=0|1`.
- `POST /learn` with a validated signal name.
- `POST /learn/cancel` with an empty body.

Every successful post returns `303 See Other` to `/`. There is no JavaScript, JSON API, polling, SSE, WebSocket, event timeline, console mirror, operation tracking, Wi-Fi management, signal deletion, replay/transmit, automation rules, settings, diagnostics, authentication, token management, or OTA.

UART retains every omitted control and remains the recovery/administration interface.

## Runtime Contract

- HTTPD is the only request task. `web_service` remains solely for network-online start and connectivity-loss stop.
- The Web component allocates no task, queue, semaphore, client table, event storage, operation record, request deduplication state, or response-sized heap buffer.
- Do not register the Web API wildcard handler, OTA routes, auth routes, or an event-broker sink.
- Use four exact URI handlers, two HTTP sockets, LRU purge, port 8032, the existing 8,192-byte HTTP task, and the existing 4,608-byte service task.
- Render the page from flash-resident constant fragments plus one reusable stack scratch buffer no larger than 512 bytes. Stream bounded fragments with `httpd_resp_send_chunk()` and terminate the response explicitly.
- Form bodies are at most 32 bytes. Accept only exact ASCII forms; signal names must satisfy the existing storage-name validator. Reject duplicate, unknown, encoded, or malformed fields.
- Keep Host validation and same-origin checks for state-changing requests even without bearer authentication. Document that these checks do not prevent a malicious client already on the LAN.
- Require at least 28 KiB free internal heap and a largest internal block of at least 24 KiB at Web-ready.

## Gate 1: Prove HTTP Transport

### Minimal rewrite

1. Create an AFT checkpoint covering the current Web, auth/OTA integration, tests, CMake, configuration, and documentation before destructive edits.
2. Replace `components/web_ui/web_api.cpp`, its private header, and the existing frontend assets with a small server-rendered handler module.
3. Initially register only:
   - `GET /`, returning a deterministic minimal HTML page.
   - Temporary `GET /probe`, returning an 8 KiB deterministic flash-resident payload in one response.
4. Remove Web calls to `initialize_web_auth()`, `initialize_web_api()`, `register_web_api_handlers()`, `register_ota_http_handlers()`, stream lifecycle functions, and OTA server-status publication.
5. Remove runtime Web/auth/OTA dependencies from `components/web_ui/CMakeLists.txt`, `main/CMakeLists.txt`, and `main/main.cpp` where they are no longer used. Leave dormant component source available for later work unless it is proven unreferenced and deletion is separately reviewed.
6. Remove gzip asset generation and embedded frontend assets; the proof page is compiled as constant HTML.
7. Keep Wi-Fi initialization, Web start/stop ownership, HTTPD readiness gating, transactional rollback, heap diagnostics, and UART behavior unchanged.

### Static verification

- Run `git diff --check`, shell checks, clean host tests, the Unity build, and `tools/verify-production.sh`.
- Inspect compiler stack usage for the page/probe handlers and HTTP dispatcher.
- Inspect the image/map to confirm the absence of `web_events`, `web_operation`, SSE clients, operation records, auth state in the Web path, OTA HTTP handlers, event queues, and Web API wildcard dispatch.
- Add host contracts proving there is no JavaScript, remote asset, API wildcard, auth header, SSE/WebSocket string, OTA route, or generic UART command route.

### Hardware proof

After renewed explicit authorization, perform one serial flash without erasing NVS and maintain one controlled serial session:

1. Require the heap thresholds before sending requests.
2. Byte-compare 100 `/probe` responses with RF receive disabled.
3. Re-enable normal RF receive and byte-compare another 100 `/probe` responses.
4. Require zero timeout, truncation, `EAGAIN`, reboot, stack fault, disconnect, or declining steady heap.

If Gate 1 fails, stop the implementation. Preserve one diagnostic capture and report that HTTP transport is unreliable even with no Web runtime state. Do not change sockets, stacks, chunks, timeouts, Wi-Fi power mode, or buffer counts under this plan.

## Gate 2: Add the Barebone Controls

Proceed only after the user reviews the Gate 1 measurements and explicitly approves Gate 2.

### Handlers and rendering

- Remove `/probe`.
- Add typed snapshot reads for radio status, last frame, learning state, and compact catalog metadata. Do not use `bridge_control_get_status()` or construct an aggregate subsystem snapshot.
- Add the three exact form handlers for RX, learn, and cancel. Call existing typed services directly and synchronously; these operations enqueue or complete quickly and do not perform RF transmission.
- Return a small `400`, `409`, or `503` HTML error response on failure. Do not retain operation results or retry requests.
- Escape every dynamic HTML field and cap rendered catalog entries at the storage component's fixed maximum.
- Send constant header/style/footer fragments directly from flash and format each dynamic row through the 512-byte scratch buffer.

### Page design

- Use one compact operational page with a restrained header, a radio status band, last-frame details, learning form, and learned-signal table.
- Use normal HTML buttons, a checkbox/toggle for RX state, and semantic forms. No decorative sections, nested cards, animation, hidden feature scaffolding, or usage instructions.
- Include a strict CSP: no scripts, no external resources, no frames, no objects, same-origin forms only.
- Make desktop and mobile layouts fit without horizontal overflow or text overlap.

### Tests and documentation

- Replace `host_tests/test_web_assets.mjs` with a server-rendered HTML contract test covering CSP, exact form routes, no script/assets, bounded page fragments, escaped output helpers, and excluded feature strings.
- Add focused host/Unity tests for form parsing, HTML escaping, valid/invalid names, exact-field rejection, and status/error rendering.
- Update `README.md` with the unauthenticated trusted-LAN warning, exact three-action Web scope, disabled Web OTA, and UART-only feature list.
- Update production verification to assert port 8032, bounded stacks, and absence of the removed Web runtime facilities without changing Wi-Fi buffer defaults.

## Final Acceptance

With renewed approval, perform one final serial flash without erasing NVS:

1. Confirm Web-ready heap >=28 KiB and largest block >=24 KiB.
2. Load and byte-validate `/` 100 times with receive off and 100 times with receive on.
3. Exercise RX off/on, learning arm/cancel, last-frame rendering, and catalog rendering through browser forms.
4. Run malformed and cross-origin form requests and verify rejection without state changes.
5. Leave the page idle for 30 minutes; require stable heap/stack minima, no HTTPD warnings, and no reboot.
6. Verify UART Wi-Fi, RF transmit/replay, automation, signal deletion, diagnostics, and recovery paths remain available. Do not transmit RF during acceptance without separate approval.
7. Capture desktop and mobile screenshots and verify no blank page, overlap, clipping, or missing controls.

## Hard Stops

- Maximum two serial development flashes: transport proof and final barebone UI.
- A Gate 1 failure ends this Web attempt on the current transport baseline; no iterative tuning follows.
- A Gate 2 failure receives one diagnostic capture and assessment, not another implementation cycle.
- WebSockets, JavaScript, authentication, OTA, event streaming, and removed controls cannot be added during this plan.
- Authentication and OTA may return only as one separately reviewed feature after the barebone baseline passes sustained hardware acceptance.
