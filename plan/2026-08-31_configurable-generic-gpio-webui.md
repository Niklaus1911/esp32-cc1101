# Configurable Generic ASK/OOK GPIOs

## Summary

Add board-aware TX/RX GPIO selectors to the System settings page for all supported profiles. Values are validated server-side, stored in NVS with the board profile ID, and applied after an automatic delayed reboot. Existing active GPIOs remain unchanged until reboot.

Generic pins may overlap any CC1101 pin because the modules are not connected simultaneously. The UI will show a permanent caution and a stronger warning when the selected pair overlaps CC1101 wiring.

## Implementation Changes

- Extend `platform_board` with role-specific GPIO option enumeration. Continue rejecting reserved, flash/PSRAM, strapping, console, unexposed, wrong-direction, activity-LED, and duplicate TX/RX pins; allow generic/CC1101 overlap for every profile.
- Add a versioned, CRC-protected `rf_hw/generic_gpio` NVS record containing profile ID, TX GPIO, and RX GPIO. Preserve the existing RF-backend selection record.
- Add `rf_ook` runtime configuration APIs and state:
  - load defaults and persisted values after RF storage initialization;
  - fall back to profile defaults on missing, corrupt, profile-mismatched, or invalid records without blocking boot;
  - expose active, saved, default, pending, overlap, and load-error state;
  - save new values without changing the active map, allowing reboot to apply them.
- Update RF GPIO ownership and cleanup for arbitrary generic/CC1101 aliases. Pass the runtime map into activity-LED validation and update console/live diagnostics to report runtime GPIOs.
- Add `POST /api/radio/generic-gpio` with exact `tx_gpio`/`rx_gpio` fields, existing same-origin checks, and board-policy validation. Return `200` for a no-op and `202` with `rebooting:true` after a changed configuration is saved.
- Reuse the OTA reboot task through a separate external-reboot flag so GPIO reboots do not alter OTA status. Reject concurrent OTA/reboot requests and increase `max_uri_handlers` from 23 to 24.
- Extend `board.generic` in `/api/live` with active/default/saved/pending values, option arrays, overlap state, and configuration error while preserving the existing `tx`/`rx` fields.
- Add always-visible TX/RX selects and a “Save and reboot” control in the System RF hardware section. Disable duplicate choices, show warnings, preserve error feedback, and handle reboot disconnect/reconnect without a false failure.

## Tests and Validation

- Add host tests for option enumeration, overlap policy, duplicate/unsafe rejection, storage encoding/CRC/profile mismatch, fallback behavior, and exact form parsing.
- Update Unity and source-contract tests for runtime loading, ownership cleanup, API/live fields, handler budget, reboot ordering, and revised N16R8 expectations.
- Run native host tests, the affected Unity image build, and one final `tools/verify-production.sh` pass for the firmware and embedded assets.
- Use Playwright MCP with a deterministic localhost mock to verify desktop/mobile layout, option population, duplicate prevention, warnings, busy/error states, exact POST body and headers, simulated reboot recovery, console/page errors, and horizontal overflow. Intercept mutations and RF actions.
- Hardware validation is authorized only for the N16R8 profile. Before flashing, record existing NVS-backed signals/rules, backend selection, Wi-Fi configuration, and current GPIO values. Build and flash only through:
  `tools/build-board.sh esp32s3-devkitc-n16r8 flash --port /dev/serial/by-id/usb-EXAMPLE_N16R8-if00`
  Then verify the recorded data remains intact, exercise one safe non-overlapping GPIO pair through the Web UI, confirm reboot persistence, and restore the original pair.

## Assumptions

- No `erase_flash`, NVS erase, partition erase, or equivalent operation is permitted; the approved flash workflow must preserve NVS.
- Controls remain available even when CC1101 is selected.
- Generic TX/RX may overlap CC1101 pins, but never each other or the physically connected activity LED.
- A single profile-tagged record is sufficient for a flashed image; retained data from another profile falls back safely.
- Other profiles receive build and localhost UI validation only; no additional hardware flash is planned.
