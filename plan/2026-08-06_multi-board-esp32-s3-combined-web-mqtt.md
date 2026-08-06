# Three-Board ESP32 RF Bridge With Concurrent Web and MQTT

## Summary

Maintain one ESP-IDF 6.0.2 repository and shared component architecture, producing three board-specific firmware images. Classic ESP32 retains memory-safe, mutually exclusive Web/MQTT operation. Both ESP32-S3 boards gain reboot-selected `web`, `mqtt`, and simultaneous `both` modes using 8 MB PSRAM.

Preserve learned signals, automation rules, Wi-Fi/MQTT settings, project identity, hostnames, mDNS names, MQTT topics, Home Assistant entities, RF behavior, console commands, and OTA rollback semantics.

## Board And Build Contract

| Profile | Target | Flash / PSRAM | Console | Activity LED | Modes |
|---|---|---|---|---|---|
| `esp32-devkit` | ESP32 | 4 MB / none | UART0, GPIO1/3 | GPIO2 active-high | `web`, `mqtt` |
| `esp32s3-devkitc-n16r8` | ESP32-S3 | 16 MB / 8 MB Octal | UART0 GPIO43/44 through USB-UART | Disabled; reserve GPIO48 | `web`, `mqtt`, `both` |
| `xiao-esp32s3` | ESP32-S3 | 8 MB / 8 MB Octal | Native USB Serial/JTAG | GPIO21 active-low | `web`, `mqtt`, `both` |

| Profile | SCK | MISO | MOSI | CSN | GDO0 TX | GDO2 RX |
|---|---:|---:|---:|---:|---:|---:|
| ESP32 DevKit | 18 | 19 | 23 | 27 | 26 | 25 |
| S3 N16R8 DevKitC | 12 | 13 | 11 | 10 | 4 | 5 |
| XIAO ESP32-S3 | D8/7 | D9/8 | D10/9 | D3/4 | D1/2 | D0/1 |

- Add a `platform_board` component with `BoardProfile`, `ConsoleTransport`, immutable board metadata, GPIO ownership checks, combined-service capability, image compatibility identity, and internal/PSRAM memory snapshots.
- Keep GPIOs configurable, but apply profile defaults and validate them against target capabilities, duplicates, CC1101/LED overlap, exposed XIAO pins, and reserved pins. S3 rejects GPIO0/3/45/46, USB GPIO19/20, flash/PSRAM GPIO26-37, and profile-owned console/LED pins.
- Use shared `sdkconfig.defaults`, automatic `sdkconfig.defaults.esp32` or `.esp32s3`, then `boards/<profile>/sdkconfig.defaults`. S3 uses DIO flash at 40 MHz, Octal PSRAM at 80 MHz, required PSRAM detection, boot memory test, and no external task stacks.
- Add `tools/build-board.sh <profile> <build|size|flash|monitor> [--port <port>]`. Use isolated `build/<profile>` SDKCONFIG/build directories and `-DIDF_TARGET`; never invoke `set-target`. A supplied port is an assertion and must exactly equal the approved profile path, not an arbitrary override.
- Replace the target-contaminated generic locks with solver-generated `dependencies.lock.esp32` and `.esp32s3` files in production and Unity projects, selected through `DEPENDENCIES_LOCK`.
- Provide 4, 8, and 16 MB partition tables. Preserve NVS/OTA metadata offsets; use OTA slots `0x1e0000`, `0x3e0000`, and `0x7e0000` respectively, with the second slot at `0x200000`, `0x400000`, or `0x800000` and a final `0x20000` reserve.

## Implementation Changes

1. **Board-safe firmware identity and OTA**
   - Place a fixed 16-byte `RFBD` version-1 descriptor in ESP-IDF's `.rodata_custom_desc` section immediately after `esp_app_desc_t`. It contains magic, version, descriptor size, stable board ID `1/2/3`, target ID, flash MiB, partition-layout ID `1/2/3`, and zeroed reserved bytes.
   - Extend OTA prefix validation to require an exact board/layout match before maintenance or flash writing. Wrong S3 profile images and legacy images without the descriptor are rejected; project-name and chip checks remain.
   - Report the running profile, wiring, console, flash, PSRAM, and combined-mode capability through startup diagnostics and `board status`.

