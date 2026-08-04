# Final Plan: Barebone Server-Rendered Web UI

## Outcome

Replace the current Web implementation rather than simplify it incrementally. The new Web UI uses classic HTTP only:

- No WebSockets.
- No JavaScript or JSON API.
- No authentication or token management.
- No SSE, polling, event history, console mirror, operation tracking, background workers, or retained Web state.
- No Web OTA while authentication is absent.

One server-rendered page displays radio/RX status, the last accepted frame, learning state, and compact learned-signal metadata. Three strict HTML forms provide RX enable/disable, learning arm, and learning cancel. Every successful post redirects to `/`.

UART remains the interface for Wi-Fi management, radio start/reset, signal deletion, replay/transmit, automation rules, diagnostics, console settings, and firmware recovery/flashing.

## Fixed Resource Contract

- HTTPD is the only Web request task; `web_service` only owns start/stop on network transitions.
- The Web component allocates no task, queue, semaphore, event sink, client table, operation record, auth state, or response-sized heap buffer.
- Use exact URI handlers, two HTTP sockets, port 8032, the existing 8,192-byte HTTP task, and the existing 4,608-byte service task.
- Render from flash-resident HTML/CSS fragments plus one reusable stack scratch buffer no larger than 512 bytes.
- Cap form bodies at 32 bytes and accept only exact ASCII fields. Reuse the existing signal-name validator.
- Retain Host and same-origin checks for POST requests. Document that the UI is still open to clients already on the trusted LAN.
- Require at Web-ready:
  - At least 28 KiB free internal heap.
  - At least 24 KiB largest internal block.
  - At least 2 KiB free stack margin on measured HTTP call chains.

## Phase 1: Destructive Web Reset

1. Create an AFT checkpoint for all current Web/auth/OTA integration, tests, CMake, defaults, verification scripts, and documentation.
2. Replace `components/web_ui/web_api.cpp` and its private header with a small server-rendered handler module.
3. Remove the current HTML/CSS/JavaScript assets, gzip generation, asset embedding, and frontend build contracts.
4. Remove Web API initialization, wildcard dispatch, auth verification, OTA handler registration, stream lifecycle, broker sinks, event queues, event worker, operation queue/worker, request IDs, fingerprints, retained operations, aggregate status, and all corresponding metrics.
5. Simplify `components/web_ui/web_ui.cpp` while preserving:
   - Wi-Fi-online startup and connectivity-loss shutdown.
   - HTTPD-first allocation.
   - Readiness gating.
   - Transactional rollback.
   - Heap and stack diagnostics.
6. Remove unused Web/auth/OTA runtime dependencies from `components/web_ui/CMakeLists.txt`, `main/CMakeLists.txt`, and `main/main.cpp`. Leave dormant source components available for later separately reviewed work unless deletion is clearly required.
7. Keep Wi-Fi buffer defaults, station-only mode, RF ownership, UART behavior, NVS, partitions, and CPU settings unchanged.

## Gate 1: Transport Proof

Initially register only:

- `GET /`: deterministic minimal HTML.
- Temporary `GET /probe`: deterministic 8 KiB flash-resident payload sent without runtime allocation.

Run before hardware:

1. `git diff --check`, shell syntax, clean host tests, Unity compilation, and `tools/verify-production.sh`.
2. Compiler stack-usage inspection for HTTPD dispatch and both handlers.
3. Map/symbol inspection proving the old Web event/operation/auth/OTA runtime is absent.
4. Host contracts proving there is no script, remote asset, wildcard API, auth header, SSE/WebSocket string, OTA route, or generic UART command route.

Hardware proof:

1. Discover/confirm the single CP2102 port with `tio --list`.
2. Flash the production image without erasing NVS.
3. Issue an explicit normal run reset and keep one controlled serial monitor session open.
4. Confirm the heap and stack thresholds before HTTP testing.
5. Use UART to disable RF receive; byte-compare 100 `/probe` responses.
6. Re-enable RF receive; byte-compare another 100 `/probe` responses under normal RF activity.
7. Require zero timeout, truncation, `EAGAIN`, disconnect, reboot, or declining heap/stack minimum.

