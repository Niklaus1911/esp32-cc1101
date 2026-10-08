# Native ESP32 433 MHz RF Tool

Native ESP-IDF 6.0.2 firmware for receiving and transmitting fixed-code 433.92 MHz ASK/OOK remote-control signals through either a CC1101 or direct-data generic transmitter/receiver pair such as STX882/SRX882. Four board-specific images support a classic ESP32 DevKit, an ESP32-S3 N16R8 DevKitC-compatible board, a Seeed Studio XIAO ESP32-S3, and an ESP32-S3FH4R2 SuperMini.

It provides:

- All 12 current `rc-switch` protocol definitions and compatible protocol numbering.
- Hardware-timed ESP32 RMT capture and transmission—no Arduino layer or GPIO bit-banging.
- Live, persistent selection of exactly one RF backend through Web, MQTT/Home Assistant, or UART.
- Stable complete-frame decoding with repeated-frame consensus and ambiguity rejection.
- Raw capture/replay for stable repeated OOK waveforms that do not match a known protocol.
- A UART console for inspection, named learning, persistent receive-to-replay automation, direct sending, replay, diagnostics, and radio recovery.
- Versioned named-signal storage plus the five newest decoded receptions in NVS; the separate unnamed latest decoded or raw frame remains RAM-only.
- Optional runtime Wi-Fi station mode with reboot-selected Web, MQTT, or (on any ESP32-S3 profile) simultaneous Web+MQTT operation. Web provides DHCP, collision-aware `.local` discovery, the trusted-LAN UI, and LAN OTA; MQTT provides native Home Assistant MQTT Discovery, learned-signal buttons, RF/automation activity, retained snapshots, and automation controls. RF, storage, automation, and the physical console remain shared by every mode.

## Hardware

CC1101 and low-cost ASK/OOK module connector orders are not standardized. Wire by each module's **printed signal names**, not by physical header position.

The firmware is one codebase with four explicit profiles. Flash the image matching the physical board; images are not interchangeable.

| Profile | Target | Flash / PSRAM | Console | Activity LED | LAN modes |
|---|---|---|---|---|---|
| `esp32-devkit` | ESP32 | 4 MB / none | UART0 GPIO1/3 | GPIO2 active-high | `web`, `mqtt` |
| `esp32s3-devkitc-n16r8` | ESP32-S3 | 16 MB / 8 MB Octal | UART0 GPIO43/44 through USB-UART | Disabled by default; GPIO48 reserved | `web`, `mqtt`, `both` |
| `xiao-esp32s3` | ESP32-S3 | 8 MB / 8 MB Octal | Native USB Serial/JTAG | GPIO21 active-low | `web`, `mqtt`, `both` |
| `esp32s3-supermini-fh4r2` | ESP32-S3FH4R2 | 4 MB / 2 MB Quad | Native USB Serial/JTAG | Disabled; GPIO48 reserved | `web`, `mqtt`, `both` |

The classic ESP32 has no PSRAM and deliberately rejects `service mode both`; it keeps the memory-safe Web-or-MQTT behavior. All three S3 profiles place MQTT's cold catalogs, ledgers, rule snapshots, and discovery scratch in PSRAM while keeping control state, credentials, queues, task stacks, OTA buffers, and RF/RMT state in internal RAM. A missing or failed S3 MQTT allocation leaves Web available when it was requested, and a failed frontend never stops the other frontend in `both` mode.

### Pin map at a glance

The following are the production defaults for each profile. CC1101 pins can be intentionally changed through the owning ESP-IDF configuration, but must remain valid and comply with the selected board's overlap policy. Generic ASK/OOK DATA pins start at these profile defaults and can be changed from Web UI **System > RF hardware**; applying a change reboots the device.

| Profile | CC1101 SCK/MISO/MOSI/CSN/GDO0/GDO2 | Generic TX/RX DATA |
|---|---|---|
| `esp32-devkit` | `18/19/23/27/26/25` | `32/33` |
| `esp32s3-devkitc-n16r8` | `12/13/11/10/4/5` | `13/4` |
| `xiao-esp32s3` | `D8/GPIO7, D9/GPIO8, D10/GPIO9, D3/GPIO4, D1/GPIO2, D0/GPIO1` | `D4/GPIO5, D5/GPIO6` |
| `esp32s3-supermini-fh4r2` | `12/13/11/10/4/5` | `6/7` |

Console, LED, and reserved pins are profile-specific:

| Profile | Console / LED | Unavailable for RF remapping |
|---|---|---|
| `esp32-devkit` | UART0 GPIO1/3; GPIO2 LED | GPIO0-3, GPIO5-12, GPIO15-17; GPIO6-11 are flash-connected |
| `esp32s3-devkitc-n16r8` | UART0 GPIO43/44; LED disabled | GPIO0/3, USB GPIO19/20, flash/PSRAM GPIO26-37, GPIO43-46, GPIO48 |
| `xiao-esp32s3` | Native USB Serial/JTAG; GPIO21 LED | Only exposed GPIO1/2/4-9/43/44 are eligible for intentional RF remapping; GPIO21 remains reserved for the LED |
| `esp32s3-supermini-fh4r2` | Native USB Serial/JTAG; LED disabled | Only exposed GPIO1/2/4-13/43/44 are eligible for intentional RF remapping; GPIO48 remains reserved for the onboard LED |

### CC1101 wiring

Wire by the module's printed signal names, not by physical header position. All VCC/GND connections are 3.3 V only.

Classic ESP32 DevKit/WROOM:

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

ESP32-S3 N16R8 DevKitC-compatible board:

| CC1101 signal | ESP32-S3 GPIO | Direction / purpose |
|---|---:|---|
| `SCK` | GPIO12 | SPI clock |
| `SO` / `GDO1` / `MISO` | GPIO13 | SPI data from CC1101 and `CHIP_RDYn` |
| `SI` / `MOSI` | GPIO11 | SPI data to CC1101 |
| `CSN` | GPIO10 | Manual active-low chip select |
| `GDO0` | GPIO4 | RMT TX into CC1101 asynchronous TX input |
| `GDO2` | GPIO5 | CC1101 asynchronous RX output into RMT RX |

Seeed Studio XIAO ESP32-S3:

| CC1101 signal | XIAO pin / GPIO | Direction / purpose |
|---|---|---|
| `SCK` | D8 / GPIO7 | SPI clock |
| `SO` / `GDO1` / `MISO` | D9 / GPIO8 | SPI data from CC1101 and `CHIP_RDYn` |
| `SI` / `MOSI` | D10 / GPIO9 | SPI data to CC1101 |
| `CSN` | D3 / GPIO4 | Manual active-low chip select |
| `GDO0` | D1 / GPIO2 | RMT TX into CC1101 asynchronous TX input |
| `GDO2` | D0 / GPIO1 | CC1101 asynchronous RX output into RMT RX |

