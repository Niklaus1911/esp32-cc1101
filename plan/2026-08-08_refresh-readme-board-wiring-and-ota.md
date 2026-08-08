# Refresh Board Wiring and OTA Documentation

## Summary

Rewrite the fragmented hardware and installation portions of `README.md` into an accurate, scan-friendly reference for all four board profiles. Synchronize machine-specific details in `docs/development-tooling-setup.md` and add a host documentation contract so GPIO maps, image paths, OTA slot sizes, profiles, and hardware-access constraints cannot silently drift.

No firmware behavior, GPIO assignment, partition layout, or OTA API will change.

## Documentation Changes

- Replace the separate, repetitive wiring sections with capability, CC1101, generic RF, and board-reservation tables using these current defaults:

| Profile | CC1101 GPIOs: SCK/MISO/MOSI/CSN/GDO0/GDO2 | Generic TX/RX | Console | Activity LED |
|---|---|---|---|---|
| `esp32-devkit` | `18/19/23/27/26/25` | `32/33` | UART0 `1/3` | GPIO2 active-high |
| `esp32s3-devkitc-n16r8` | `12/13/11/10/4/5` | `6/7` | UART0 `43/44` | Disabled; GPIO48 reserved |
| `xiao-esp32s3` | `7/8/9/4/2/1` | `5/6` | Native USB Serial/JTAG | GPIO21 active-low |
| `esp32s3-supermini-fh4r2` | `12/13/11/10/4/5` | `6/7` | Native USB Serial/JTAG | Disabled; GPIO48 reserved |

- Include XIAO `D` aliases, GPIO direction, common VCC/GND wiring, CC1101 and STX882/SRX882 connection instructions, reserved or unsafe GPIOs, 3.3 V limits, receiver level shifting, decoupling, antenna requirements, and the one-active-backend rule.
- Clearly distinguish configurable CC1101 defaults from the fixed generic DATA GPIO assignments.
- Correct stale overview and command claims, including persistent five-signal history, manual saving, all four profiles, RF backend selection through UART/Web/MQTT, and the misleading sentence that currently names only two authorized boards while listing three.

## Build, Wired Installation, and OTA

- Reorganize the current Build and migration text into one ordered workflow: select profile, build, perform the initial wired installation when necessary, configure Wi-Fi/Web services, then use OTA.
- Add an all-profile image matrix:

| Profile | Application image | OTA slot |
|---|---|---:|
| `esp32-devkit` | `build/esp32-devkit/esp32-cc1101.bin` | `0x1e0000` |
| `esp32s3-devkitc-n16r8` | `build/esp32s3-devkitc-n16r8/esp32-cc1101.bin` | `0x7e0000` |
| `xiao-esp32s3` | `build/xiao-esp32s3/esp32-cc1101.bin` | `0x3e0000` |
| `esp32s3-supermini-fh4r2` | `build/esp32s3-supermini-fh4r2/esp32-cc1101.bin` | `0x1e0000` |

- Provide expanded build and `tools/push-ota.sh <effective-hostname>.local <profile-image>` examples for every profile, plus the IPv4 fallback.
- Document both upload interfaces: CLI and `Web UI -> System -> Firmware update -> Application image -> Install and reboot`.
- State prerequisites explicitly: an exact-profile OTA-capable image must already be installed, Wi-Fi must be online, effective service mode must be `web` or `both`, and MQTT-only mode requires selecting Web and rebooting first.
- Explain that the first installation or legacy partition migration is wired, never erases NVS, preserves `0x9000..0xefff`, and must use the repository wrapper and the exact approved by-id mapping from the tooling guide.
- Document classic ESP32, N16R8, and FH4R2 as having approved local wired paths. Document XIAO OTA as supported after installation, while its first wired flash remains blocked by the wrapper until the user supplies and approves a persistent by-id path.
- Cover profile mismatch rejection through the `RFBD` descriptor, slot-size validation, trusted-LAN/no-auth risk, reboot/reconnect behavior, and legacy downgrade requiring wired flashing.
- Keep exact machine-local by-id paths authoritative in `docs/development-tooling-setup.md`; README will link there instead of duplicating them.

## Drift Protection and Verification

- Add `host_tests/test_documentation.mjs` and register it as `documentation_contract_tests` in the native CTest suite.
- Assert that all four profiles, CC1101 pins, generic pins, console/LED facts, image paths, OTA slot sizes, supported `push-ota.sh` profiles, and wired-access statuses agree across board policy, board defaults, partition tables, scripts, README, and the tooling guide.
- Assert that README contains the first-install, Web/Both-mode, CLI/Web upload, no-erase, same-profile, XIAO limitation, and trusted-LAN warnings.
- Run `git diff --check`, the documentation contract directly, and the complete six-plus-one native host suite.
- Manually inspect rendered Markdown for readable tables, command wrapping, working internal links, and mobile-friendly scanning.
- Skip ESP-IDF builds, production verification, flashing, monitoring, and live OTA because the change affects only Markdown and host-only documentation contracts.

## Assumptions

- Existing profile GPIOs, OTA layouts, and hardware-access mappings are correct and remain unchanged.
- README remains the primary user hardware and operational guide; the tooling document remains authoritative for this machine's exact serial paths.
- Historical plans, firmware sources, Web assets, and helper-script behavior are out of scope.
- No board diagrams or screenshots will be added; tables and concise wiring instructions are the selected presentation.
