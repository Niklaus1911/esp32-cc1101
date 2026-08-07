# Add ESP32-S3FH4R2 SuperMini Profile

## Summary

Add `esp32s3-supermini-fh4r2` as the fourth supported board profile. It targets ESP32-S3FH4R2 with 4 MB flash, 2 MB Quad PSRAM, native USB Serial-JTAG, and simultaneous Web+MQTT capability.

The connected board is authorized for flashing and monitoring through this exact path:

`/dev/serial/by-id/usb-EXAMPLE_SUPERMINI_FH4R2-if00`

No `/dev/ttyACM0` fallback or port auto-detection will be used.

## Implementation Changes

- Add `BoardProfile::kEsp32s3SuperminiFh4r2` with board ID `4`.
- Add `CONFIG_PLATFORM_BOARD_ESP32S3_SUPERMINI_FH4R2`.
- Add metadata for `esp32s3-supermini-fh4r2`, `ESP32-S3 SuperMini FH4R2 + CC1101`, ESP32-S3, 4 MiB flash, 2 MiB PSRAM, USB Serial-JTAG, and combined Web+MQTT support.
- Embed descriptor `52464244 01100402 04010000 00000000`.
- Use CC1101 SCK GPIO12, MISO/GDO1 GPIO13, MOSI GPIO11, CSN GPIO10, GDO0 GPIO4, and GDO2 GPIO5.
- Allow exposed GPIO1-GPIO13 and TX/RX GPIO43/GPIO44 while rejecting straps, USB GPIO19/20, flash/PSRAM GPIO26-GPIO37, GPIO48, duplicate pins, and unsafe assignments.
- Disable the activity LED by default and never add WS2812 support. GPIO48 remains unavailable to activity-LED overrides.
- Add board defaults with 4 MB flash, native USB console, disabled UART, Quad PSRAM, the CC1101 map, disabled LED, and the board partition table.
- Move `CONFIG_SPIRAM_MODE_OCT=y` from shared S3 defaults into N16R8/XIAO defaults and set `CONFIG_SPIRAM_MODE_QUAD=y` for FH4R2.
- Add a 4 MB two-slot OTA table with `0x1e0000` application slots and preserved NVS/OTA offsets.

## Tooling and Interfaces

- Extend `tools/build-board.sh` for build, size, flash, and monitor using the exact approved by-id path.
- Add local ignored VS Code tasks for FH4R2 build, flash, and monitor.
- Extend production verification for all four profiles, descriptor/partition/size checks, FH4R2 Quad PSRAM, existing S3 Octal PSRAM, GPIO defaults, USB console, disabled LED, and combined services.
- Extend OTA image validation for the FH4R2 descriptor, 4 MB header, and `0x1e0000` OTA limit.
- Keep Web and MQTT topic schemas unchanged; existing system metadata must report the new profile and memory sizes.

## Tests and Acceptance

- Extend host board-policy, MQTT discovery, OTA script, and Unity coverage for FH4R2.
- Run host tests, Unity compilation, all profile builds, and the production verifier.
- Flash without erasing NVS and monitor native USB output through the approved path.
- Validate offline boot, Quad PSRAM detection, native USB console commands, board/memory status, profile descriptor, no boot loop/assertion, and CC1101 initialization only if the radio is connected.
- Do not perform Wi-Fi, DHCP, mDNS, Web, MQTT, Home Assistant, LAN OTA, reconnect, or network soak tests.

## Assumptions

- The connected board is the pictured ESP32-S3FH4R2 SuperMini and the reported USB Serial-JTAG path belongs to it.
- GPIO48 is an onboard LED data pin and must never be driven by firmware.
- Existing N16R8 and classic ESP32 paths and unrelated worktree changes remain untouched.
