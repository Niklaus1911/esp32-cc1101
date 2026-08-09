# End-to-End OTA Reliability

## Summary

Improve OTA so normal success means the exact uploaded image booted from the inactive slot and reached ESP-IDF's confirmed `VALID` state. Preserve the current dual-slot layout, unsigned trusted-LAN model, downgrade support, and conservative boot-health checkpoint.

## Implementation Changes

- Extend `components/ota_update/` to expose:
  - `board_profile`
  - full lowercase `running_elf_sha256` and `candidate_elf_sha256`
  - `running_image_state`: `undefined`, `new`, `pending_verify`, `valid`, `invalid`, `aborted`, or `unknown`
  - `running_image_state_error`
  - existing `pending_verification` for compatibility
- Return `target_partition`, candidate version, and candidate ELF hash in successful upload responses. JSON-escape all application metadata and clear stale candidate metadata when a new attempt begins.
- Keep confirmation at the existing post-startup checkpoint. AP availability, NVS degradation, frontend failures, and disconnected RF hardware remain nonfatal; state-query or confirmation failures must remain visible and must never be reported as confirmed.
- Make the shared HTTP server actually use `CONFIG_OTA_HTTP_PORT` and `CONFIG_OTA_HTTP_TASK_STACK_SIZE` everywhere: listener, authorization, mDNS, logs, console status, and OTA status. Retain the existing symbols and production defaults of port 80 and an 8192-byte stack.
- Update `tools/push-ota.sh` to accept:
  - `tools/push-ota.sh <host> <image>`: wait up to 120 seconds for confirmation.
  - `tools/push-ota.sh --no-wait <host> <image>`: stop after upload acceptance.
- During CLI preflight:
  - Extract the image's profile, version, and full ELF hash.
  - Record the current and target partitions.
  - Reject busy or pending-verification devices.
  - Compare the device profile when the running firmware exposes it.
  - Refuse wait-mode when the requested next-boot service is MQTT-only; direct the operator to use Web/Both or explicitly select `--no-wait`.
- Poll every two seconds after upload. Exit successfully only when the target partition, full ELF hash, and `valid` image state all match. Report rollback, target-but-unconfirmed, legacy metadata, and timeout as distinct nonzero outcomes. An older upload response may omit new fields, but the new candidate must expose them before confirmation succeeds.
- In `components/web_ui/`, inspect only the selected image prefix to obtain its profile, version, and ELF hash before upload. Reject obvious profile mismatches while retaining device-side validation as authoritative.
- Replace the current "first `/api/live` response means success" behavior with the same partition/hash/`valid` checks as the CLI. Detect rollback and unconfirmed boots explicitly. After 120 seconds, unlock the UI, retain the expected result for late reconciliation, and show that success is unknown.
- Block Web uploads when the requested next boot is MQTT-only. After confirmed success, preserve a one-time success notice in session storage and reload the page so assets match the new firmware.

## Public Interface Changes

- `OtaUpdateStatus` gains board profile, running/candidate ELF hashes, raw image-state classification, and image-state error fields.
- `GET /api/v1/ota/status` gains the corresponding additive JSON fields; existing fields remain unchanged.
- Successful `POST /api/v1/ota` responses remain backward compatible with `ok` and `rebooting`, while adding target identity.
- Plain and pretty `ota status` output includes image state, state error, and running/candidate hashes.
- No partition, NVS, RF, MQTT, or stored-record schema changes are introduced.

## Test Plan

- Add host and Unity tests for full SHA-256 formatting, every ESP-IDF OTA image-state mapping, unknown/error handling, and additive status contracts.
- Expand mocked uploader tests for exact confirmation, same-image slot switching, rollback, pending confirmation, missing legacy metadata, timeout, profile mismatch, busy preflight, MQTT-only refusal, and `--no-wait`.
- Validate Web behavior with a deterministic Playwright mock: success, rollback, unconfirmed target, lost upload response, timeout/late recovery, wrong profile, MQTT-only next boot, reload behavior, desktop/mobile screenshots, overflow, and browser errors.
- Run the clean host suite, both ESP32 and ESP32-S3 Unity builds, then `tools/verify-production.sh` once on the final unchanged firmware/assets/configuration candidate.
- Hardware validation remains separately approval-gated: confirm slot alternation, same-image updates, forced pre-confirmation rollback, lost connectivity, and post-reboot identity on approved boards.

## Assumptions

- OTA remains unauthenticated, unsigned, and HTTP-only on a trusted LAN.
- Same-version and downgrade images remain accepted, although legacy targets lacking exact identity fields cannot produce a confirmed-success exit.
- No resumable, compressed, delta, TLS, signing, Secure Boot, or eFuse anti-rollback work is included.
- The conservative checkpoint follows the existing project policy and ESP-IDF's application-defined rollback confirmation model.
