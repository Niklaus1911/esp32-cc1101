# All-Profile OTA Validation with N16R8 Hardware

## Summary

Validate OTA build and image contracts for all four board profiles. Perform real flash, serial, CLI OTA, and Web OTA validation only on the connected `esp32s3-devkitc-n16r8`. Report other profiles as offline-validated, not hardware-validated.

## Validation Matrix

| Profile | Offline validation | N16R8 mismatch test | Hardware validation |
|---|---|---|---|
| `esp32-devkit` | Build, size, header, descriptor, partition contract | Must reject before upload | Not run |
| `esp32s3-devkitc-n16r8` | Same offline gates | N/A | Flash, monitor, CLI OTA, Web OTA |
| `xiao-esp32s3` | Same offline gates | Must reject before upload | Not run; no approved port |
| `esp32s3-supermini-fh4r2` | Same offline gates | Must reject before upload | Not run |

## Validation Sequence

1. Freeze the candidate by recording `HEAD`, the binary diff hash, Git status, and SHA256 identities for all four images. Reuse the existing successful host, Unity, browser, and four-profile production-verifier results if production inputs remain unchanged; otherwise rerun the affected gates and one final production verifier.
2. Require the approved N16R8 path `/dev/serial/by-id/usb-EXAMPLE_N16R8-if00` to exist and resolve to a character device. Restate the exact board and path before hardware operations.
3. Before flashing, read the existing `0x6000`-byte NVS partition at `0x9000` into a timestamped `/tmp` backup and record its checksum. Never erase flash or automatically restore this backup.
4. Flash production firmware only through `tools/build-board.sh esp32s3-devkitc-n16r8 flash --port <approved-path>`. Start the monitor with the same wrapper and path, keep it open across OTA reboots, and switch the console to plain output.
5. Capture `board status`, `memory status`, `radio info`, `wifi status`, `hostname status`, `service status`, `ota status`, `learn list`, and `rule list`. Require the N16R8 profile, ESP32-S3 target, 16 MiB flash, 8 MiB PSRAM, UART0 console, correct image hash, and `ESP_OK` OTA state/confirmation queries. A disconnected RF frontend is recorded but remains nonfatal to OTA.
6. Preserve all existing NVS configuration. Continue with network tests only if saved Wi-Fi comes online and the requested service mode is already `web` or `both`. If not, stop after wired validation and report the network portion as blocked without changing credentials or service mode.
7. Against the running N16R8, invoke `push-ota.sh` with the clean ESP32, XIAO, and SuperMini images. Each must fail with a profile mismatch before POST; compare OTA status before and after to prove the running partition, digest, candidate identity, and byte counters were unchanged.
8. Upload the exact N16R8 image through the default CLI. Require exit zero only after the inactive slot is running with the expected full ELF SHA256, image state `valid`, and both state-query and confirmation errors equal to `ESP_OK`. Verify the serial monitor shows the reboot and clean startup.
9. Upload the same N16R8 image a second time through the real Web UI using Playwright. Confirm the dialog reports the expected profile, version, digest, and target slot. Require the one-time success notice after reload and exact API identity. This must produce slot progression `ota_0 -> ota_1 -> ota_0`, proving both slots and same-image updates.
10. Inspect the real Web UI on desktop and mobile after confirmation. Check console/page errors, polling recovery, same-origin requests, clipping, horizontal overflow, and firmware-status readability. Intercept every mutation except the explicitly authorized OTA upload.
11. Repeat the console status and persistent catalog checks, stop the monitor with `Ctrl+]`, and produce a result matrix containing exact commands, image hashes, slot transitions, API states, screenshots, failures, and explicit hardware coverage limitations.

## Acceptance Criteria

- All four profiles retain successful clean production image, descriptor, partition, configuration, and 25% free-slot checks.
- All three foreign images are rejected by the N16R8 without upload or state mutation.
- Wired flash boots the expected N16R8 profile without erasing NVS.
- CLI and Web OTA each confirm the exact uploaded digest and `valid` state on alternating slots.
- Learned signals, rules, hostname, Wi-Fi record, and service selection remain semantically unchanged.
- Any mismatch or lost confirmation stops subsequent OTA work and leaves the board on the last confirmed image.

## Interfaces and Assumptions

- No public API, storage schema, firmware behavior, or tracked source change is planned.
- Safe full-path validation excludes deliberate rollback fault injection and interrupted-upload testing; those remain covered by mocks, not claimed as hardware-tested.
- No on-device Unity image, RF transmission, replay, credential change, service-mode change, generic serial path, or flash erase is authorized.
- Runtime claims for ESP32 DevKit, XIAO, and SuperMini remain deferred until those exact boards and approved paths are connected.
