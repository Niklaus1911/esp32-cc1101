# Fast RX Activity LED Plan

## Hardware Decision

Use GPIO2 as the configurable default, active-high, with a 25 ms pulse.

- GPIO2 is free in the current wiring: CC1101 uses GPIO18/19/23/25/26/27 and UART0 uses GPIO1/3.
- Many DOIT/clone ESP32 DevKit V1 boards have an active-high blue LED on GPIO2.
- The official Espressif ESP32-DevKitC V4 does **not** have a controllable GPIO2 LED; its onboard red LED is a 5 V power indicator. On that board the feature requires an external LED/resistor on GPIO2 or must be disabled.
- GPIO2 is a boot-strapping pin, but configuring it only after application startup is safe for the common onboard LED circuit. Document that external circuitry must not force an invalid level during reset.

Provide Kconfig options for enable/disable, GPIO, polarity, and pulse duration so the firmware is not tied to one clone layout.

## Receive Semantics

Pulse once for each accepted logical `RfFrame` in `rf_ook`: decoded and raw frames after validation, repeated-frame consensus, and duplicate suppression. This matches UART `RX` events and avoids blinking continuously on malformed RF noise or every repeated packet within one remote-button transmission.

## Implementation

1. **Add an internal RF activity LED owner**
   - Add a small implementation under `components/rf_ook/` using native ESP-IDF GPIO and a one-shot `esp_timer`; do not introduce Arduino APIs or a task that sleeps.
   - Configure the output to its inactive level during initialization. Initialization is idempotent and nonfatal: an invalid/unavailable LED logs one warning but never prevents RF startup.
   - Validate that the selected GPIO is output-capable, does not use flash/UART pins, and does not overlap any configured CC1101 SPI/GDO pin.

2. **Implement a bounded retriggerable pulse**
   - On an accepted frame, arm/restart a 25 ms one-shot and drive the configured active level.
   - Use a deadline/generation check so a timer callback racing with a new frame cannot turn off a newly retriggered pulse.
   - Repeated accepted frames during the pulse extend the active interval to 25 ms after the newest frame. Timer/API failure must fail dark at the inactive level.
   - Keep the receive path nonblocking: no delays, allocations, UART output, queue waits, or NVS access in the notification path; the timer callback only resolves the deadline and updates the GPIO.

3. **Hook the accepted-frame path**
   - Initialize the activity LED as part of `start_rf_ook()` and retain it across `radio stop`/`radio start` lifecycle operations.
   - Notify it immediately after `s_accepted_frames` increments and before the existing console/automation callback.
   - Do not pulse during TX, maintenance, malformed captures, duplicate suppression, or when RX is disabled.

4. **Configuration and documentation**
   - Add `RF_ACTIVITY_LED_ENABLE` (default `y`), `RF_ACTIVITY_LED_GPIO` (default `2`), `RF_ACTIVITY_LED_ACTIVE_HIGH` (default `y`), and `RF_ACTIVITY_LED_PULSE_MS` (default `25`, bounded range) under the OOK service Kconfig menu.
   - Record explicit production and Unity defaults and make the production verifier assert them.
   - Update the README hardware/configuration sections with the clone-versus-official-board distinction, GPIO2 strapping caution, external LED wiring, logical-RX semantics, and polarity/duration configuration.

## Files

- `components/rf_ook/Kconfig`
- `components/rf_ook/CMakeLists.txt`
- `components/rf_ook/rf_ook.cpp`
- New activity LED implementation/private policy files under `components/rf_ook/`
- `components/rf_ook/test/test_rf_ook.cpp` and focused host/Unity policy coverage
- `host_tests/CMakeLists.txt`, `host_tests/host_tests.cpp` if the validation/deadline policy is portable
- `sdkconfig.defaults`, `test_apps/unit/sdkconfig.defaults`
- `tools/verify-production.sh`
- `README.md`

## Verification

1. Test GPIO policy boundaries, CC1101/UART/flash conflicts, polarity levels, disabled behavior, pulse-duration bounds, and retrigger/deadline race decisions without hardware.
2. Run normal and ASan/UBSan host tests, shell checks, `git diff --check`, the clean production verifier, and a clean Unity build.
3. Review radio-task/timer-callback stack usage, IRAM/DRAM/image growth, startup failure isolation, and radio stop/start behavior.
4. Run an independent blocker review focused on GPIO2 boot strapping, timer callback races, callback latency, and RF lifecycle ownership.
5. With explicit hardware approval, identify the exact DevKit variant, confirm LED polarity, receive decoded and unknown-raw signals, test rapid accepted frames and `radio stop`/`radio start`, and confirm no blink for duplicate-suppressed/noisy captures or during OTA maintenance.

No flashing, serial access, or on-device Unity execution is part of implementation verification without separate approval.

## References

- Espressif ESP32-DevKitC V4 guide: <https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html>
- Official DevKitC V4 schematic: <https://dl.espressif.com/dl/schematics/esp32_devkitc_v4_sch.pdf>
- ESP-IDF high-resolution timer API: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/esp_timer.html>
