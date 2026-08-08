# Simplify Random Signal Generation For Offline Use

## Summary

Keep the existing `random` UART command, Web button, NVS record format, collision retries, and Home Assistant behavior. Replace the Wi-Fi-dependent true-entropy workflow with a direct `esp_random()` call so generation works whether Wi-Fi is running or stopped.

## Implementation Changes

- Generate each 24-bit candidate directly in the shared bridge operation using `esp_random()`. Retain protocol 1, 350 us timing, `random_%06X` names, eight retries, create-only storage, and semantic collision checks.
- Remove `network_wifi_get_true_random_word()` and all associated Wi-Fi message types, reply queues, power-save switching, initialization cleanup, and `esp_hw_support` dependency.
- Add the ESP hardware RNG dependency to `bridge_control`, where randomness is consumed.
- Preserve `POST /api/signals/random`, its successful JSON schema, `503 random_generation_exhausted`, the Web button, and UART success output. Remove the obsolete `409 true_random_unavailable` path and Wi-Fi guidance.
- Change documentation and command help from "true-random" to "random." State that offline output is suitable for selecting unique fixed ASK/OOK codes but is not a cryptographic security mechanism.
- Do not transmit, replay, change the active RF backend, or modify MQTT interfaces.

## Test Plan

- Update host/source contracts to verify direct RNG use, complete removal of the Wi-Fi entropy plumbing, unchanged candidate boundaries, collision handling, Web/UART interfaces, and the no-transmit invariant.
- Run all host tests and compile ESP32 and ESP32-S3 Unity images.
- Reconfirm the deterministic Web success flow, learned-list refresh, same-origin request, no transmit requests, and unchanged desktop/mobile layout.
- Run the final unchanged-candidate four-profile production verifier.
- Flash `esp32s3-devkitc-n16r8` only through `tools/build-board.sh` and `/dev/serial/by-id/usb-EXAMPLE_N16R8-if00`, then monitor through the same validated workflow.
- Record initial Wi-Fi state, run `wifi stop`, execute `random`, confirm it succeeds while offline, and verify the generated name appears in `learn list`. Delete the temporary generated signal afterward and restore Wi-Fi only if it was initially active.
- If the device already runs Web or combined service mode and reconnects without configuration changes, exercise the physical Web button once and delete that temporary signal as well. Never replay either signal.

## Assumptions

- "Random" rather than continuously entropy-backed "true random" is the intended product promise.
- Existing UART, Web, NVS, MQTT discovery, and Home Assistant identifiers remain compatible.
- Wired flashing preserves existing NVS; no erase or network credential changes are allowed.
- Hardware authorization applies only to the connected N16R8 and its documented by-id path.
- No Git commit is included unless separately requested.
