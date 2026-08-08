# Persistent Recent Decoded-Signal History

## Summary

- Before implementation, save this plan as `plan/2026-08-08_persistent-recent-decoded-signal-history.md`.
- Maintain the five newest accepted decoded receptions in NVS, newest first. Raw captures remain excluded.
- Keep every logical reception after existing hold-duplicate suppression.
- Assign persistent, nonzero 64-bit sequence IDs so rotated entries cannot be mistaken for newer signals.

## Key Changes

- Add a dedicated `rf_recent/history` versioned, CRC-protected NVS blob containing the next ID and up to five decoded entries. Each accepted decoded frame commits a new snapshot; the oldest entry is evicted at capacity.
- Expose `RfRecentSignal`, the fixed capacity, and bounded append/list/load/clear storage APIs. IDs remain stable across reboot and normal clearing; corrupt data fails closed until an explicit clear replaces it.
- Record history from the `rf_signals` worker, never the receive callback. Persistence failures must not suppress RX events, learning, automation, or RAM replay; expose count, revision, availability, error counters, and last error through signal status and `/api/live`.
- Add shared control operations for replaying, saving, and clearing recent entries. Replay uses the selected decoded payload and 1-20 repeats. Save copies it into create-only learned storage, refreshes the catalog, publishes the existing catalog-change event, and leaves history unchanged.
- Add UART commands:
  - `recent list`
  - `recent replay <id> [repeats]`
  - `recent save <id> <name>`
  - `recent clear`
- Print `id`, decimal and hexadecimal code, bits, protocol, pulse width, and current learned-match annotation. Omitted replay repeats use `CONFIG_RF_DEFAULT_TX_REPEATS`.
- Add `GET /api/recent`, returning revision and newest-first entries with IDs represented as decimal strings, decoded metadata, and current learned-match information.
- Add exact same-origin `POST /api/recent` forms: `action=replay&id=...&repeats=...`, `action=save&id=...&name=...`, and `action=clear`. Invalid input returns 400, rotated IDs return 404, and existing names return 409.
- Add a compact "Recent decoded signals" section above learned signals in the Signals tab, with refresh, confirmed clear, per-entry replay controls, and save-as-name controls. Refresh it on activation, revision changes, and mutations.
- Retain the existing 50-entry browser-only decoded/raw activity list, but label it clearly as tab-local. Update `README.md` with persistence, command/API behavior, stale-ID handling, and the distinction between RAM latest, browser activity, recent NVS history, and named learned signals.

## Test Plan

- Add host tests for record round trips, zero/one/five entries, duplicate receptions, ordering, eviction, persistent IDs, CRC/version/count corruption, invalid decoded data, and ID overflow handling.
- Add UART and Web-form parser tests covering exact grammar, 64-bit ID bounds, zero IDs, repeat bounds, names, extra fields, and embedded NUL input.
- Add Unity NVS tests for append/list/load, reboot-equivalent reload, stale-ID rejection, save copy semantics, clear with sequence preservation, corrupt-record recovery, and isolation from learned/rule/hardware namespaces.
- Extend Web asset contracts and validate a deterministic mock with Playwright on desktop and mobile: list refresh, correct ID targeting, stale-ID errors, save refresh, clear confirmation, no overflow, and no console/page errors.
- Run clean host tests, build both ESP32 and ESP32-S3 Unity images, perform focused production builds, then run `tools/verify-production.sh` once on the final unchanged firmware candidate.
- Flash and monitor only `esp32-devkit` through `tools/build-board.sh` using `/dev/serial/by-id/usb-EXAMPLE_CLASSIC-if00`. With a decoded RF source available, verify five-entry rotation and reboot persistence. Do not exercise RF replay on hardware without separate transmit authorization.

## Assumptions

- "Readable `code=`" means any codec-validated `RfEncoding::kDecoded` frame; raw frames are never added.
- Saving copies an entry and follows existing learned-name validation and no-overwrite rules.
- A selected ID that has rotated out is never redirected to another entry.
- Clearing is immediate over UART, confirmed in the Web UI, and does not reset a valid sequence counter.
- MQTT and Home Assistant exposure are outside this feature.
