# Native ESP32 + CC1101 433 MHz RF Tool

Native ESP-IDF 6.0.2 firmware for receiving and transmitting fixed-code 433.92 MHz ASK/OOK remote-control signals with a classic ESP32 DevKit and a CC1101 transceiver.

It provides:

- All 12 current `rc-switch` protocol definitions and compatible protocol numbering.
- Hardware-timed ESP32 RMT capture and transmission—no Arduino layer or GPIO bit-banging.
- Stable complete-frame decoding with repeated-frame consensus and ambiguity rejection.
- Raw capture/replay for stable repeated OOK waveforms that do not match a known protocol.
- A UART console for inspection, named learning, direct sending, replay, diagnostics, and radio recovery.
- Versioned named signal storage in NVS; the unnamed latest frame remains RAM-only and is lost at reboot.
- No Wi-Fi or MQTT.

## Hardware

The pictured “CC1101 V2.0” module is a generic board; its connector order is not standardized. Wire by the module's **printed signal names**, not by physical header position.

Default strap-safe wiring for a classic ESP32 DevKit/WROOM:

| CC1101 signal | ESP32 | Direction / purpose |
|---|---:|---|
| `VCC` | `3V3` | 3.3 V only |
| `GND` | `GND` | Common ground |
| `SCK` | GPIO18 | SPI clock |
| `SO` / `GDO1` / `MISO` | GPIO19 | SPI data from CC1101 and `CHIP_RDYn` |
| `SI` / `MOSI` | GPIO23 | SPI data to CC1101 |
| `CSN` | GPIO27 | Manual active-low chip select |
| `GDO0` | GPIO26 | ESP32 RMT TX into CC1101 asynchronous TX input |
| `GDO2` | GPIO25 | CC1101 asynchronous RX output into ESP32 RMT RX |

Recommended hardware details:

- Place 100 nF ceramic plus 4.7–10 µF bulk capacitance near the module's VCC/GND pins.
- Add a roughly 10 kΩ pull-up from `CSN` to 3.3 V.
- Keep SPI/GDO wiring short; breadboards and long Dupont wires reduce RF and SPI reliability.
- Use the 433 MHz version of the module with its normal 26 MHz crystal.
- Attach the 433 MHz SMA antenna **before transmitting**.
- Never power or drive the CC1101 at 5 V. Avoid powering one board while the other is unpowered.

## Build

Activate an ESP-IDF 6.0.2 environment using the installation method for your system, then build:

```bash
# Example for a standard ESP-IDF checkout:
. "$HOME/esp/esp-idf/export.sh"
idf.py set-target esp32
idf.py build
idf.py size
```

To configure pins, center frequency, nominal power, RX inversion, repeat count, or duplicate window:

```bash
idf.py menuconfig
# CC1101 RF configuration
```

Framework defaults are in `sdkconfig.defaults`; shared radio options are defined by the owning components' `Kconfig` files. The generated `sdkconfig` should not be edited by hand. The project defaults to the 4 MB flash detected on the target DevKit; select the real size in `menuconfig` before flashing a board with different flash capacity.

Flash only when the correct serial port and wiring have been verified:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Exit the monitor with `Ctrl+]`. The software build and host-test results do not imply that a particular CC1101 module has been RF-tested.

## Serial console

UART0 runs at 115200 baud. Type `help` to list commands. The console uses linenoise dumb mode because RF events arrive asynchronously; command history, arrow-key editing, and tab completion are intentionally disabled so event output cannot corrupt the active input line.

### Receive

RX starts automatically. Known frames are printed as:

```text
RX RC code=11043138 hex=0xA88142 bits=24 protocol=1 pulse_us=386 confidence=repeated repeats=3 fingerprint=0x...
```

Unknown stable repeated frames are printed in copyable form:

```text
RX RAW start=1 count=20 repeats=3 fingerprint=0x... durations=210,740,330,...
```

