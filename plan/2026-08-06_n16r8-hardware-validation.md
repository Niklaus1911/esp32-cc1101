# ESP32-S3 N16R8 Flash and Hardware Validation

## Summary

Prepare the repository for the newly available N16R8 board, flash its board-specific image only through `/dev/serial/by-id/usb-EXAMPLE_N16R8-if00`, then stop completely so the user can provision Wi-Fi and MQTT. After the user exits the monitor, resume with Web+MQTT, OTA, persistence, browser, mDNS, memory, reconnect, and one-hour soak validation.

The CC1101 is intentionally absent. Radio startup must fail cleanly and nonfatally; RF reception, transmission, learning, replay, and live rule triggering cannot pass in this run.

## Tooling Changes

- Save this plan as `plan/2026-08-06_n16r8-hardware-validation.md` before making other changes.
- Add the exact N16R8 by-id path to `tools/build-board.sh`; retain the classic ESP32 path and keep XIAO hardware access blocked.
- Update README, development-tooling documentation, and the original multi-board plan's hardware contract to identify N16R8 as available and hardware-authorized.
- Keep `/dev/ttyACM0`, port auto-detection, alternate by-id paths, NVS erase, `set-target`, `menuconfig`, `reconfigure`, and `clean` forbidden.
- Make `tools/push-ota.sh` recognize all three board images from chip, flash size, and the embedded `RFBD` descriptor. Preserve its current CLI while applying the correct per-profile OTA-slot limit.
- Extend the OTA script tests for classic, XIAO, and N16R8 images, descriptor/metadata mismatches, oversized images, transport errors, and malformed responses.
- Do not commit or push; preserve the existing uncommitted multi-board work.

## Phase 1: Verify, Flash, and Stop

1. Inspect status/diffs, confirm the exact symlink still resolves to the `1a86:55d3` device with serial `EXAMPLE_N16R8_SERIAL`, resolves to a character device, and is not held by another monitor.
2. Run host tests, ESP32 and ESP32-S3 Unity compile builds, `tools/verify-production.sh`, and `git diff --check`.
3. Build `esp32s3-devkitc-n16r8` in its isolated profile directory and assert:
   - ESP32-S3 target, 16 MB flash, 8 MB Octal PSRAM configuration.
   - UART0 console on GPIO43/44 and N16R8 profile selection.
   - N16R8 CC1101 pins, disabled activity LED, two `0x7e0000` OTA slots, and exact `RFBD` board/layout descriptor.
4. Flash through the approved by-id path using the board helper. Do not erase NVS and do not open any serial monitor afterward.
5. End the execution turn immediately after a successful flash.

During the user-owned monitor window, the user will:

```text
wifi connect <ssid>
# Enter the Wi-Fi password and wait for WIFI CONNECTED.
mqtt configure 192.0.2.35 example_broker_user
# Enter the MQTT password at the masked prompt.
service mode both
service status
```

The user must then exit the monitor without rebooting and report that provisioning is complete. Passwords must not be placed in chat or logs.

## Phase 2: Hardware Validation

- Revalidate the exact port, attach a logged monitor, and capture the still-running Web-only baseline before reboot: `status`, `board status`, `memory status`, `wifi status`, `service status`, `mqtt status`, `hostname status`, `ota status`, `learn list`, `rule list`, and `radio info`.
- Perform one controlled reboot. Confirm clean ESP32-S3 boot, 16 MB flash, usable 8 MB PSRAM, retained Wi-Fi/MQTT configuration, requested/effective `both`, and independently successful Web and MQTT startup without fallback.
- Confirm the absent CC1101 produces three bounded startup failures, no panic or reboot loop, explicit unavailable radio diagnostics, and a bounded failure from one `radio start` retry.
- Validate Web by numeric IP and the effective `.local` hostname: `/`, `/api/live`, OTA status, polling/reconnect behavior, headers, same-origin rejection, desktop/mobile screenshots, overflow, clipping, page errors, and console errors.
- Validate mDNS hostname resolution and `_http._tcp`/`_rfbridge._tcp` DNS-SD records where the LAN client supports multicast discovery.
- Record the current automation enabled/log setting, change one setting through an authorized same-origin Web request, reboot, verify it through Web/UART/MQTT state, restore the original value, and verify restoration persists.
- Validate device-side MQTT connection, subscription, discovery reconciliation, retained-state publication counters, Web-triggered automation-state propagation, outbox use, reconnect counters, and MQTT/worker stack margins. Broker-retained payload inspection and Home Assistant UI acceptance remain unclaimed because broker credentials stay private.
- Upload the verified N16R8 image through the corrected OTA tool using the effective `.local` hostname, never a persisted DHCP address. Confirm alternate-slot boot, image confirmation without rollback, persisted Wi-Fi/MQTT/automation state, and simultaneous Web+MQTT recovery.
- Exercise one wrong-profile prefix rejection and one deliberately truncated same-profile OTA failure. Confirm no reboot, unchanged running partition, released maintenance state, Web recovery, MQTT reconciliation, and continued command availability.
- Run a one-hour Both-mode soak with periodic `/api/live` samples and controlled Wi-Fi reconnects at 15, 30, and 45 minutes. After each reconnect, wait for DHCP, Web, mDNS, MQTT subscription, and discovery reconciliation before taking an equivalent steady-state memory sample.
- Finish with another reboot and final status capture. Leave service mode `both`, restore all temporary automation settings, and rerun host tests, both Unity builds, production verification, diff checking, and status inspection.

## Acceptance and Limits

- Steady-state Both mode must retain at least 48 KiB free internal RAM, a 32 KiB largest internal block, and at least 1 KiB reported MQTT and worker stack margins.
- Equivalent settled samples must show no monotonic internal-memory loss above 1 KiB over the soak; PSRAM must remain healthy and Web/MQTT must survive every reconnect and failed OTA.
- There must be no assertion, watchdog, stack overflow, panic, spontaneous reset, service fallback, or persistent OTA-maintenance state.
- Learned-signal buttons, RF event telemetry, live automation triggering, CC1101 recovery after wiring, RF timing/range, and RF persistence remain deferred until the CC1101 is connected.
- The existing firmware does not expose every project-owned task's stack high-water mark. Unreported task margins cannot be claimed; absence of stack failures and the available Web/MQTT diagnostics will be recorded explicitly as the remaining observability limit.