ESP32-S3FH4R2 SuperMini:

| CC1101 signal | SuperMini GPIO | Direction / purpose |
|---|---:|---|
| `SCK` | GPIO12 | SPI clock |
| `SO` / `GDO1` / `MISO` | GPIO13 | SPI data from CC1101 and `CHIP_RDYn` |
| `SI` / `MOSI` | GPIO11 | SPI data to CC1101 |
| `CSN` | GPIO10 | Manual active-low chip select |
| `GDO0` | GPIO4 | RMT TX into CC1101 asynchronous TX input |
| `GDO2` | GPIO5 | CC1101 asynchronous RX output into ESP32 RMT RX |

The XIAO and SuperMini native USB connectors carry the Serial/JTAG console and must remain accessible while the CC1101 is wired. The N16R8 profile expects a USB-UART connection on UART0 GPIO43/44. On the SuperMini, GPIO48 is reserved for its onboard LED and is never driven by this firmware. On ESP32-S3 profiles, do not use GPIO19/20 (USB), GPIO26-37 (flash/PSRAM), strapping pins, or profile-reserved LED pins for CC1101 wiring.

Recommended hardware details:

- Place 100 nF ceramic plus 4.7–10 µF bulk capacitance near the module's VCC/GND pins.
- Add a roughly 10 kΩ pull-up from `CSN` to 3.3 V.
- Keep SPI/GDO wiring short; breadboards and long Dupont wires reduce RF and SPI reliability.
- Use the 433 MHz version of the module with its normal 26 MHz crystal.
- Attach the 433 MHz SMA antenna **before transmitting**.
- Never power or drive the CC1101 at 5 V. Avoid powering one board while the other is unpowered.

### Generic ASK/OOK wiring

Generic mode is for direct digital DATA modules: one 433 MHz ASK/OOK transmitter such as STX882 and one receiver such as SRX882/SRX882S. It does not use SPI. The table lists the profile defaults; valid TX/RX GPIOs can be selected from Web UI **System > RF hardware**. Generic pins may overlap CC1101 pins because only one module set is connected at a time; never connect both module sets simultaneously, and perform all rewiring with power removed.

| Profile | Generic TX DATA | Generic RX DATA |
|---|---:|---:|
| `esp32-devkit` | GPIO32 | GPIO33 |
| `esp32s3-devkitc-n16r8` | GPIO13 | GPIO4 |
| `xiao-esp32s3` | D4 / GPIO5 | D5 / GPIO6 |
| `esp32s3-supermini-fh4r2` | GPIO6 | GPIO7 |

Wire an STX882/SRX882 pair as follows. Use the equivalent labels on another direct-data ASK/OOK pair:

| Module signal | ESP32 connection | Direction / purpose |
|---|---|---|
| STX882 `VCC` | `3V3` | 3.3 V supply |
| STX882 `GND` | `GND` | Common ground |
| STX882 `DATA` | Profile's Generic TX DATA GPIO | ESP32 RMT output to transmitter |
| SRX882 `VCC` | `3V3` | 3.3 V supply |
| SRX882 `GND` | `GND` | Common ground; connect every GND pin provided |
| SRX882 `DATA` | Profile's Generic RX DATA GPIO | Receiver output to ESP32 RMT input |
| SRX882 `CS` / `EN` | `3V3` | Strap active-high for normal operation; do not leave floating |
| Each module `ANT` | Correct 433 MHz antenna | Use the module vendor's antenna connection |