Repeated observations from one hold are suppressed for 350 ms by default. A single complete decoded frame is accepted only when its capture boundary is trusted and is marked `confidence=single`. Raw fallback requires at least two aligned periods and is never accepted from a truncation-suspected capture. A final delimiter stopped by the 30 ms RMT threshold is treated as censored timing evidence rather than an exact duration.

Control reception with:

```text
rx off
rx on
```

### Send a decoded rc-switch value

```text
send <code> <bits> <protocol> [pulse_us] [repeats]
```

Examples:

```text
send 11043138 24 1
send 0xA88142 24 1 386 8
```

- `code` accepts decimal or `0x` hexadecimal.
- `bits` must be 4–64 and the code must fit.
- `protocol` must be 1–12.
- Omitted `pulse_us` uses the nominal rc-switch protocol timing.
- Omitted repeats use `CONFIG_RF_DEFAULT_TX_REPEATS` (default 8); the valid range is 1–20.
- Total continuous transmission is bounded to five seconds.

### Inspect, learn, and replay frames

The latest accepted frame remains available only in RAM:

```text
last
replay
replay 10
```

Learn the next accepted decoded or raw signal under a persistent name:

```text
learn gate
# Press the remote after LEARN ARMED appears.
learn list
replay gate 10
forget gate
```

Learning rules:

- `learn <name>` waits up to 30 seconds for the next accepted signal that begins after `LEARN ARMED`. It prints `LEARNED name=...` after a successful NVS commit or `LEARN TIMEOUT name=...` when the window expires.
- Names are 1–15 characters, begin with a letter, and contain only letters, digits, `_`, or `-`. The name `list` is reserved.
- Only one learn request can be pending. A second valid request replaces the pending name and starts a fresh 30-second window. If the console event queue overflows, learning is cancelled with an error instead of risking saving a later signal under the wrong name.
- Existing names are never overwritten: `learn <existing-name>` fails without arming. Use `forget <name>` first when replacement is intentional.
- `learn list` enumerates committed names. `forget` erases only the selected record and commits the deletion.
- Persistent records use a versioned, checksummed format and are validated again before replay. Missing, corrupt, or unsupported records are never transmitted.
- NVS initialization or capacity errors disable/fail only persistent operations; RF and RAM commands continue. Capacity depends on the configured NVS partition and decoded/raw record sizes. Firmware never automatically erases NVS to recover an error.

Named replay requires an explicit repeat count from 1–20. Existing `replay` and `replay <repeats>` forms continue to use the latest RAM frame. Named replay does not replace that RAM frame.

RX is disabled during TX, stale captures are discarded, and the prior desired RX state is restored afterward.

### Stage and send raw timings

A staged workflow supports the complete 256-pulse bound without requiring one oversized command:

```text
raw begin 1
raw append 210 740 330 910 460 620 270 830
raw append 510 570 390 680 240 960 430 650
raw show
raw send 8
raw clear
```

Raw rules:

- Start level is 0 or 1.
- Durations are 100–29000 µs. This keeps pulses above the CC1101 asynchronous sampling floor and below the 30 ms RMT frame-stop threshold.
- A frame must contain an even 8–256 alternating pulses.
- The staged frame and unnamed last received frame are not persistent; only frames captured by `learn <name>` are stored in NVS.

### Diagnostics and recovery

```text
status
radio info
radio reset
radio start
```

Diagnostics include CC1101 PARTNUM/VERSION, stable MARCSTATE, RSSI in half-dBm units (`rssi_x2`), carrier/CCA bits, resets/recoveries/timeouts, RMT queue drops/truncations, duplicate count, and whether desired RX is actually armed. Software status remains available while RF is stopped or busy; hardware fields are explicitly marked unavailable when they cannot be sampled safely.

