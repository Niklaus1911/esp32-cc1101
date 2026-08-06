# Board-Aware Web and MQTT System Diagnostics

## Summary

Commit the completed multi-board implementation before editing the feature. Then make the System page and MQTT output accurately represent the classic ESP32, N16R8, and XIAO ESP32-S3 while preserving all existing MQTT identities, commands, and Home Assistant entities.

## Commit And Compatibility

- Re-run `git diff --check`, host tests, both Unity builds, and `tools/verify-production.sh`; stage the intended multi-board source, board profiles, lockfiles, tests, documentation, and existing plans.
- Exclude and leave untouched the four untracked `n16r8-*.png` validation artifacts.
- Create baseline commit: `feat: add multi-board ESP32-S3 support`.
- Preserve `rfbridge/<12hex>/...`, `homeassistant/.../rfbridge_<12hex>/...`, MQTT client IDs, device identifiers, and all existing entity unique IDs.
- After feature verification, create `feat: expose board-aware system diagnostics`. Do not push.

## Web System Page

- Add a shared human-readable model name to `BoardInfo`:
  - `ESP32 DevKit + CC1101`
  - `ESP32-S3 DevKitC N16R8 + CC1101`
  - `Seeed Studio XIAO ESP32-S3 + CC1101`
- Add `board.model` to `/api/live`; retain existing board, service, heap-alias, internal-memory, and PSRAM fields.
- Rework "Runtime and services" into a three-column desktop layout containing Hardware, Runtime and memory, and Services, collapsing to one column below 820 px.
- Show model, profile, target, flash, PSRAM, console transport, uptime, reset reason, internal memory, and PSRAM. Show PSRAM as "Not installed" on classic ESP32.
- Add an expandable hardware diagnostic section for concurrent-service capability, activity LED configuration, CC1101 pin assignments, minimum-free watermarks, and largest blocks.
- Keep memory health based on internal RAM. Warn below a 16 KiB largest block on classic ESP32 or 32 KiB on combined-mode S3; retain the existing 12/24 KiB critical/free thresholds. The current N16R8 result of 30,720 bytes must display as fragmented/degraded, not healthy.
- Retain graceful rendering for older `/api/live` schemas and keep `app.js` below 32 KiB.

## MQTT Contract

- Pass board metadata into every discovery formatter. Keep manufacturer, identifier, name, and software version stable; make `model` board-specific and set `hw_version` to the exact profile slug.
- Add retained QoS 1 topic `rfbridge/<12hex>/state/system` with this fixed schema:
  - `board`: `profile`, `target`, `flash_mib`, `psram_mib`
  - `services`: `requested`, `effective`, `reboot_required`
  - `uptime_s`
  - `memory.internal` and `memory.psram`: `total`, `free`, `minimum`, `largest`
- Publish the system snapshot after connection, Home Assistant birth, forced reconciliation, OTA-maintenance recovery, and each existing 60-second audit.
- Add Home Assistant diagnostic sensors backed by `state/system`:

| Object ID | Value | Availability |
|---|---|---|
| `internal_free` | `memory.internal.free` | All boards |
| `internal_minimum` | `memory.internal.minimum` | All boards |
| `internal_largest` | `memory.internal.largest` | All boards |
| `psram_free` | `memory.psram.free` | PSRAM boards only |

- Configure these as `data_size` sensors in bytes, diagnostic category, with the shared availability topic and `expire_after: 180`.
- Extend MQTT retirement to tombstone every new discovery topic and `state/system`; tombstoning the conditional PSRAM entity on classic ESP32 is harmless.
- Keep the existing 1,024-byte bounded payload buffer and PSRAM cold-working-set ownership. No NVS or retained-ledger format migration is required.

## Verification

- Extend host tests for all three model mappings, unchanged legacy topics, exact system topic/schema, bounded JSON, sensor discovery payloads, conditional PSRAM discovery, and retirement tombstones.
- Extend Web tests for the new API fields, classic/S3/legacy-schema rendering, PSRAM absence, the 30,720-byte fragmentation warning, disconnected state, and asset-size limits.
- Run Playwright against deterministic classic and S3 fixtures at desktop and 390 px mobile widths; inspect screenshots and verify no overflow, clipping, console errors, or polling regressions.
- Run host tests, both Unity builds, three-profile production verification, image inspection, and `git diff --check`.
- Build the clean feature commit for N16R8, obtain the effective hostname from the device, and install through `.local` OTA without hardcoding its DHCP address or erasing NVS.
- Monitor only `/dev/serial/by-id/usb-EXAMPLE_N16R8-if00`; verify Both mode, Web rendering, MQTT reconciliation, zero state-publish failures, and at least three 60-second system publications.
- Sample memory for ten minutes. Require no reset, stack regression, monotonic leak, or sustained internal-memory loss above 1 KiB versus the validated baseline. Continue reporting the known 30,720-byte largest-block threshold miss.
- RF validation remains deferred because the N16R8 has no CC1101 connected; classic ESP32 and XIAO receive compile-only validation in this change.
