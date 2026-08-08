# Manual Decoded Signal Saving

## Summary

Add structured manual signal creation through the Web UI and UART. A successful save creates a normal persistent learned-signal record, displays it in the existing Learned signals list, and triggers the existing MQTT catalog reconciliation so Home Assistant receives a replay button.

## Interfaces

- Add UART command:
  `save <name> <code> <bits> <protocol> [pulse_us]`
- Accept decimal or `0x` hexadecimal codes. For example:
  `save gate 13830801 24 1 199`
- Add `POST /api/signals` with exact form fields `name`, `code`, `bits`, `protocol`, and optional `pulse_us`.
- Return `201 Created` after a committed save, `400` for invalid fields, and `409` when the name already exists.
- Add a compact "Add decoded signal" form above the existing Learned signals list. On success, clear the name/code fields and refresh the signal list and automation selectors.
- Add a typed bridge-control save request and operation shared by Web and UART.

## Implementation

- Validate names using existing learned-name rules, bits as 4-64, protocols as 1-12, code width against the selected bit count, and pulse timing against protocol-specific transport limits.
- Normalize an omitted or zero `pulse_us` to the selected protocol's nominal timing and derive `inverted` from the protocol.
- Store through the existing versioned `RfStoredSignal` NVS format. Do not introduce a storage migration.
- Keep saves create-only: never overwrite an existing name and never partially alter its record.
- Consolidate manual and recent-history saves around the same create, catalog-refresh, and `kSignalCatalogChanged` behavior.
- Publish the catalog event only after a successful NVS commit. MQTT then publishes the Home Assistant button immediately when connected or during its next normal reconciliation.
- Do not transmit RF, change the latest RAM frame, alter learning state, or add the manually entered signal to recent history.
- Ignore `confidence`, observed `repeats`, and `fingerprint`; they are reception metadata. Home Assistant replay continues using the existing configured repeat policy.
- Document the Web workflow, UART syntax, validation, persistence, and Home Assistant behavior in `README.md`.

## Test Plan

- Add host and Unity parser tests for decimal/hex codes, optional/zero pulse normalization, bounds, code-width overflow, malformed fields, extra fields, invalid names, and duplicate names.
- Verify NVS round-trip values, derived inversion, create-only conflicts, catalog refresh, and catalog-change event publication.
- Extend Web asset/API tests for the new route and form, successful refresh, validation failures, and duplicate-name feedback.
- Verify MQTT reconciliation creates exactly one retained Home Assistant button for the new name and removes it through the existing tombstone path after deletion.
- Run host tests, build the Unity image, and run the final unchanged production verifier.
- Validate desktop and mobile Web layouts with the deterministic mock and screenshots.
- Flash only `esp32-devkit` through `tools/build-board.sh` on `/dev/serial/by-id/usb-EXAMPLE_CLASSIC-if00`; verify UART and Web saves, reboot persistence, Web listing, and MQTT discovery without pressing the Home Assistant button or transmitting RF.

## Assumptions

- Direct creation is exposed through Web and UART only; MQTT provides the resulting replay button but no administrative signal-create command.
- The existing "Learned signals" label and internal learned-signal terminology remain unchanged.
- Existing uncommitted recent-history work is preserved, and no commit is created unless separately requested.
