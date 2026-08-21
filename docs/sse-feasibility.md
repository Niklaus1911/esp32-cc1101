# Server-Sent Events for the Web UI

## Outcome

Server-Sent Events (SSE) are implemented as a bounded notification channel alongside the existing `/api/live` snapshot. SSE does not become the Web UI's state store or replace polling outright.

The implementation permits one active SSE client per device. The browser keeps `/api/live` as the authoritative resync path and falls back to visibility-aware polling whenever SSE is unavailable, rejected, or disconnected.

This document records the constraints and acceptance criteria used for the implementation.

## Pre-Implementation Constraints

The existing design creates several constraints that make a simple infinite response unsafe:

| Area | Current behavior | SSE consequence |
| --- | --- | --- |
| Live UI | `app.js` polls `/api/live` with timeout, backoff, cancellation, and visibility checks | SSE can reduce latency, but must preserve the current snapshot and fallback behavior |
| HTTP server | ESP-IDF `esp_http_server` originally used two sockets with LRU purge enabled | A persistent stream competes with normal API, static asset, and OTA traffic; the implementation raises the budget to three sockets |
| HTTP handler | URI handlers run in the server context | A loop around `httpd_resp_send_chunk()` would block request processing; the stream must use an asynchronous request and worker |
| Event broker | The original broker had two sink slots, a bounded queue, and a zero-wait sink contract | UART, MQTT, and Web now share three fixed sink slots; no resource use scales with browser tabs |
| Lifecycle | HTTP starts/stops with Wi-Fi and publishes Web lifecycle events | Stream tasks must be cancelled and joined before server shutdown or OTA restart |
| Security | Trusted-LAN UI validates the device `Host` for GETs and rejects bodies | The stream must use the same host validation and empty-body checks; no new authentication model is implied |

ESP-IDF provides `httpd_req_async_handler_begin()` / `httpd_req_async_handler_complete()` for long-lived requests and `httpd_resp_send_chunk()` for chunked responses. The official async-handler example also recommends keeping at least one socket available for ordinary requests. References:

- [ESP-IDF HTTP server API](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/protocols/esp_http_server.html)
- [ESP-IDF asynchronous request handlers example](https://github.com/espressif/esp-idf/tree/v6.0.2/examples/protocols/http_server/async_handlers)

## Implemented Contract

`GET /api/events` sends:

```text
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive
```

Each event uses the broker sequence as its SSE id and carries a bounded typed envelope:

```text
id: 481
event: rx
data: {"sequence":481,"encoding":"decoded","code":"0xA88142","bits":24,"protocol":1,"pulse_us":386,"repeats":4,"match":"unique","match_count":1,"match_name":"gate"}

```

The exact data fields remain limited to values needed by the UI: the broker sequence, results, bounded names, counters, and small event-specific summaries. Raw `BridgeEvent` unions and full `/api/live` snapshots are not exposed on every event.

The stream protocol includes:

- A `hello` event after connection, containing the current broker sequence and `snapshot_required: true`. The browser fetches `/api/live` before treating the stream as current.
- Typed events for RX, learning lifecycle, TX, automation, network, OTA, signal-catalog changes, and hardware-switch changes.
- Comment heartbeats at a fixed interval to detect dead connections without creating UI work.
- A `resync` event when the bounded queue overflows or an event sequence gap is observed. The browser immediately fetches `/api/live` and resumes normal processing.
- No replay guarantee for `Last-Event-ID`; reconnect always performs a snapshot resync. The id is still useful for diagnostics and gap detection.

## Firmware Shape

The broker sink must remain short and non-blocking. A single Web event hub should register one sink, copy only bounded event data into a fixed-capacity queue, and signal a stream worker. It must never call the HTTP server or perform socket I/O from the broker dispatcher.

The HTTP handler validates the request, reserves the one client slot, detaches the request with `httpd_req_async_handler_begin()`, and hands ownership to a worker. The worker sends the initial event, heartbeats, and queued events with chunked responses. On send failure, client disconnect, Wi-Fi loss, or server stop it calls `httpd_req_async_handler_complete()` exactly once and releases the slot.

The implementation uses `max_open_sockets = 3`, reserving capacity for the stream and ordinary requests while retaining LRU purging. A second stream receives `409` and falls back to snapshots instead of consuming another persistent socket.

The broker allows three sinks for UART, MQTT, and Web. The Web event hub owns one sink regardless of browser count; a per-browser sink remains prohibited.

## Browser Behavior

The browser creates one `EventSource` only while the page is visible and no conflicting action or OTA transition is active. On `open`, it performs a snapshot fetch. On a typed event, it schedules a single snapshot refresh through the existing polling cancellation and busy-state rules rather than mutating state from partial event data.

The browser closes the stream when hidden, reconnects with bounded exponential backoff after errors, and continues the existing polling loop whenever the stream cannot be opened. A second tab that receives the single-client rejection remains usable through polling. No RF transmit, deletion, hardware switching, or OTA request is initiated by the stream itself.

## Risks and Go/No-Go Gates

SSE is a good fit for event latency, but it is not a free optimization. The implementation is a go only if all of the following hold:

- `/api/live`, static assets, mutations, and OTA remain responsive with the stream held open.
- Event and client queues remain bounded, and overflow produces an observable `resync` rather than silent stale state.
- Wi-Fi loss, HTTP restart, OTA reboot, client disconnect, and browser reconnect leave no task, request, or sink leak.
- Host tests cover envelope serialization, every event family, sequence gaps, queue overflow, and second-client admission.
- Playwright mock validation covers desktop and mobile views, stream loss, reconnect, polling fallback, page/console errors, and no horizontal overflow.
- All Web-capable board profiles retain their existing internal-heap margin. If the classic ESP32 cannot meet the measured budget, SSE must be feature-gated there while remaining available on S3 profiles.
- A production implementation passes the affected profile build and the required clean production verifier before any device flash.

Polling remains available as the compatibility and recovery transport when any gate prevents SSE from being authoritative enough to drive a refresh.