2. **Cross-target RF and console**
   - Retain `SPI3_HOST` and the existing CC1101 protocol/configuration.
   - Leave classic ESP32 RMT unchanged: 448-symbol non-DMA RX and 64-symbol TX allocation.
   - On ESP32-S3 enable RX and TX DMA with 48-symbol hardware blocks. Allocate both 448-symbol RX buffers at 64-byte alignment with internal, DMA-capable memory; keep TX payloads, snapshots, callbacks, atomics, queues, and ISR state internal. Release dynamic buffers only after channels are deleted.
   - Refactor only console transport setup/teardown: UART driver/VFS for the two DevKits and USB Serial/JTAG driver/VFS for XIAO. Preserve the same custom linenoise editor, history, completion, asynchronous redraw, masked Wi-Fi/MQTT prompts, output format, and command set.
   - Make LED validation target-aware; retain classic behavior, disable the N16R8 default, and drive XIAO GPIO21 active-low.

3. **Three service modes and durable migration**
   - Replace `NetworkServiceProfile` with runtime bitmask `NetworkServiceMask`: None=0, Web=1, MQTT=2, Both=3. Add independent MQTT retirement state Active=0, Retiring=1, Retired=2.
   - Upgrade `mqtt_cfg/service` to format v2 without changing its size: byte 4 is version 2, byte 5 is requested mask, byte 14 is retirement state, and endpoint, lengths, generation, credentials, reserved byte 15, and CRC retain their locations.
   - Decode v1 as: Web -> Web/Active, MQTT -> MQTT/Active, Retiring -> MQTT/Retiring, Retired -> Web/Retired. Do not rewrite merely because the device booted; every later successful mutation writes v2.
   - Add `service mode both`. On classic ESP32 it returns `ESP_ERR_NOT_SUPPORTED` before changing NVS. A transplanted persisted Both request falls back to Web for that boot without erasing the record.
   - Modes remain reboot-selected. Default is Web; MQTT and Both require valid credentials. Status reports requested/effective masks, generation/reboot state, independent Web and MQTT errors, and degraded/fallback state.
   - In Both, Web and MQTT initialize independently. A failure in one leaves the other running. MQTT-only allocation/activation failure retains the existing Web recovery fallback. RF, storage, automation, UART, and Wi-Fi remain independent of both frontends.
   - In Both, `mqtt forget` durably enters Both/Retiring, tombstones all retained entities with acknowledgements, then records Web/Retired, clears the ledger/credentials, stops and frees MQTT, and leaves Web running without reboot. MQTT-only retirement follows the same transaction but requires reboot to start Web. Power loss at any step resumes retirement safely.

