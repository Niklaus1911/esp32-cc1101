# Native ESP32 + CC1101 rc-switch RF Tool

## Goal

Build a 100% native ESP-IDF 6.0.2 application for a classic ESP32 DevKit and the pictured generic CC1101 V2.0 433 MHz SMA module. It will continuously receive common fixed-code ASK/OOK remotes, report rc-switch-compatible decodes, preserve stable unknown signals as raw pulse periods, and transmit either representation from a UART console.

There will be no Arduino layer, Wi-Fi, MQTT, or NVS. The last accepted frame exists only in RAM and is lost on reboot. Rolling-code, encrypted, FSK, and CC1101 packet-mode remotes are explicitly out of scope.

## Locked hardware defaults

Use strap-safe classic ESP32 pins:

| CC1101 signal | ESP32 GPIO |
|---|---:|
| SCK | 18 |
| MISO / GDO1 | 19 |
| MOSI | 23 |
| CSN | 27 |
| GDO0 asynchronous TX input | 26 |
| GDO2 asynchronous RX output | 25 |
| VCC / GND | 3V3 / GND |

The module name “V2.0” does not standardize its header order, so the README will require wiring by silkscreened signal name rather than connector position. Assume the usual 26 MHz crystal, use short wiring, add local 100 nF plus 4.7–10 µF decoupling, recommend a 10 kΩ CSN pull-up, and never transmit without the 433 MHz antenna attached.

## Implementation approach

### 1. Establish the native component layout

- Replace `main/main.c` with a small C++ `app_main` that starts the radio service and UART console.
- Add build-time Kconfig defaults for the six GPIOs, 26 MHz crystal, 433.920 MHz center frequency, conservative transmit power, default repeat count, RX inversion, and duplicate-suppression window. Keep generated `sdkconfig` out of manual edits; put intentional defaults in `sdkconfig.defaults`.
- Use fixed-size structs/arrays and bounded queues throughout. Avoid heap allocation and UART logging in ISRs.

### 2. Implement a defensive CC1101 driver

Create `components/cc1101/` around ESP-IDF SPI master and GPIO APIs:

- Use `SPI3_HOST`, mode 0, polling transactions, no DMA, manual CSN, and bounded `CHIP_RDYn` waits on MISO.
- Implement the TI reset sequence, register/strobe access, burst PATABLE writes, critical-register readback, PARTNUM/VERSION diagnostics, and timeout-returning APIs.
- Configure transparent asynchronous ASK/OOK: `PKT_FORMAT=3`, ASK/OOK modulation with sync detection disabled, GDO2 as asynchronous serial RX output, and GDO0 high-impedance except for its automatic TX-data-input role.
- Start from a TI DN022-style 26 MHz profile at 433.91983 MHz, approximately 4.8 kbaud and 203 kHz RX bandwidth, OOK AGC baseline `04/00/92`, and a conservative nominal +5 dBm PATABLE entry. Keep the profile centralized and document which values require real-hardware tuning.
- Read dynamic status registers until two consecutive values agree, as required by the CC1101 SPI synchronization erratum. Use stable `MARCSTATE` checks and deadlines for `SIDLE`, `SCAL`, `SRX`, and `STX`.
- Provide deterministic RX/TX transitions and escalating recovery: return to IDLE, clear unexpected FIFO error states, recalibrate, then fully reset/reapply/read back the profile if necessary. Never leave the PA active after an RMT or state timeout.
- Expose bounded diagnostics for identity, state, RSSI, carrier sense, timeouts, resets, and the active profile; RSSI is diagnostic rather than calibrated measurement.

### 3. Port and strengthen the rc-switch-compatible codec

Create a hardware-independent `components/rf_codec/` based on the hardened implementation in `$HOME/esp-projects/RF_Bridge_Mqtt`:

- Preserve all 12 current rc-switch protocol numbers, pulse factors, inversion flags, nominal timings, MSB-first encoding, and 4–64-bit validated code range.
- Preserve complete-frame segmentation, trusted capture-boundary handling, repeated non-overlapping consensus, nominal/residual candidate ranking, and fail-closed ambiguity behavior—especially the protocol 11/12 timing alias.
- Prefer canonical decoded identity `(protocol, bits, code)` whenever a complete waveform uniquely proves it. Pulse width remains transmission metadata, not identity.
- Accept an idle-bounded single complete decoded frame with explicit lower confidence so short taps remain visible; never trust an arbitrary capture-start suffix or a truncation-suspected single frame.
- Preserve raw fallback only for an unknown, delimiter-bearing complete period demonstrated by at least two aligned repeats. Canonicalize its phase and median timings, reject truncated/ambiguous captures, and cap it at 256 alternating pulses.
- Preserve strict raw identity rules: bounded jitter/global scale and cyclic phase are tolerated, but changed PWM topology or one changed data bit must remain a distinct signal. Embedded rc-switch fragments must not promote an extended raw period.
- Build both decoded and raw waveforms with overflow, duration, bit-width, repeat-count, and total-airtime validation.
- Retain an attribution notice for the rc-switch protocol definitions (LGPL-2.1-or-later) while keeping the transport fully native ESP-IDF/RMT rather than importing Arduino interrupt code.