`radio reset` disables capture, performs the CC1101 reset/profile/readback/calibration sequence, and restores the prior desired RX state. `radio start` retries complete initialization after wiring or power is corrected. Boot also makes three bounded startup attempts. All command, SPI-ready, and radio-state waits are bounded. An unrecoverable classic-ESP32 RMT TX timeout attempts to force the CC1101 idle and leaves the service faulted; reboot is then required rather than risking a late transmission or an unbounded driver abort.

## Radio profile

The default CC1101 profile assumes a 26 MHz crystal:

- Center frequency: 433.920 MHz (register word `0x10B071`, approximately 433.91983 MHz).
- Transparent asynchronous serial mode; packet FIFO, CRC, whitening, and sync detection are bypassed.
- ASK/OOK modulation, approximately 4.8 kbaud demodulator rate and 203 kHz RX bandwidth.
- GDO2 provides raw demodulated RX data; GDO0 is the asynchronous TX-data input.
- OOK AGC baseline `AGCCTRL2/1/0 = 04/00/92`.
- Nominal default TX power: +5 dBm using TI's 433 MHz PATABLE guidance.

Generic-module matching networks, oscillators, antennas, and local regulations vary. Frequency, bandwidth/AGC sensitivity, actual output power, and usable range require measurement on the real hardware.

## Codec behavior

The codec was ported from the hardened native `RF_Bridge_Mqtt` implementation used during development, including fixes for:

- Complete-frame/repeat consensus instead of arbitrary pulse suffixes.
- Protocol 11/12 timing alias ambiguity.
- Raw periods containing embedded known-protocol fragments.
- One-bit-different raw identities, PWM topology changes, timing jitter, scaling, and cyclic capture phase.
- Capacity/truncation rejection and 4–64-bit bounds.

The 12 protocol timing definitions follow [`sui77/rc-switch`](https://github.com/sui77/rc-switch), LGPL-2.1-or-later; see `components/rf_codec/NOTICE.md` and `components/rf_codec/COPYING.LESSER`. The Arduino interrupt implementation is not included.

## Limits and safety

Supported:

- Fixed-code ASK/OOK remotes compatible with rc-switch's protocols.
- Stable repeated unknown ASK/OOK periods that fit the capture limits.

Not supported:

- Rolling-code garage, alarm, or vehicle remotes.
- Encryption, challenge/response, FSK/GFSK, Manchester-specific protocols, or CC1101 packet-mode traffic.
- Continuous/gapless streams larger than classic ESP32 RMT memory. RX uses seven 64-symbol blocks and TX uses the remaining block, so no other RMT peripheral can be used concurrently with the default build.
- Sharing `SPI3_HOST` with another component. The CC1101 driver owns that host exclusively so its reset and transaction deadlines remain deterministic.
- Simultaneous RX and TX; the CC1101 is operated half-duplex.

Do not attempt to clone security or access-control devices. Follow local 433 MHz frequency, power, bandwidth, duty-cycle, antenna-gain, and listen-before-talk rules.

## Tests

Run portable codec/parser regressions from a clean native shell (outside an activated ESP-IDF cross-toolchain environment):

```bash
cmake -S host_tests -B /tmp/esp32-cc1101-host-tests
cmake --build /tmp/esp32-cc1101-host-tests
ctest --test-dir /tmp/esp32-cc1101-host-tests --output-on-failure
```

Build the dedicated Unity image with ESP-IDF:

```bash
# With ESP-IDF 6.0.2 already activated:
cd test_apps/unit
idf.py -B build build
# Flash only after explicit approval and with a connected test board:
# idf.py -B build -p /dev/ttyUSB0 flash monitor
```

The Unity image uses an interactive menu; enter `*` and press Enter to run all tests. Building it does not mean its tests passed—only the on-device `0 Failures` summary does. The suite covers the hardened decoder vectors, raw matching, parser/deadline bounds, versioned storage records, frequency calculation, and PA selection; CC1101 SPI/RMT lifecycle, NVS persistence across reboot, and RF behavior still require hardware testing. RF timing/range and recovery claims are not proven by compilation alone.