4. **OTA coordination and memory ownership**
   - Add a synchronous MQTT maintenance handshake. During Web OTA in Both, keep the MQTT TCP client and keepalive alive, reject RF and persistent MQTT commands, drain queued commands, and defer discovery, state, birth, and catalog reconciliation before OTA writes begin.
   - A failed/aborted OTA releases maintenance and forces full MQTT reconciliation. A successful OTA proceeds directly to the existing controlled reboot. Retirement and OTA cannot begin concurrently.
   - Enable `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, retain at least 64 KiB for internal-only/DMA allocations, and keep the normal 16 KiB internal-allocation threshold.
   - Split MQTT into a small internal control context and a cold working set. Allocate ledgers, merge/reduction scratch, rule snapshots, temporary catalog arrays, advertised-record buffers, and discovery payload scratch explicitly from PSRAM on S3; use internal memory on classic. Never silently fall back to internal memory for the S3 cold set.
   - Keep credentials and their scrubbed record buffers, task stacks/TCBs, queues, synchronization, MQTT callback state, OTA buffers, and RF/RMT state internal.
   - Add `memory status` and extend `/api/live` with board, requested/effective services, and internal/PSRAM total/free/minimum/largest-block values. Preserve existing `system.heap_*` fields as internal-memory compatibility aliases. Update the System Web view without adding runtime mode controls.

5. **Documentation and operational workflow**
   - Update the authoritative README with the support matrix, all three wiring tables, USB connector guidance, Web/MQTT/Both behavior, retirement semantics, PSRAM expectations, and per-profile build/flash/monitor commands.
   - Update development tooling documentation for isolated board builds, target-specific locks, three-profile verification, VS Code terminal usage, and XIAO USB re-enumeration.
   - Document that first installation on either S3 board is wired, OTA images are profile-specific, legacy downgrades require wired flashing, and NVS must never be erased during migration.

## Verification And Acceptance

- Extend host tests for all board/pin policies, v1-to-v2 mappings, v2 CRC/reserved-field validation, service-mode capability, startup failure matrices, retirement power-loss transitions, OTA descriptor compatibility, and allocator-failure cleanup.
- Compile Unity for ESP32 and ESP32-S3 configurations, while the three production profiles ensure both S3 console branches compile. Do not run on-device Unity tests without separate authorization.
- Make `tools/verify-production.sh` build all three profiles cleanly, run size reports, inspect target/flash headers, validate partition offsets and the custom board descriptor, confirm PSRAM/console/pin defaults, and enforce 25% free space in every OTA slot.
- Run Playwright against the deterministic mock and then authorized hardware: desktop/mobile screenshots, overflow, console/page errors, polling, same-origin mutation checks, reconnect handling, and correct new service/memory diagnostics.
- Hardware-test the available classic ESP32's console, RF RX/TX, learning, replay, rule persistence, Wi-Fi persistence, correct LED behavior, mode changes, OTA rollback, and reboot recovery.
- Hardware-test the available N16R8's UART console, PSRAM, Both mode, persistence, OTA, reconnect recovery, and soak behavior under `plan/2026-08-06_n16r8-hardware-validation.md`. Its RF acceptance remains deferred because no CC1101 is connected.
- Defer XIAO hardware acceptance until that board and a separate persistent by-id path are available.
- S3 Both acceptance requires at least 48 KiB free internal RAM, a 32 KiB largest internal block, 1 KiB minimum stack margin on every project-owned task, and no monotonic heap loss above 1 KiB over a one-hour reconnect/failed-OTA soak.

## Assumptions

- Support covers the named classic DevKit/WROOM, the identified dual-USB 44-pin N16R8 DevKitC-compatible board, and standard 8 MB flash/8 MB PSRAM XIAO ESP32-S3. XIAO Sense, Plus, and arbitrary S3 boards need additional profiles.
- These are three board-specific images from one project, not a universal binary and not separate forks.
- MQTT remains plaintext LAN MQTT 3.1.1; TLS and external task stacks remain out of scope.
- Hostname derivation, mDNS identity, project name, MQTT client/topic identity, learned-signal and automation storage formats, and Home Assistant entity IDs remain compatible.

## Current Hardware Validation Contract

- The classic `esp32-devkit` path is `/dev/serial/by-id/usb-EXAMPLE_CLASSIC-if00`.
- The N16R8 path is `/dev/serial/by-id/usb-EXAMPLE_N16R8-if00`; its connected board currently has no CC1101.
- Before hardware access, verify the selected profile's exact symlink exists, resolves to a character device, and its target, flash size, profile define, and board descriptor all match.
- Never fall back to `/dev/ttyUSB*`, `/dev/ttyACM*`, another by-id path, or port auto-detection. If the selected profile's approved path is absent, stop without searching for another port.
- Do not flash or monitor the XIAO profile until its board exists and a separate persistent by-id path has been explicitly approved.
