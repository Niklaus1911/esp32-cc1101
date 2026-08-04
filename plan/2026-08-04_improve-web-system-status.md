# Improve Web UI System Status

## Summary

Rebuild the System page as a read-only operational dashboard, retaining the existing OTA upload as its only mutation. Put live health before OTA, expose detailed CC1101, RF, Wi-Fi, automation, signals, uptime, reset, and heap data, and keep uncommon counters in expandable diagnostics.

The current Playwright baseline shows OTA occupying almost the entire 390 px first viewport and pushing five coarse runtime rows below the fold. Baseline screenshots are preserved under `/tmp/esp32-cc1101-playwright/system-before-{mobile,desktop}.png`.

## API And Firmware Changes

- Extend `/api/live` without removing or renaming existing fields:
  - `radio`: frequency, configured TX power, truncated captures, and a nested CC1101 object containing availability/error, part/version, numeric MARC state, half-dBm RSSI, carrier/clear-channel flags, resets, recoveries, and timeout counters.
  - `network`: state, driver/scan/OTA-lock flags, escaped active and saved SSIDs, saved-state flags, IPv4/netmask/gateway/DNS, RSSI, retries, disconnect reason, event drops, and initialization/persistence/last errors.
  - `learning` and `automation`: catalog, queue, stale/ambiguous, log-drop, and last-error diagnostics already present in their firmware status structs.
  - `system`: uptime milliseconds, named reset reason, and current/minimum/largest free internal 8-bit heap using `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`.
- Treat `cc1101_info_valid` and `cc1101_error` as authoritative chip health; the outer radio getter can remain `ESP_OK` when the chip-info read fails.
- Add bounded JSON-string escaping for printable SSIDs and a tested IPv4 formatter. Split JSON into sub-512-byte chunks and introduce no allocation, endpoint, queue, or persistent state.
- Keep `/api/v1/ota/status` as the OTA source. Do not add radio reset, Wi-Fi lifecycle controls, RF transmission, storage utilization, or NVS schema changes.

## System Page

- Order the page as: four-metric health summary, RF and CC1101, Wi-Fi, runtime and services, then firmware update.
- Summary metrics show RF service/RX state, CC1101 availability/MARC state, Wi-Fi state/RSSI, and free internal heap.
- Show key values directly in responsive two-column, unframed detail groups:
  - RF/CC1101: service state, RX/TX/maintenance, 433.920 MHz configuration, TX power, chip identity, MARC state with raw hex, RSSI, carrier sense, and clear-channel state.
  - Wi-Fi: state, active SSID, RSSI with quality label, IP, gateway, saved-network state, and OTA lock.
  - Runtime/services: uptime, reset reason, current/largest/minimum heap, learned-signal health, automation state, and subsystem errors.
- Put frame/drop/recovery/timeout counters, netmask/DNS/retries/disconnect details, saved SSID, and service queue/error counters in native expandable diagnostics. Preserve each disclosure's open state across live renders.
- Move the existing OTA band last, retaining upload progress, reboot recovery, security warning, partitions, versions, rollback, and error reporting.
- On connection loss, mark summary and band statuses as disconnected while retaining the last detailed snapshot with muted styling; restore current state on reconnect.
- Preserve the dependency-free assets and existing visual language. Allow `app.js` up to 32 KiB after measuring the final asset; keep HTML and CSS below 20 KiB each.

## Subagent Orchestration

- Backend agent owns `/api/live`, JSON/IP helpers, and focused C++ host tests.
- Layout agent owns System-page HTML and responsive CSS.
- Frontend agent owns JavaScript rendering, status formatters, and Web asset contracts.
- The primary agent publishes the exact JSON/DOM contract before parallel work, integrates all branches, updates README and the plan results, and owns Playwright, builds, flashing, and live validation.
- After the first implementation agents finish, reuse one slot for a read-only cross-review of API failure semantics, mobile layout, and resource bounds before final fixes. Agents do not commit.

## Verification

- Add host coverage for JSON escaping, truncation, IPv4 formatting, and required API/DOM/render contracts. Build the Unity image because Web component behavior changes.
- Run JavaScript syntax checks, Web asset contracts, host CTest, ESP-IDF MCP build, `git diff --check`, and `tools/verify-production.sh`.
- Use Playwright MCP with deterministic healthy, CC1101-unavailable, Wi-Fi-offline/retrying, nonzero-fault, low-heap, OTA-active, and disconnected fixtures. Verify one-second single-flight polling, fault transitions without stale green states, disclosure persistence, no mutations, and zero console/page errors.
- Validate 280, 390, 560, 820, and 1280 px widths with no overlap or horizontal overflow. Capture final 390 px mobile and 1280 px desktop screenshots and compare them with the existing baseline.
- Resolve and confirm the classic ESP32's stable CP2102 port, then use ESP-IDF Tools MCP `flash_project` for a normal wired flash. Do not erase NVS, clean the build, change the target, or open a serial monitor.
- After the automatic flash reset, wait for `192.0.2.17` to return. Confirm CC1101 part `0x00`, version `0x14`, configured frequency/power, live Wi-Fi addressing/RSSI, plausible heap values, healthy subsystem errors, polling stability, and live mobile/desktop screenshots.
- Require no reset-button press, RF signal, serial interaction, or other user action during implementation.

## Implementation Results

- Extended `/api/live` with the planned CC1101, RF, Wi-Fi, learning, automation,
  uptime/reset, and internal-heap diagnostics. SSIDs use bounded JSON escaping and
  IPv4 values use a tested bounded formatter; serialization remains allocation-free
  and split below the 512-byte scratch capacity.
- Rebuilt System as the planned health-first dashboard. Key operational values are
  visible, rare counters stay in native disclosures, OTA remains last, and no new
  mutation or recovery controls were added.
- Independent review caught and fixed three integration defects before deployment:
  hung live polls now mark cached health stale, Wi-Fi online state comes from the
  same copied snapshot as its details, and disconnected summary labels remain visibly
  bad while retained details are muted.
- Host CTest passed 3/3, JavaScript syntax and Web asset contracts passed, the ESP-IDF
  MCP production build passed, the Unity image compiled out of tree, and
  `tools/verify-production.sh` passed. The production image is 1,049,738 bytes with
  47% free in each OTA application slot; `app.js` is 32,372 bytes under the strict
  32 KiB limit.
- Deterministic Playwright fixtures covered healthy, CC1101 unavailable, Wi-Fi retry,
  service error, low heap, active OTA, disconnect, and reconnect states. At 280, 390,
  560, 820, and 1280 px there was no horizontal overflow, clipped status text, or band
  overlap. Disclosures remained open across polling, reconnect restored current
  health, and no mutation request occurred.
- Per the approved deployment plan, normal wired flashing succeeded through the
  stable CP2102 path without erasing NVS. Live validation at `192.0.2.17` confirmed
  CC1101 part `0x00`, version `0x14`, MARC RX, 433920000 Hz, 5 dBm, valid Wi-Fi
  addressing/RSSI, plausible heap telemetry, and `ESP_OK` subsystem errors.
- Final local and device screenshots are preserved under
  `/tmp/esp32-cc1101-playwright/system-after-{local,device}-{mobile,desktop}.png`.
