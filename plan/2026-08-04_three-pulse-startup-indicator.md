# Separate the Three Startup Flashes

## Summary

Keep each GPIO2 startup pulse at 25 ms, but increase the dark interval between pulses from 25 ms to 150 ms. The visible sequence becomes `25 ms on, 150 ms off, 25 ms on, 150 ms off, 25 ms on`, lasting 375 ms and making all three flashes distinct.

## Implementation Changes

- Add a fixed 150 ms startup-gap constant; do not change the existing 25 ms RF activity pulse configuration or add another Kconfig option.
- Extend the startup policy step to return both the GPIO level and that phase's duration: 25 ms for active phases and 150 ms for inactive phases.
- Make the timer arm itself from the current startup step's duration instead of applying `RF_ACTIVITY_LED_PULSE_MS` to every phase.
- Retain a 150 ms inactive settling gap after the third flash before delivering any coalesced early RF activity.
- Preserve once-per-boot behavior, active-low support, timer failure handling, GPIO safety validation, and ordinary RF pulse deadline extension.
- Update the startup log, Kconfig help, and README to state `3 x 25 ms` flashes with `150 ms` dark gaps.

## Test Plan

- Update host and Unity policy tests to assert the exact level and duration sequence for active-high and active-low LEDs: `25, 150, 25, 150, 25, 150 ms`, with completion after the final inactive phase.
- Run host tests, the Unity image build, ESP-IDF MCP build, `tools/verify-production.sh`, and `git diff --check`.
- Flash the authorized classic ESP32 through the confirmed CP2102 port without erasing NVS, reset through ESP-IDF monitor, and verify the new timing configuration is logged and all RF/network services recover.
- Do not pause for user input during implementation. Report physical visual separation as verified only if it is directly observed.

## Assumptions

- The selected dark gap is 150 ms.
- GPIO2 remains enabled, active-high, with the existing 25 ms pulse configuration.
- This replaces the timing portion of the existing uncommitted startup-indicator implementation; unrelated serial-console changes remain untouched.
