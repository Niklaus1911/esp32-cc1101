# Qualify an 80 MHz ESP32 CPU Clock

## Decision Context

The current clean ESP-IDF 6.0.2 configuration resolves to a fixed 160 MHz CPU with power management disabled. The best first power-saving candidate is a fixed 80 MHz CPU, not dynamic frequency scaling or automatic Light-sleep:

- On classic ESP32, fixed 80 MHz operation keeps APB at 80 MHz, so the configured 4 MHz CC1101 SPI clock and the RMT peripheral clock are not intentionally reduced.
- RMT pulse timing is hardware-derived and `esp_timer` deadlines remain wall-clock based. The main risk is reduced CPU/ISR scheduling margin while decoding RF, refilling long RMT transmissions, printing UART output, handling Wi-Fi, or processing OTA.
- The RF service keeps non-DMA RMT channels enabled. ESP-IDF acquires power-management locks between `rmt_enable()` and `rmt_disable()`, so DFS would spend little useful time below the configured maximum while RF is active. Light-sleep would also conflict with continuous RF capture and interactive UART semantics.

Authoritative references: [ESP-IDF power management](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/system/power_management.html), [RMT power management](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/peripherals/rmt.html#power-management), and [SPI clock behavior](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/peripherals/spi_master.html#spi-clock-frequency).

## Phase 1: Reproducible A/B Images

1. Add a small root-level CPU-clock defaults overlay for the 80 MHz candidate. Keep `sdkconfig.defaults` at the current 160 MHz behavior until hardware qualification passes; never edit generated `sdkconfig` files.
2. Extend `tools/verify-production.sh` with a validated optional clock-profile input that supports only `160` and `80`, uses separate `/tmp` build/log directories, applies the overlay only for the 80 MHz build, and asserts the generated `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` value. The existing no-argument production path must remain 160 MHz and retain its current artifact location for OTA tooling compatibility.
3. Add an explicit application startup log in `main/main.cpp` containing the compiled CPU frequency and whether dynamic power management is enabled. This makes serial captures self-identifying without changing stable `plain` console records.
4. Build both profiles and the Unity image at both frequencies. Keep `CONFIG_PM_ENABLE` disabled in every profile; do not alter RMT clock sources, task priorities, queues, watchdogs, or timeouts during this experiment.

## Phase 2: Hardware Qualification

Hardware work requires explicit approval plus a confirmed board and serial port. Use the same board, CC1101, antenna, supply voltage, RF activity LED setting, Wi-Fi credentials, firmware revision, and test signals for both profiles.

1. Record board-level average and peak current for at least these states: RF RX active with Wi-Fi off; RF RX active with Wi-Fi associated and idle; sustained RF receive traffic; repeated automation replay; sustained Wi-Fi traffic; and OTA upload. Measure at the same supply point and allow equal settling time.
2. Capture `console style plain` plus `status`, `wifi status`, and `ota status` before and after each run. Compare existing counters for RF queue drops, truncation, command timeouts, console drops, CC1101 ready/state timeouts and recoveries, automation queue/log drops and TX errors, Wi-Fi event drops, and OTA failures.
3. Validate RX margin with repeatable generated bursts, including short inter-frame gaps and RF traffic concurrent with Wi-Fi and frequent console output. Compare decoded/raw results and accepted-plus-suppressed observations; require no new queue drops, truncations, or missed logical signals at 80 MHz.
4. Validate TX with a logic analyzer on GDO0: test the shortest supported decoded pulse pattern and the largest valid raw replay at 20 repeats, both idle and under Wi-Fi/console load. Require no inserted gaps or underflow and pulse durations matching the 160 MHz baseline within analyzer resolution and the 1 us RMT time base.
5. Exercise automation cooldown boundaries and alternating trigger/target rules. Compare capture-to-action `elapsed_ms`; require no errors and no material latency regression beyond an agreed operational budget.
6. Run successful, interrupted, slow-client, and full-speed OTA uploads. Confirm maintenance entry/exit, RF restoration, image validation, reboot confirmation, and absence of interrupt/task watchdog diagnostics.
7. Finish with a multi-hour combined-load soak. Reject the 80 MHz profile on any WDT reset, RF service stoppage, RMT timeout, unexplained queue/drop counter increase, CC1101 timeout increase, lost automation action, Wi-Fi instability, or failed maintenance recovery.

## Acceptance And Promotion

1. Before testing, define the minimum useful power benefit for the real operating mode; a practical starting gate is at least 5 mA or 10% board-level reduction, whichever is easier to measure reliably.
2. Promote 80 MHz only if all functional gates pass and the measured saving clears that threshold. Then set `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80=y` in both `sdkconfig.defaults` and `test_apps/unit/sdkconfig.defaults`, make the production verifier expect 80 MHz by default, and document the fixed-clock choice and validated operating envelope in `README.md`.
3. Re-run host tests, `tools/verify-production.sh`, and the dedicated Unity image build after promotion. Repeat the critical RX/TX/current smoke tests on the final production image.
4. If 80 MHz fails, retain 160 MHz and preserve the measurement report. Investigate the specific CPU-bound path before retrying; do not mask insufficient margin by enlarging queues or relaxing timeouts.
5. Treat DFS, automatic Light-sleep, RMT duty cycling, or CC1101 sleep/wake as a separate future design requiring explicit tolerance for missed wake-triggering RF frames and UART wake behavior.

## Expected Files

- New 80 MHz sdkconfig defaults overlay at the repository root.
- `tools/verify-production.sh` for selectable, asserted A/B builds.
- `main/main.cpp` for self-identifying startup clock output.
- `sdkconfig.defaults` and `test_apps/unit/sdkconfig.defaults` only after hardware acceptance.
- `README.md` for the final measured result and production policy.
