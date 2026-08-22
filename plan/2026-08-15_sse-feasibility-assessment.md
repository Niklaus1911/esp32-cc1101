# SSE Feasibility Assessment

## Summary

SSE is feasible, but it should supplement the existing `/api/live` polling rather than replace it. The snapshot remains authoritative; SSE delivers low-latency change notifications and triggers a resync when events are missed.

Record the assessment in `docs/sse-feasibility.md`. This deliverable will not change firmware, browser assets, public behavior, or hardware.

## Assessment Content

- Document the current constraints: two HTTP sockets, two broker sink slots, bounded polling, Wi-Fi-driven server lifecycle, and ESP-IDF's asynchronous request APIs.
- Recommend one active SSE client per device, with polling fallback for additional tabs or unsupported/failing streams.
- Define a future `GET /api/events` contract:
  - `text/event-stream`, no-cache, keep-alive response.
  - Initial `hello` event and periodic comment heartbeats.
  - `id` values from `BridgeEvent.sequence`.
  - Bounded typed envelopes for RX, learning, TX, automation, network, OTA, catalog, and hardware-switch events.
  - Queue overflow or reconnect produces `resync`; the browser fetches `/api/live`.
  - No full snapshot or raw internal union is emitted for every event, and no replay guarantee is made from `Last-Event-ID`.
- Describe the required implementation shape:
  - One shared Web event hub registered as a broker sink; the sink only copies bounded data into a queue and performs no I/O.
  - An asynchronous HTTP request/worker using `httpd_req_async_handler_begin` and `httpd_req_async_handler_complete`.
  - A single client slot with deterministic rejection of a second stream.
  - A measured increase in `max_open_sockets` (at least one socket reserved for ordinary API/static/OTA traffic), with `lru_purge_enable` behavior explicitly tested.
  - Clean shutdown on Wi-Fi loss, server restart, OTA, client disconnect, and queue pressure.
  - Browser `EventSource` reconnect/backoff, visibility handling, action coordination, and polling fallback.
- Explain broker capacity impact: UART and MQTT already consume the two existing sink slots, so production support for Web+MQTT requires either a third slot or a broker-level multiplexer.

## Go/No-Go Gates

The note will define implementation gates for a later change:

- `/api/live`, static assets, mutations, and OTA remain responsive while the stream is open.
- Broker and per-client queues are bounded; drops are visible through `resync`.
- Reconnect, Wi-Fi loss, server restart, and OTA reboot recover without stale UI state.
- Host tests cover event serialization, sequence/gap handling, queue overflow, and single-client admission.
- Playwright mock tests cover desktop/mobile layout, connection loss, reconnect, fallback polling, and console/page errors.
- All supported Web profiles retain their existing internal-heap margin; the classic ESP32 is feature-gated if its measured budget is insufficient.
- A future implementation must pass the applicable production verifier before flashing.

## Assumptions

- This turn is feasibility-only; no production implementation, build, Playwright run, or flash is included.
- The trusted-LAN, unauthenticated Web UI security model remains unchanged; the new GET route must still validate the device `Host` and reject request bodies.
- The existing polling path remains permanently available as the recovery and compatibility path.
