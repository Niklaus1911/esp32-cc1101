# Selectable CC1101 and Generic OOK RF Backends

## Scope

Add support for direct-data ASK/OOK 433 MHz modules such as SRX882/STX882 alongside the existing CC1101 backend. The user selects exactly one backend at a time through Web, MQTT Home Assistant, or the serial console. Both modules may remain wired, but their GPIOs are separate and the inactive backend is held idle.

## Decisions

- Support all board profiles: `esp32-devkit`, `esp32s3-devkitc-n16r8`, `xiao-esp32s3`, and `esp32s3-supermini-fh4r2`.
- Persist a versioned, CRC-protected backend selection record in NVS; default to CC1101 when absent or invalid.
- Switch live and transactionally without reboot. Pause automation, invalidate queued pre-switch frames, stop and idle the old backend, bind and initialize the target backend, restore RX, and persist only after activation succeeds. Roll back on failure.
- Generic modules expose only direct TX DATA and RX DATA lines. Generic pins are fixed by board profile and are not runtime-configurable.
- Web uses a two-option selector with an explicit Apply action. MQTT uses a Home Assistant select entity and exact non-retained QoS 0 commands.
- Use 3.3 V direct module wiring in the documentation, with warnings for variants that require level shifting.

## Generic GPIO map

| Profile | TX DATA | RX DATA |
| --- | ---: | ---: |
| `esp32-devkit` | GPIO32 | GPIO33 |
| `esp32s3-devkitc-n16r8` | GPIO6 | GPIO7 |
| `xiao-esp32s3` | D4 / GPIO5 | D5 / GPIO6 |
| `esp32s3-supermini-fh4r2` | GPIO6 | GPIO7 |

## Implementation

1. Add an `RfHardware` enum and board-policy generic TX/RX mappings. Validate mappings against reserved board pins, CC1101 pins, and the activity LED.
2. Add the `RFHW` NVS record and storage helpers with version, magic, and CRC validation.
3. Refactor `rf_ook` behind a backend adapter. Keep CC1101 SPI/config/status behavior; run generic direct GPIO through the existing RMT timing, codec, and replay path. Keep generic TX low while inactive and report CC1101-only diagnostics as unavailable.
4. Implement the live switch transaction and shared bridge event for success, failure, and rollback. Support changing backend while RF is stopped.
5. Add serial commands `radio hardware`, `radio hardware cc1101`, and `radio hardware generic`.
6. Add `POST /api/radio/hardware` with exact `hardware=cc1101|generic` parsing and existing Host/Origin validation. Expose active backend and switch diagnostics in `/api/live`.
7. Add MQTT Home Assistant select discovery, state, and command topic `rfbridge/<12hex>/radio/hardware/set`, including tombstone handling.
8. Generalize user-facing RF labels while preserving project identity, OTA descriptors, hostnames, and MQTT topic compatibility.
9. Update `README.md` with module wiring, power/antenna and CS guidance, limitations, and all GPIO tables.

## Verification

- Add focused host tests for GPIO policy, `RFHW` format/CRC, Web parsing, MQTT topic/discovery/state, and switch policy.
- Build the ESP32 and ESP32-S3 Unity images; use compile-only validation unless hardware access is explicitly authorized.
- Run Web asset/source contract tests and deterministic Playwright desktop/mobile checks for apply success, failure/rollback, polling/reconnect, maintenance state, overflow, and page/console errors.
- Run `tools/verify-production.sh` once on the final unchanged candidate because firmware, Kconfig, board defaults, and Web assets change.
- Do not flash, monitor serial, OTA, or run on-device tests without explicit approval and a confirmed board path.