The original NiceRF [STX882 datasheet](https://www.nicerf.com/pdf/stx882-100mw-high-power-ask-transmitter-module-v2.1.pdf) specifies a 1.2–6 V supply and DATA-low sleep, while the [SRX882S datasheet](https://www.nicerf.com/pdf/srx882s-micropower-superheterodyne-receiver-module-v1.0.pdf) specifies 2.0–5.5 V and active-high `CS`. Powering both at 3.3 V therefore keeps their DATA levels directly compatible with ESP32 GPIO. Clone pinouts and electrical limits vary: check the exact module datasheet. If a receiver is powered at 5 V, its DATA output must pass through a proper 5 V-to-3.3 V level shifter or divider before the ESP32. If a 5 V transmitter does not accept a 3.3 V high level, level-shift TX DATA as well. Never apply more than 3.3 V to an ESP32 GPIO.

Place 100 nF ceramic plus 4.7–10 µF bulk capacitance close to each module, keep DATA wiring short, and attach the correct antenna before transmitting. A straight quarter-wave wire for 433.92 MHz is approximately 17.3 cm, but follow the module vendor's antenna/layout guidance. The firmware does not control generic `CS`/`EN`; strap it to its documented active level. Modules without `CS`/`EN` need no extra connection.

Only one backend runs at a time. CC1101 mode releases any generic GPIO that aliases its wiring instead of driving it as an inactive output. In generic mode, the CC1101 is placed idle and its SPI driver is released; Generic TX uses RMT on the selected GPIO and Generic RX uses RMT on the selected GPIO with an explicit internal pull-down so an absent or tri-stated receiver cannot float. RMT is bound only to the active backend, and RX remains half-duplex with TX. The active generic TX DATA pin is driven low again after RMT teardown, including failed or timed-out transmission cleanup. The default backend is `cc1101`; generic GPIO selections persist in a versioned, CRC-protected NVS record tagged to the board profile and apply after reboot.

### RF activity LED

On the classic ESP32, GPIO2 blinks active-high three times during application startup, using 25 ms pulses separated by 150 ms inactive gaps. It then pulses for 25 ms whenever a decoded or raw frame is accepted. The XIAO uses the same behavior on its active-low GPIO21 LED; the N16R8 and SuperMini activity LEDs are disabled by default. RF activity received before the startup sequence completes is coalesced into one normal pulse after the final inactive gap. Many DOIT/clone ESP32 DevKit V1 boards connect a blue LED to GPIO2. The official Espressif ESP32-DevKitC V4 does not: its onboard red LED is a non-programmable 5 V power indicator. On that board, disable the feature or connect an external active-high LED as `GPIO2 -> 220-1000 ohm resistor -> LED anode`, with the LED cathode connected to GND.

GPIO2 is a boot-strapping pin. The firmware does not configure it until application startup, so the three-blink indication does not run during ROM or bootloader execution and external circuitry must not force an incompatible level while the ESP32 resets. `RF_ACTIVITY_LED_ENABLE`, `RF_ACTIVITY_LED_GPIO`, `RF_ACTIVITY_LED_ACTIVE_HIGH`, and `RF_ACTIVITY_LED_PULSE_MS` configure both startup and RF activity pulses. GPIO0, GPIO5, GPIO12, and GPIO15 are rejected because external LED wiring on those strapping pins can prevent boot or select an unsafe flash voltage. The selected output must not overlap UART0, flash/PSRAM, any configured CC1101 pin, or either generic RF DATA pin. An invalid software configuration is nonfatal and never prevents RF reception.

## Build and install

The machine-local ESP-IDF, MCP, coding-agent, and Playwright arrangement is recorded in [Development Tooling Setup](docs/development-tooling-setup.md).

The repository wrapper activates the installed ESP-IDF 6.0.2 environment, selects the target, board defaults, partition table, and target-specific dependency lock, and isolates each profile under `build/<profile>`. Run it from the repository root:

```bash
tools/build-board.sh esp32-devkit build
tools/build-board.sh esp32s3-devkitc-n16r8 build
tools/build-board.sh xiao-esp32s3 build
tools/build-board.sh esp32s3-supermini-fh4r2 build
```

The output application image paths and OTA layouts are:

| Profile | Application image | OTA slot size | OTA slot offsets |
|---|---|---:|---|
| `esp32-devkit` | `build/esp32-devkit/esp32-cc1101.bin` | `0x1e0000` | `0x020000`, `0x200000` |
| `esp32s3-devkitc-n16r8` | `build/esp32s3-devkitc-n16r8/esp32-cc1101.bin` | `0x7e0000` | `0x020000`, `0x800000` |
| `xiao-esp32s3` | `build/xiao-esp32s3/esp32-cc1101.bin` | `0x3e0000` | `0x020000`, `0x400000` |
| `esp32s3-supermini-fh4r2` | `build/esp32s3-supermini-fh4r2/esp32-cc1101.bin` | `0x1e0000` | `0x020000`, `0x200000` |

`tools/build-board.sh` also accepts `size`, `flash`, or `monitor`. Do not use `set-target`, share generated configuration between profiles, edit generated `sdkconfig`, or substitute one profile's image for another.

For a clean, no-flash production build with size and image metadata checks, run:

```bash
tools/verify-production.sh
```

The verifier uses isolated build and log directories under `/tmp`; it never accesses a serial port or flashes hardware.

Framework defaults are in `sdkconfig.defaults`; profile defaults, including production CC1101 pins, are under `boards/<profile>/sdkconfig.defaults`; shared radio options are defined by the owning components' `Kconfig` files. Put deliberate persistent changes in those defaults and rebuild through `tools/build-board.sh`; do not use `menuconfig` as a profile selector or edit generated `sdkconfig`. The verifier enforces at least 25% free space in every OTA slot.

### First wired installation or migration

A blank board, legacy single-app image, or different board profile cannot receive this firmware through OTA. Install the exact profile once over its wired console so the bootloader, profile partition table, application, and `RFBD` descriptor agree.

| Profile | Repository wired workflow |
|---|---|
| `esp32-devkit` | Enabled with its approved persistent by-id path |
| `esp32s3-devkitc-n16r8` | Enabled with its approved persistent by-id path |
| `xiao-esp32s3` | Blocked until a persistent XIAO by-id path is supplied and explicitly approved |
| `esp32s3-supermini-fh4r2` | Enabled with its approved persistent by-id path |

Store exact machine-local mappings in the ignored `.local/board-ports.conf`, initialized from `tools/board-ports.example.conf` as described in [Development Tooling Setup](docs/development-tooling-setup.md#hardware-access-contract). Missing or empty mappings disable wired hardware access. For an enabled profile, use only the validated wrapper and its matching path:

```bash
tools/build-board.sh <profile> flash --port <approved-by-id-path>
tools/build-board.sh <profile> monitor --port <approved-by-id-path>
```

Before migrating a device that already contains learned signals or network settings, back up its NVS partition with the confirmed chip and approved by-id path:

```bash
esptool --chip <esp32-or-esp32s3> --port <approved-by-id-path> \
  read-flash 0x9000 0x6000 nvs-backup.bin
```

The profile partition tables preserve NVS at `0x9000` with size `0x6000`, add `otadata` at `0xf000` and `phy_init` at `0x11000`, and place two application slots at the offsets shown above. Never run `erase-flash` for installation or migration. A legacy downgrade or profile change also requires a wired flash; never use OTA to cross board profiles.

### OTA over the local network

OTA works on all four profiles after an OTA-capable image of that exact profile is installed. Before uploading:

- Connect Wi-Fi and confirm an address with `wifi status`.
- Confirm the effective collision-resolved name with `hostname status`; use its `.local` form or the canonical IPv4 address.
- Run service mode `web` or `both`. MQTT-only mode has no HTTP or OTA server; select `service mode web` (or `both` on an S3), then reset or power-cycle once.
- Run `tools/verify-production.sh` once for a changed production candidate before installing it. Reuse a successful result only while production inputs remain unchanged.
- Build the profile that matches the running board.

Upload the corresponding application image with the validated CLI:

```bash
tools/push-ota.sh <effective-hostname>.local build/esp32-devkit/esp32-cc1101.bin
tools/push-ota.sh <effective-hostname>.local build/esp32s3-devkitc-n16r8/esp32-cc1101.bin
tools/push-ota.sh <effective-hostname>.local build/xiao-esp32s3/esp32-cc1101.bin
tools/push-ota.sh <effective-hostname>.local build/esp32s3-supermini-fh4r2/esp32-cc1101.bin
```

Replace the hostname with the device's IPv4 address when `.local` resolution is unavailable. The uploader validates the image checksum and hash, ESP-IDF/project identity, chip, flash header, embedded `RFBD` profile descriptor, and profile-specific slot size before contacting the device. Its preflight also rejects a busy or pending OTA, a device/image profile mismatch, and an MQTT-only next boot that would make HTTP confirmation impossible.

By default the command waits up to 120 seconds and exits successfully only after the target partition is running with the selected image's full ELF SHA256, ESP-IDF reports the image state as `valid`, and the firmware reports `ESP_OK` for both the state query and confirmation checkpoint. A rollback, identity mismatch, unconfirmed image, or timeout returns nonzero even when the upload itself was accepted. Use `--no-wait` only when acceptance-only behavior is intentional:

```bash
tools/push-ota.sh --no-wait <effective-hostname>.local build/<profile>/esp32-cc1101.bin
```

The same image can be installed from **Web UI > System > Firmware update**: choose **Application image**, select **Install and reboot**, and confirm. Before sending it, the browser inspects the image project, version, full ELF SHA256, and `RFBD` profile and compares the profile with a fresh device snapshot. The device pauses automation and RF safely, writes only the inactive slot, validates it, sends the acceptance response, and reboots. The Web UI does not treat a reconnect alone as success: it checks the target partition, digest, image state, and confirmation result, then reloads with a one-time confirmation message. If the 120-second foreground wait expires, controls are restored while background reconciliation continues.

The Web UI and OTA endpoint are intentionally unauthenticated. Any client on the trusted LAN can install firmware; never expose port 80 to an untrusted network or the Internet. A mismatched profile, oversized image, interrupted upload, or failed validation is rejected without selecting the incomplete slot.

## Serial console

The classic ESP32 and N16R8 profiles use UART0 at 115200 baud; the XIAO and SuperMini profiles use native USB Serial/JTAG. Type `help` to list commands. Use the selected profile and exact approved path from the tooling guide:

```bash
tools/build-board.sh <profile> monitor --port <approved-by-id-path>
```

The wrapper validates the built image and board mapping before starting the monitor. Exit with `Ctrl+]`.

The console starts in the nonpersistent `pretty` style after every reboot. It uses bounded ASCII sections and semantic ANSI colors: green for success/online, cyan for RF receive and information, magenta for TX/automation, yellow for waiting/maintenance/warnings, red for failures, and dim white for metadata. Change or inspect the runtime style with:

```text
console style
console style plain
console style pretty
```

`plain` removes application-console ANSI sequences and preserves stable machine-readable `RX`, `RULE`, and `WIFI` records. ESP-IDF application and bootloader severity logs embed their own ANSI colors, so external log capture must strip those separately when pure text is required. Colored bootloader logs take effect only after an approved wired bootloader flash. The prompt renders as `rf> ` using the native informational color. The repository-owned ESP-IDF 6.0.2 linenoise adapter preserves history, arrows, editing, hints, and completion while coordinating asynchronous application events and native `ESP_LOG` records. An active command is cleared before external output and redrawn afterward with its buffer and cursor intact, including while RF, Wi-Fi, DHCP, OTA, and Web services start. ROM, bootloader, and early application logs before console initialization are unchanged, and local console access is not delayed for optional startup work.

If terminal probing falls back to dumb mode, history and cursor editing remain disabled as in the ESP-IDF fallback. Asynchronous output starts on a fresh line and the prompt plus the unmasked input collected so far is printed again without ANSI cursor sequences; the earlier partial line cannot be erased on such a terminal.

### Select the RF backend

The initial and fallback selection is CC1101. Inspect or change it from the serial console with:

```text
radio hardware
radio hardware cc1101
radio hardware generic
```

A change is applied live; no reboot is required. The switch pauses automation, invalidates frames and actions queued before the switch, stops and idles the old backend, initializes the requested backend, restores receive, and only then commits the choice to NVS. If RF is stopped because the previous startup failed, the same request starts the newly selected backend immediately. If activation or persistence fails, the firmware attempts to restore the previous backend and reports the error. Reapplying the active choice does not rewrite an already-valid matching NVS record; it repairs a missing, corrupt, invalid-version, or mismatched record without restarting RF. Start, stop, maintenance, and hardware switching are serialized, and requests that overlap a transition are rejected.

The same operation is available in the Web UI under **System > RF hardware** using the two-option selector and explicit **Apply** button. MQTT creates a Home Assistant **RF hardware** select entity with `cc1101` and `generic` options. All three interfaces call the same switching transaction, and concurrent switch requests are rejected rather than overlapping.

### Wi-Fi

Wi-Fi is optional and uses station mode with DHCP. With no saved network, the Wi-Fi driver remains off. Configure it from UART:

```text
wifi status
wifi scan
wifi connect <ssid>
wifi start
wifi stop
wifi forget
hostname status
hostname set workshop-bridge
hostname reset
```

`wifi connect <ssid>` prompts for a bounded password without echo and does not place the password in command history. Asynchronous output redraws only the password prompt and never the entered bytes. An open network uses an empty password. Candidate credentials are committed to the versioned, checksummed `net_cfg/station` NVS record only after DHCP succeeds; a failed candidate leaves the previously saved network unchanged. A valid saved network starts connecting asynchronously after normal console and RF startup on later boots. `wifi stop` is temporary, `wifi start` reconnects the saved network, and `wifi forget` deletes the saved record, stops Wi-Fi, and prevents boot reconnection.

The DHCP hostname, and the Web mode's mDNS identity, default to `esp32-cc1101-<last-3-STA-MAC-bytes>`, for example `esp32-cc1101-a1b2c3`. `hostname set <label>` persists a custom 1-32 character label; ASCII letters, digits, and interior hyphens are accepted and letters are stored lowercase. Do not include `.local`. `hostname reset` removes the override and restores the MAC-derived default. Changes apply to an active Web mDNS responder immediately without disconnecting Wi-Fi and become the DHCP hostname on the next Wi-Fi start or reconnect. A malformed hostname NVS record is retained for diagnosis, reported as a persistence error, and bypassed in favor of the safe default.

In Web or Both mode, the responsive UI starts automatically on port `80` when DHCP supplies an address and stops after connectivity is lost. After every HTTP handler is ready, mDNS advertises the Web UI at `http://<hostname>.local/`. If another device already owns the name, the mDNS responder selects a conflict suffix such as `-2`; `hostname status`, the Web System dashboard, and `/api/live` report that effective name. The device remains station-only and does not create a fallback access point. Wi-Fi and hostname configuration remain UART-only.

The stable DNS-SD instance `ESP32 CC1101 RF Bridge <MAC-SUFFIX>` publishes:

- `_http._tcp` on port 80 with `path=/`.
- `_rfbridge._tcp` on port 80 with `txtvers=1`, Web/API/OTA paths, project and running-version identity, supported feature names, and `auth=none`.

Service registration means the records were accepted by the local responder; it does not prove multicast delivery or resolution on every LAN. Client and router mDNS support, multicast filtering, and network isolation still apply. Use the numeric address from `WIFI CONNECTED` when `.local` resolution is unavailable.

Every successful DHCP connection or reconnection emits:

```text
WIFI CONNECTED ssid=<ssid> ip=<ip> netmask=<netmask> gateway=<gateway>
```

Use `wifi status` or the global `status` command for driver, saved-record, DHCP, retry, scan, OTA-lock, persistence-error, and event-drop state. Network failures are nonfatal to RF, storage, automation, and the UART console. Firmware never erases NVS to repair Wi-Fi data; a malformed network record disables only saved-network startup until `wifi forget` or a later successful connection replaces it.

### Network service modes

The persisted service mask is selected at boot and is never changed implicitly by a frontend failure:

- `web` is the default. It starts the HTTP UI/API, mDNS, and LAN OTA.
- `mqtt` starts native Home Assistant MQTT Discovery without HTTP, Web UI, mDNS, or HTTP OTA.
- `both` starts Web and MQTT independently. It is supported by all three S3 profiles; classic ESP32 rejects the command before changing NVS.

Wi-Fi station mode, RF receive/transmit, learned storage, automation, and UART remain available in every mode. In `both`, a Web failure does not stop MQTT and an MQTT failure does not stop Web. MQTT allocation or activation failure in MQTT-only mode still brings up Web for that boot. A persisted `both` record imported on classic falls back to Web for that boot without rewriting the record. Inspect requested, boot, and effective masks, independent Web/MQTT errors, reboot requirement, fallback/retirement state, connection state, outbox use, internal heap, PSRAM, and MQTT/worker stack margins with:

```text
service status
mqtt status
```

Configure a broker and select a service mode from UART:

```text
mqtt configure <broker-ipv4> <username>
# Enter the broker password at the masked prompt.
service mode mqtt
# On an S3, use `service mode both` for simultaneous Web and MQTT.
# Reset or power-cycle once; mode changes are reboot-selected.
```

`mqtt configure <broker-ipv4> <username> [port]` accepts only a numeric unicast IPv4 address; the default port is `1883`. Give the broker a DHCP reservation or static address so the persisted endpoint does not move. Usernames are 1-63 printable ASCII bytes and passwords are 1-127 printable ASCII bytes. The prompt is masked and history-free, and the password is never printed. MQTT uses plaintext 3.1.1: broker credentials and traffic are not encrypted, and the CRC-protected NVS record is not credential encryption. Use it only on a trusted LAN unless flash/NVS encryption and an appropriately isolated network are handled outside this feature.

Mode and configuration changes are persisted but never reboot automatically. `service mode web`, `service mode mqtt`, and `service mode both` take effect after one reset. LAN OTA is available whenever Web is running, including `both`; during Web OTA the MQTT TCP client remains alive while commands and telemetry are paused, then discovery/state/birth reconciliation is forced after maintenance. While MQTT-only mode is active, use wired flashing or select Web and reboot before using `tools/push-ota.sh`. Switching modes preserves the MQTT configuration and retained Home Assistant entities.

Each committed learned signal becomes one Home Assistant MQTT button. Discovery uses the default `homeassistant` prefix, the full lowercase Wi-Fi STA MAC, and these topics:

```text
homeassistant/button/rfbridge_<12hex>/<signal>/config
homeassistant/select/rfbridge_<12hex>/radio_hardware/config
rfbridge/<12hex>/signal/<signal>/press
rfbridge/<12hex>/radio/hardware/set
rfbridge/<12hex>/availability
homeassistant/status
```

Discovery and availability are retained at QoS 1. Every discovery entity keeps the stable bridge identifier and name, reports `RF Bridge` as its manufacturer, uses the running board's human-readable model, and exposes the exact profile slug as its hardware version. Button commands are exact, non-retained QoS 0 `PRESS` messages so broker redelivery cannot cause a second RF action. A valid command replays the named signal using `CONFIG_RF_DEFAULT_TX_REPEATS` (default 8). Retained, duplicate, fragmented, malformed, wrong-topic, and wrong-QoS commands are rejected. Learning, manually saving, or deleting a signal reconciles discovery automatically; a 60-second audit and `homeassistant/status` birth messages recover dropped events and Home Assistant restarts.

For a dedicated Mosquitto user, replace `<12hex>` with the bridge's full lowercase STA MAC and grant only the bridge-side topics:

```text
user rfbridge_<12hex>
topic read homeassistant/status
topic read rfbridge/<12hex>/signal/+/press
topic read rfbridge/<12hex>/automation/+/set
topic read rfbridge/<12hex>/radio/hardware/set
topic write rfbridge/<12hex>/availability
topic write rfbridge/<12hex>/event/#
topic write rfbridge/<12hex>/state/#
topic write homeassistant/+/rfbridge_<12hex>/#
```

Home Assistant needs its own normal broker permissions to read discovery/availability and publish commands. Ensure MQTT Discovery is enabled with the `homeassistant` prefix.

The MQTT profile also mirrors the shared RF and automation event stream. The physical UART remains the same console used by the Web profile: `rule list`, `rule add`, `rule remove`, `rule enable`, `rule disable`, and `rule log` operate on the same NVS records and use the same asynchronous RX and automation log path. At console startup, the `[STORE]` line reports learned-signal count, rule count, enabled state, log mode, and the NVS initialization result. Use `mqtt status` to inspect Wi-Fi startup gating, signal/rule discovery counts, retained-state failures, event publication/drop counters, heap margins, and both task stack margins.

The following bounded topics are published for Home Assistant and other LAN consumers:

```text
rfbridge/<12hex>/event/rx
rfbridge/<12hex>/event/automation
rfbridge/<12hex>/state/automation
rfbridge/<12hex>/state/last_rx
rfbridge/<12hex>/state/last_automation
rfbridge/<12hex>/state/rule/<trigger>
rfbridge/<12hex>/state/system
rfbridge/<12hex>/automation/enabled/set
rfbridge/<12hex>/automation/log_mode/set
rfbridge/<12hex>/radio/hardware/set
```

`event/rx` and `event/automation` are non-retained QoS 0 JSON event messages. They contain sequence numbers, bounded decoded metadata or raw pulse counts, matching information, rule/action names, and result counters; complete raw pulse arrays are never sent. Home Assistant Event entities expose these messages for automations, and Home Assistant Recorder is the durable event history. `state/automation`, `state/last_rx`, `state/last_automation`, `state/system`, and each `state/rule/<trigger>` snapshot are retained QoS 1 so the latest configuration, counters, and result are available after a bridge or broker restart. Broker persistence must be enabled if retained snapshots must survive a broker restart.

`state/system` contains the active RF hardware; board profile, target, flash and PSRAM sizes; requested/effective services and reboot requirement; uptime; and total, free, minimum-free, and largest-block values for internal RAM and PSRAM. It is republished after a hardware switch, during connection reconciliation, Home Assistant birth recovery, OTA-maintenance recovery, and the 60-second audit.

Discovery additionally creates RF activity and automation activity Event entities, an RF hardware select, an automation enabled switch, an automation log-mode select (`off`, `actions`, or `verbose`), a rule-count diagnostic sensor, an event-drop diagnostic sensor, and one diagnostic sensor for each persisted rule. Four `data_size` diagnostic sensors use `state/system`: internal free, internal minimum free, largest internal block, and PSRAM free. The first three exist on every board; PSRAM free is advertised only on PSRAM-equipped S3 profiles. All four use bytes and expire after 180 seconds without a fresh snapshot. Hardware commands accept only exact `cc1101` or `generic`; automation commands accept only exact `ON`/`OFF` or `off`/`actions`/`verbose`. Every command must be non-retained, non-duplicate, unfragmented QoS 0. Successful switches publish the new state; failed switches publish a reconciliation snapshot for the backend restored by rollback, so Home Assistant does not retain a requested-but-inactive value. Rule CRUD intentionally remains a UART operation so all profiles share one bounded administrative interface.

`mqtt forget` is a durable retirement transaction. While MQTT is connected it records `retiring`, tombstones every discovery, state, rule, and availability topic with acknowledgements, clears the ledger and credentials, then records Web/retired and stops MQTT. In `both`, Web stays available throughout and no reset is needed after completion; in MQTT-only mode, reset once after retirement to start Web. Power loss at any step resumes the retirement path on the next boot. If Web is active while a retained ledger exists, select MQTT, reboot, wait for connection, and retry `mqtt forget`. Broker IP/port changes are blocked until old retained entities are retired; credential changes for the same endpoint are allowed and take effect after reboot.

The retained ledger keeps the broker endpoint even when the signal and rule lists are empty. This marker covers the fixed activity/control entities, so they can still be tombstoned during `mqtt forget` and an endpoint change cannot strand stale retained discovery.

### Web UI

The Web UI is available whenever the boot mask includes Web (`web` or `both`). It is a compact static HTML/CSS/JavaScript application served on port `80`. The first visible browser uses one bounded Server-Sent Events stream for low-latency notifications, while `/api/live` remains the authoritative snapshot. Unsupported browsers, additional tabs, rejected streams, and disconnected clients automatically use the same visibility-aware polling fallback. The Overview tab keeps the latest 50 decoded or raw observations in browser tab-scoped session storage, so that activity list survives page refreshes. Clear or closing the tab session removes the browser-local list. It is not shared with other browsers, does not follow a changed device IP, and is not a guaranteed event history.

The separate **Signals > Recent decoded signals** list comes from the device, not the browser. It contains the five newest decoded receptions committed to NVS, survives browser and bridge restarts, and is shared by every Web client and the UART `recent` command. Each entry has a persistent ID and controls for bounded replay or copying it into the named learned-signal catalog. Raw captures never enter this five-entry list.

The Web surface calls typed services directly and provides:

- A health-first System dashboard with board model/profile/target, flash and PSRAM,
  console transport and both backend pin assignments, active RF backend, live CC1101 identity/state, RF configuration
  and counters, Wi-Fi addressing and retry diagnostics, uptime/reset reason, internal
  and PSRAM watermarks, and learning/automation/LAN service health.
- Explicit live selection of CC1101 or generic ASK/OOK hardware.
- Learning and cancellation.
- Structured manual saving of decoded signals without receiving or transmitting them.
- Latest-frame and named learned-signal replay with bounded repeats.
- Persistent recent-decoded listing, replay, save-as-learned, refresh, and confirmed clear.
- Learned-signal metadata and deletion with rule-reference protection.
- Decoded and raw RF transmission forms.
- Automation rule add/remove/enable/disable and log-mode controls.
- OTA status and direct application-image upload with progress and reboot recovery.

The System dashboard includes RF backend selection, Generic TX/RX GPIO configuration, and firmware upload. Generic GPIO changes are saved and applied after an automatic reboot; the page pauses polling during the restart and confirms the new active pair after reconnect. Hardware, runtime/memory, and services share three desktop columns and collapse to one below 820 px; detailed hardware and service data remain in expandable disclosures. Internal RAM determines memory health. Free or largest-block values below 12 KiB are critical, free memory below 24 KiB is degraded, and largest blocks below 16 KiB on classic ESP32 or 32 KiB on a combined-service S3 are reported as fragmented. PSRAM is explicitly shown as not installed on classic ESP32. Hard current failures are red; cumulative runtime drops, timeouts, recoveries, and transmission/log errors remain amber until reset. A temporarily unavailable CC1101 status sample during RF transmission or maintenance is shown as paused rather than faulted; CC1101 diagnostics are inactive, not failed, while generic RF is selected. If a running browser reconnects to older firmware after rollback, missing diagnostics are shown as limited while the existing live data remains connected. A transport failure retains the last detailed snapshot in a muted state until polling reconnects.

The receiver has no user-controlled off state. RX is always the desired state and automatically resumes after the bounded half-duplex pauses required by transmission, radio reset, and OTA maintenance. Wi-Fi credentials and lifecycle, radio recovery, console settings, and generic UART command execution remain UART-only.

The Web UI intentionally has no authentication. Any client already on the local network can read status, transmit RF, change persistent automation, delete learned signals, or install firmware. Requests accept only the exact current IPv4 address, configured `.local` name, or currently cached conflict-resolved `.local` name. Mutations additionally require an exact same-origin match to the Host representation used by that request; valid names cannot be mixed. These checks reduce browser cross-origin abuse but are not authentication. Do not expose port `80` to an untrusted network or the Internet.

The obsolete `web auth status` and `web auth rotate` UART commands have been removed. Any legacy authentication record in NVS is left untouched and unused.

The OTA endpoint uses the existing inactive-partition writer, chip/project/image validation, maintenance locks, rollback support, and running-image confirmation. The complete per-profile build, wired-installation, CLI upload, Web upload, and first-install migration workflow is documented in [Build and install](#build-and-install) above. The Web UI is intentionally unauthenticated trusted-LAN firmware administration; keep port 80 isolated.

### Receive

RX starts automatically. Known frames are printed as:

```text
RX learned=gate RC code=11043138 hex=0xA88142 bits=24 protocol=1 pulse_us=386 confidence=repeated repeats=3 fingerprint=0x...
```

`learned=<name>` appears when exactly one committed learned signal is canonically equivalent. Ambiguous matches are reported with `learned_ambiguous=<count>` instead of selecting an arbitrary name. The same annotation appears in `last`, while the Web event console and timeline render the unique name as a learned badge.

Unknown stable repeated frames are printed in copyable form:

```text
RX RAW start=1 count=20 repeats=3 fingerprint=0x... durations=210,740,330,...
```

Repeated observations from one hold are suppressed for 350 ms by default. The activity LED pulses once per accepted logical `RX` event after this duplicate suppression, so malformed noise and repeated packets from the same button hold do not cause extra flashes. Accepted frames arriving within the 25 ms LED pulse extend it to 25 ms after the newest frame. A single complete decoded frame is accepted only when its capture boundary is trusted and is marked `confidence=single`. Raw fallback requires at least two aligned periods and is never accepted from a truncation-suspected capture. A final delimiter stopped by the 30 ms RMT threshold is treated as censored timing evidence rather than an exact duration.

Reception is always enabled whenever the radio is running. Transmission and maintenance temporarily pause capture and restore it automatically.

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

The five newest decoded receptions are also stored automatically in NVS. List them newest first, replay one by its persistent ID, or copy one into the named learned catalog:

```text
recent list
recent replay 42
recent replay 42 10
recent save 42 gate
recent clear
```

Recent-history rules:

- Only accepted decoded frames that print a readable `code=` are stored. Raw captures are ignored.
- Every logical reception is retained, including a later reception of the same code. The normal 350 ms button-hold suppression runs before history capture.
- The list is fixed at five entries. Adding a sixth atomically evicts the oldest committed entry.
- IDs are nonzero 64-bit decimal values stored with the history and remain stable across reboot. A rotated or cleared ID returns not found and is never redirected to another entry.
- `recent replay <id> [repeats]` uses the configured default when repeats are omitted; the explicit range is 1-20.
- `recent save <id> <name>` copies the decoded payload without removing the history entry. Existing learned names are never overwritten, and normal name validation and catalog-change reporting apply.
- `recent clear` immediately empties the list while preserving the next valid ID. The Web UI requires confirmation before issuing the same mutation.
- History uses its own versioned, checksummed NVS record. A corrupt record fails closed until explicitly cleared. Initialization or commit failures are reported but never stop receive, learning, automation, or RAM replay.

Save a decoded signal directly without receiving or transmitting it:

```text
save gate 13830801 24 1 199
save gate_default 0xD30A91 24 1
```

The same operation is available under **Signals > Learned signals** in the Web UI. Enter a persistent name, code, bit count, protocol, and pulse width, then select **Save signal**. The Web form uses `0` for the selected protocol's nominal pulse width.

Manual-save rules:

- `save <name> <code> <bits> <protocol> [pulse_us]` accepts decimal or `0x` hexadecimal codes. Bits must be 4-64, the code must fit, and the protocol must be 1-12.
- Omitted or zero `pulse_us` uses the selected protocol's nominal timing. A nonzero value must fit that protocol's transport limits.
- The name uses the normal learned-signal rules: 1-15 characters, starts with a letter, and then contains only letters, digits, `_`, or `-`; `list` is reserved.
- Saving is create-only. An existing name is never overwritten, and a failed save does not change its record.
- Confidence, observed repeat count, and fingerprint text from an `RX` line are reception metadata and are not stored. Home Assistant replay continues to use `CONFIG_RF_DEFAULT_TX_REPEATS`.
- A committed manual signal appears in the existing learned list and automation selectors. When MQTT is configured, the catalog-change event creates its Home Assistant button immediately when connected or during the next normal reconciliation.
- Manual saving does not transmit RF, replace the latest RAM frame, change learning state, or add an entry to recent history.

Generate and save a random decoded signal without receiving or transmitting it:

```text
random
```

The Web equivalent is **Signals > Learned signals > Generate & save**. The firmware takes 24 bits from the ESP hardware RNG, uses rc-switch protocol 1 at its nominal 350 us pulse width, and creates a name such as `random_D30A91`. Existing equivalent signals and names are never overwritten; generation retries with a fresh value up to eight times. The committed result enters the normal learned list and automation selectors and becomes a Home Assistant replay button when MQTT discovery is active.

Generation works with Wi-Fi running or stopped. ESP-IDF classifies RNG output without an active RF or SAR entropy source as pseudo-random, which is sufficient for selecting a collision-checked fixed ASK/OOK code but is not a cryptographic security mechanism. Generation alone never transmits RF, regardless of whether CC1101 or the generic ASK/OOK backend is selected.

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

### Automate learned signals

Create a persistent rule that transmits one learned signal when another is received:

```text
rule add motore_on motore_off 8
rule list
rule log
rule log verbose
rule disable
rule enable
rule remove motore_on
```

Rule behavior:

- `rule add <received_name> <transmit_name> [repeats]` requires two valid learned names. Omitted repeats use the configured default; the valid range is 1–20.
- One trigger name has at most one action and cannot be overwritten. Remove its rule before adding a replacement. Any number of different triggers may replay the same target.
- Rules, repeat counts, the fixed 1000 ms cooldown, and global enable/disable state persist across reboot. Runtime cooldown timestamps restart at boot and successful or failed action attempts are spaced by monotonic action time.
- Decoded and raw learned signals use the same canonical matching as receive duplicate detection. Equivalent duplicate triggers, self-rules, ambiguous target aliases, and directed cycles are rejected. If tolerance boundaries make one received raw frame match multiple otherwise-distinct triggers, no action fires and the ambiguity counter increases.
- An incoming frame fires at most one action. The same rule cannot fire again until its 1000 ms cooldown expires. Frames queued before add/remove/enable/disable are discarded by configuration generation. RX is temporarily paused during the bounded transmission, then restored automatically.
- `forget <name>` fails while the learned name is referenced as a trigger or target. Remove all referencing rules first.
- Automation startup failures never erase NVS or stop ordinary receive/manual replay. Learned-code load/list/replay remains available if only the new automation namespaces lack capacity. Automation fails closed if persisted rules are corrupt, dangling, cyclic, equivalent, ambiguous, or exceed the configured bounded table (default and maximum 32).
- Automation activity logging is persistent and defaults to `actions`. `rule log off` disables it, `rule log actions` prints trigger and TX completion/error lines, and `rule log verbose` additionally prints cooldown suppressions, ambiguous/stale skips, and automation frame-queue drop summaries. Unmatched frames are never logged.
- Automation logs use a dedicated bounded queue and the existing console output worker. Log records never consume frame/learn queue slots; a full log queue drops only that log record, increments `log_drops`, and never blocks RF transmission or suppresses an action. Like asynchronous `RX` output, accepted log output shares the UART worker and temporarily clears and redraws an active prompt without changing its input; normal linenoise history, arrows, editing, and tab completion remain enabled.
- Use `status` for action, stale-frame, ambiguity, cooldown, queue, TX, log-event, and log-drop counters plus the last result. `rule list` shows persisted configuration and marks per-record or graph-level startup failures. If runtime startup fails but the relevant NVS namespace remains readable, `rule list`, `rule remove`, `rule enable`, `rule disable`, and `rule log <mode>` remain administrative; successful changes report that a reboot is required before automation can retry. Unreadable values are reported with `enabled_known=0`, `rules_known=0`, or `log_mode_known=0` rather than as authoritative defaults.

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
- Durations are 100–29000 µs. This keeps pulses above the supported asynchronous sampling floor and below the 30 ms RMT frame-stop threshold.
- A frame must contain an even 8–256 alternating pulses.
- The staged frame and unnamed last received frame are not persistent. Named learning stores decoded or raw frames, while the automatic five-entry recent history stores decoded frames only.

### Diagnostics and recovery

```text
status
board status
memory status
radio info
wifi status
radio hardware
radio hardware generic
radio reset
radio start
```

`status` renders the System/RF, Automation, Wi-Fi, board, service, and memory dashboards. `board status` reports the running profile, target, flash/PSRAM, console transport, CC1101 wiring, generic DATA GPIOs, and combined-service capability. `memory status` reports internal and PSRAM total/free/minimum/largest-block values. `radio info` and `wifi status` render only their owning subsystem; in plain style they emit only the corresponding stable record.

Diagnostics include the active backend, generic TX/RX GPIOs, last switch error and successful switch count. In CC1101 mode they also include PARTNUM/VERSION, stable MARCSTATE, RSSI in half-dBm units (`rssi_x2`), carrier/CCA bits, and resets/recoveries/timeouts. Generic modules have no SPI control or diagnostic channel, so carrier frequency, bandwidth, AGC behavior, TX power, RSSI, CCA, identity, reset, and recovery are module-defined or unavailable. Both modes report RMT queue drops/truncations, duplicate count, whether desired RX is actually armed, and automation availability/enabled/logging state, rules, stale or ambiguous frames, matches, actions, cooldown suppressions, queue drops, TX errors, emitted log events, dropped log events, and last result. Software status remains available while RF is stopped or busy; hardware fields are explicitly marked unavailable when they cannot be sampled safely.

`radio reset` temporarily disables capture and restores the always-on RX state. In CC1101 mode it also performs the chip reset/profile/readback/calibration sequence; in generic mode it resets only the RMT receive lifecycle. `radio start` retries complete initialization after wiring or power is corrected. Boot also makes three bounded startup attempts. All command, SPI-ready, and radio-state waits are bounded. An unrecoverable classic-ESP32 RMT TX timeout attempts to idle the active backend and leaves the service faulted; reboot is then required rather than risking a late transmission or an unbounded driver abort.

## CC1101 radio profile

The default CC1101 profile assumes a 26 MHz crystal:

- Center frequency: 433.920 MHz (register word `0x10B071`, approximately 433.91983 MHz).
- Transparent asynchronous serial mode; packet FIFO, CRC, whitening, and sync detection are bypassed.
- ASK/OOK modulation, approximately 4.8 kbaud demodulator rate and 203 kHz RX bandwidth.
- GDO2 provides raw demodulated RX data; GDO0 is the asynchronous TX-data input.
- OOK AGC baseline `AGCCTRL2/1/0 = 04/00/92`.
- Nominal default TX power: +5 dBm using TI's 433 MHz PATABLE guidance.

In generic mode, the firmware supplies only the baseband DATA waveform. The transmitter module determines carrier frequency and output power; the receiver determines bandwidth, AGC, sensitivity, polarity, and noise behavior. Use 433.92 MHz ASK/OOK modules whose DATA polarity is active-high. Generic-module matching networks, oscillators, antennas, and local regulations vary, so actual output power and usable range require measurement on the real hardware.

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
- Sharing `SPI3_HOST` with another component while CC1101 is selected. The CC1101 driver owns that host exclusively so its reset and transaction deadlines remain deterministic.
- Simultaneous RX and TX; either backend is operated half-duplex.

Do not attempt to clone security or access-control devices. Follow local 433 MHz frequency, power, bandwidth, duty-cycle, antenna-gain, and listen-before-talk rules.

## Tests

Run portable codec/parser regressions from a clean native shell (outside an activated ESP-IDF cross-toolchain environment):

```bash
cmake -S host_tests -B /tmp/esp32-cc1101-host-tests
cmake --build /tmp/esp32-cc1101-host-tests
ctest --test-dir /tmp/esp32-cc1101-host-tests --output-on-failure
```

Build the dedicated Unity image for each target with isolated configuration:

```bash
# With ESP-IDF 6.0.2 already activated:
idf.py -C test_apps/unit -B /tmp/esp32-cc1101-unit-esp32 \
  -DIDF_TARGET=esp32 \
  -DSDKCONFIG=/tmp/esp32-cc1101-unit-esp32/sdkconfig \
  -DSDKCONFIG_DEFAULTS="$PWD/test_apps/unit/sdkconfig.defaults;$PWD/test_apps/unit/sdkconfig.defaults.esp32" \
  -DDEPENDENCIES_LOCK="$PWD/test_apps/unit/dependencies.lock.esp32" build
idf.py -C test_apps/unit -B /tmp/esp32-cc1101-unit-esp32s3 \
  -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=/tmp/esp32-cc1101-unit-esp32s3/sdkconfig \
  -DSDKCONFIG_DEFAULTS="$PWD/test_apps/unit/sdkconfig.defaults;$PWD/test_apps/unit/sdkconfig.defaults.esp32s3" \
  -DDEPENDENCIES_LOCK="$PWD/test_apps/unit/dependencies.lock.esp32s3" build
```

The Unity image is compile-only in this workflow. Do not flash or run on-device Unity tests without separate approval and a board-specific approved by-id path. Production firmware hardware access is authorized for the classic ESP32, N16R8, and FH4R2 paths documented above; this does not authorize flashing the Unity image.

The Unity image uses an interactive menu; enter `*` and press Enter to run all tests. Building it does not mean its tests passed, only the on-device `0 Failures` summary does. The host and Unity sources cover the hardened decoder vectors, raw and learned matching, ambiguity and learning-window bounds, recent-history rotation/IDs/corruption, parser bounds, console style/ANSI bounds, bounded Web form parsing, same-origin checks, HTML escaping, server-rendered route contracts, versioned RF, Wi-Fi, and MQTT records, MQTT topics/discovery/command rejection, owned-key NVS repair, OTA compatibility policy, frequency calculation, and PA selection. MQTT broker behavior, CC1101 SPI/RMT lifecycle, NVS persistence across reboot, DHCP/reconnection, browser behavior on a device, RF timing, range, and recovery still require explicit hardware testing; compilation alone proves none of those behaviors.
