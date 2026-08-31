# Temporary N16R8 Generic GPIO Overlap and Erase-First Deployment

## Summary

Change only `esp32s3-devkitc-n16r8` so Generic TX uses GPIO13 and Generic RX uses GPIO4. Preserve the existing CC1101 map and both software backends by permitting only these profile-specific directional aliases.

After final verification, erase the connected N16R8's entire flash and install the verified production firmware through the approved wired workflow.

## Implementation Changes

- Update the N16R8 board policy to Generic TX/RX `13/4`.
- Permit only Generic TX to alias CC1101 MISO on GPIO13 and Generic RX to alias CC1101 GDO0 on GPIO4 for N16R8. Reject reversed aliases, additional overlaps, and every overlap on other profiles.
- Make RF GPIO ownership backend-aware:
  - Generic mode configures GPIO13 as RMT TX and GPIO4 as RMT RX with pull-down, retaining TX-low cleanup.
  - CC1101 mode never drives GPIO13 through generic-idle handling.
  - Reset shared pins before CC1101/RMT initialization so GPIO13 becomes SPI MISO and GPIO4 becomes CC1101 RMT TX.
  - Preserve teardown, failed-start cleanup, hardware-switch serialization, and rollback to the previous backend.
- Update README and development-tooling pin tables to `13/4`. Document this as a temporary N16R8-only exception: CC1101 and generic modules must never be connected simultaneously, and rewiring must occur with power removed.
- Update pin-contract, board-policy, Unity metadata, documentation, and RF lifecycle source tests.
- Make no HTTP, console, MQTT, storage, Kconfig, public type, or RFBD format changes. Existing status interfaces expose the new GPIO values automatically.

## Test and Deployment Plan

- Run the clean native host suite, covering exact N16R8 alias acceptance, reversed or additional overlap rejection, and unchanged rejection on every other profile.
- Build the ESP32-S3 Unity image and confirm N16R8 metadata reports Generic TX13/RX4.
- Run `tools/verify-production.sh` once on the final unchanged candidate because GPIO behavior in production firmware changes.
- Confirm only the N16R8 is targeted and require the approved path `/dev/serial/by-id/usb-EXAMPLE_N16R8-if00` to exist and resolve to a character device. Do not substitute another serial path; the approved path was absent during planning and must reappear before deployment.
- Using ESP-IDF 6.0.2 and that exact path, run a full-chip `idf.py erase-flash`, require success, then immediately flash with:
  `tools/build-board.sh esp32s3-devkitc-n16r8 flash --port /dev/serial/by-id/usb-EXAMPLE_N16R8-if00`
- If erasure succeeds but flashing fails, report that the board is blank or incomplete and retry only the validated N16R8 flash on the same approved path.
- Do not open a monitor, run on-device Unity tests, or perform RF transmission without separate authorization. Report only erase and flash command success as hardware validation.

## Assumptions

- The N16R8 CC1101 mapping remains unchanged.
- Only one module set is physically connected to GPIO13/4 at a time; backend selection cannot make simultaneous wiring safe.
- The full-chip erase is intentional and removes NVS, Wi-Fi credentials, learned signals, automation rules, OTA state, and all other persisted configuration; nothing is automatically restored.
- Flash authorization applies only to the connected N16R8 and its exact approved by-id path.
- The exception remains documentation- and test-coherent until it is deliberately reverted.