If Gate 1 fails, stop immediately. Capture one diagnostic log and report that HTTP transport remains unreliable without Web runtime pressure. Do not alter sockets, stacks, chunks, timeouts, Wi-Fi power mode, or buffer counts.

If Gate 1 passes, proceed directly to Phase 2 under the same authorization.

## Phase 2: Minimal Operational Page

1. Remove `/probe`.
2. Implement `GET /` using direct typed snapshots only:
   - Radio availability and RX state.
   - Last accepted frame summary.
   - Learning armed/pending state.
   - Learned-signal names and encoding types, capped at the storage maximum.
3. Do not use `bridge_control_get_status()` or collect unrelated subsystem state.
4. Implement exact form routes:
   - `POST /rx` with `enabled=0|1`.
   - `POST /learn` with one validated name.
   - `POST /learn/cancel` with an empty body.
5. Execute these short typed operations synchronously. Return `303` on success and small bounded HTML errors on malformed, conflicting, or unavailable requests.
6. Reject unknown/duplicate fields, percent-encoded names, oversized bodies, invalid methods, invalid Host, and cross-origin POSTs before changing state.
7. Escape all rendered dynamic fields. Format dynamic rows through the 512-byte scratch buffer and send constant fragments directly from flash.
8. Build one compact responsive page with an inline CSP-protected stylesheet, status band, last-frame section, RX control, learning form, and learned-signal table. Include no hidden scaffolding for removed features.

## Tests and Documentation

- Replace `host_tests/test_web_assets.mjs` with server-rendered contracts for CSP, exact routes/forms, no script/assets, no auth/OTA/WebSocket/SSE, bounded fragments, and excluded feature strings.
- Add focused host/Unity tests for form parsing, HTML escaping, name validation, exact-field rejection, status rendering, and error rendering.
- Update `README.md` with:
  - The exact three-action Web scope.
  - The unauthenticated trusted-LAN warning.
  - Disabled Web OTA.
  - UART-only administration and recovery features.
- Update production verification to assert port/stacks and absence of removed runtime facilities without changing Wi-Fi defaults.
- Re-run every Gate 1 static/compiler/build check before the final flash.

## Final Hardware Acceptance

1. Flash the final production image without erasing NVS.
2. Use an explicit run reset and one bounded serial monitor session.
3. Confirm free heap >=28 KiB, largest block >=24 KiB, and measured stack margins.
4. Load and validate `/` 100 times with receive off and 100 times with receive on.
5. Exercise RX off/on, learning arm/cancel, last-frame rendering, and catalog rendering through browser forms.
6. Verify malformed and cross-origin posts cannot change state.
7. Leave the page idle for 30 minutes and require stable heap/stack minima, no HTTPD warning, and no reboot.
8. Capture desktop and mobile screenshots and verify no blank page, overlap, clipping, or missing controls.
9. Verify UART Wi-Fi, RF, automation, diagnostics, and recovery commands remain available. Do not transmit RF during acceptance.

## Hardware Authorization

For this plan, the user authorizes:

- `tio --list` to confirm the single serial device.
- At most two serial production flashes: Gate 1 and final acceptance.
- Explicit normal run resets after flashing.
- Bounded serial monitoring and capture.
- UART `receive off`, `receive on`, and read-only status commands needed by acceptance.

The authorization excludes:

- Flash/NVS erase.
- Partition changes.
- `menuconfig`, `set-target`, or `reconfigure`.
- Editing generated `sdkconfig` outside approved defaults.
- On-device Unity execution.
- RF transmission.
- Commit, push, or destructive Git operations.

Authorization ends when final acceptance passes or either hard stop is reached.

## Hard Stops

- Maximum two serial development flashes.
- Gate 1 failure ends the attempt without another patch cycle.
- Final acceptance failure receives one diagnostic capture and assessment, not another implementation iteration.
- WebSockets, JavaScript, authentication, OTA, streaming, polling, and excluded controls cannot be introduced during this plan.
- Future features require separate proposals and one-at-a-time hardware qualification against the proven baseline.