### 4. Add a serialized CC1101/RMT radio service

Create `components/rf_ook/` using the modern ESP-IDF RMT API:

- Route GDO2 to a 1 MHz RMT RX channel and GDO0 to a separate 1 MHz RMT TX channel. Allocate enough classic-ESP32 RMT memory for the bounded capture while leaving capacity for TX.
- Use two internal-RAM RX buffers. The ISR callback only records the completed buffer metadata/generation/timestamps and queues it; task context snapshots it, rearms RX before decoding, merges adjacent equal levels, and reports queue drops or truncation.
- A single radio-owner task performs all non-ISR RMT and CC1101 state operations. Console handlers submit bounded commands and wait for results instead of touching hardware concurrently.
- On TX, invalidate stale RX events, stop RX, drive GDO0 low, enter and verify CC1101 TX, send the complete RMT frame for 1–20 repeats, force the end level low, stop the carrier, and restore/reverify RX even after failure.
- Bound one transmission to five seconds, reject malformed raw data, and suppress self-reception. Track recovery, queue-drop, truncation, accepted-frame, and duplicate counters.
- Collapse repeated observations from one physical hold within the configurable window. Save only the latest accepted decoded/raw frame in RAM for inspection and replay.

### 5. Provide a strict serial console

Create `components/rf_console/` with the native ESP-IDF console on UART0 at 115200 baud:

- Start in RX mode and print machine-copyable receive records. Decoded output includes decimal/hex code, bits, rc-switch protocol, measured pulse length, confidence, repeats, and fingerprint. Raw output includes start level, pulse count, repeats, fingerprint, and bounded durations.
- Implement: `help`, `status`, `radio info`, `radio reset`, `rx on`, `rx off`, `last`, `replay [repeats]`, `send <code> <bits> <protocol> [pulse_us] [repeats]`, and a bounded `sendraw`/staged raw-load workflow that can represent the full 256-pulse limit without exceeding a console line.
- Accept decimal and `0x` codes, reject trailing junk and overflow, use the protocol nominal pulse when omitted, and display actionable errors. No command persists settings or frames.

### 6. Document wiring, use, and limitations

Add `README.md` with the labeled-pin wiring table, decoupling/antenna guidance, build/monitor commands, sample RX and TX sessions, CLI syntax, profile defaults, raw replay workflow, and diagnostics. Clearly state half-duplex behavior, ASK/OOK-only support, no rolling-code cloning, regional duty-cycle/power obligations, and that generic-module RF performance must be validated on hardware.

## Expected files

- `components/cc1101/{CMakeLists.txt,cc1101.cpp,include/cc1101.hpp}`
- `components/rf_codec/{CMakeLists.txt,rf_codec.cpp,include/rf_codec.hpp,NOTICE.md}`
- `components/rf_ook/{CMakeLists.txt,rf_ook.cpp,include/rf_ook.hpp}`
- `components/rf_console/{CMakeLists.txt,rf_console.cpp,include/rf_console.hpp}`
- Focused tests under each component’s `test/` directory
- `host_tests/` for executable pure-codec/parser regression tests
- `test_apps/unit/` for the ESP-IDF Unity image and mocked driver/state tests
- `main/main.cpp`, `main/CMakeLists.txt`, `main/Kconfig.projbuild`
- `sdkconfig.defaults`, `README.md`
- Remove the superseded `main/main.c`

## Verification

1. Run host tests for all protocol encode/decode vectors, decimal/hex CLI parsing, malformed bounds, ambiguity, short taps, repeated consensus, capture prefixes/suffixes, truncation, raw jitter/phase/one-bit distinctions, maximum pulse counts, and exact TX waveform generation.
2. Add mocked CC1101 tests for SPI command headers, frequency/profile calculations, critical readback failures, erratum-safe status reads, transition deadlines, TX-failure carrier shutdown, and reset/recovery escalation.
3. Build the dedicated ESP-IDF Unity image, then run the production `idf.py build` and `idf.py size`; treat builds as compilation evidence, not proof of RF behavior.
4. After separate approval to flash, require the Unity image’s on-device `0 Failures` result before production hardware testing.
5. Hardware acceptance: verify module identity/profile; capture every button using short taps and long holds; run at least 20 presses of a known rc-switch remote without identity drift; transmit decoded values back to the target device; capture/replay a non-rc-switch fixed raw signal; test weak/strong range, idle noise, repeated RX↔TX cycles, radio reset, missing-module startup, TX timeout recovery, and confirmation that the PA returns off and RX resumes.
6. Do not claim raw/decoded RF compatibility, range, power, or recovery as hardware-validated until those tests are actually performed. Never flash without explicit user approval.
