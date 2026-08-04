# Repository Guidelines

## Project Map

- `main/`: application startup and subsystem orchestration.
- `components/cc1101/`: CC1101 SPI driver and radio configuration.
- `components/rf_codec/`: rc-switch-compatible decode/encode and raw identity logic.
- `components/rf_ook/`: ESP-IDF RMT receive/transmit lifecycle and timing bounds.
- `components/rf_storage/`: versioned NVS signal and automation-rule storage.
- `components/rf_automation/`: receive-to-replay rules, queues, cooldowns, and event reporting.
- `components/rf_console/`: UART commands and bounded command parsing.
- `components/bridge_control/`, `components/bridge_events/`: shared typed control APIs and bounded cross-subsystem event delivery.
- `components/app_maintenance/`: transactional RF, automation, and Wi-Fi maintenance coordination for OTA.
- `components/network_wifi/`, `components/ota_update/`: station lifecycle and validated streaming OTA.
- `components/rf_signals/`: learned-signal matching, learning state, replay ownership, and snapshots.
- `components/platform_nvs/`: shared NVS initialization and availability state.
- `components/web_ui/`: responsive assets, typed HTTP API, polling, request validation, and shared HTTP server ownership.
- `components/web_auth/`: legacy token storage and UART token administration; the current trusted-LAN Web UI is unauthenticated.
- `host_tests/`: portable C++17 codec/parser/storage/automation tests.
- `test_apps/unit/`: dedicated ESP-IDF Unity image. Building it does not run its on-device tests.
- `README.md`: authoritative hardware, command, RF, and safety documentation.

## Environment and Commands

Run commands from the repository root. Start Codex CLI from this directory so project instructions and tool working directories resolve consistently.

Run host tests from a clean native shell before activating the ESP-IDF cross-toolchain:

```bash
cmake -S host_tests -B /tmp/esp32-cc1101-host-tests
cmake --build /tmp/esp32-cc1101-host-tests
ctest --test-dir /tmp/esp32-cc1101-host-tests --output-on-failure
```

The production verifier activates the installed ESP-IDF 6.0.2 environment itself:

```bash
tools/verify-production.sh
```

Build the dedicated Unity image separately when component behavior changes. In fish, source `"$HOME/.espressif/v6.0.2/esp-idf/export.fish"` instead; Bash automation uses the stable checkout export shown here:

```bash
source "$HOME/.espressif/v6.0.2/esp-idf/export.sh"
cd test_apps/unit
idf.py -B build build
```

The production verifier runs a clean out-of-tree firmware build, size report, and image inspection under `/tmp`. It does not access hardware. `idf.py build` is the authoritative compiler gate.

## Diagnostics

Generic and Espressif clangd can report false ESP-IDF cross-toolchain diagnostics for this project. Treat editor and language-server diagnostics as non-authoritative hints, especially around callbacks, function pointers, FreeRTOS tasks, and hardware entry points. `idf.py build` is the firmware compilation gate.

Use repository search and focused file reads for exploration. Inspect the relevant diff after edits and keep changes scoped to the owning components.

## External Research

Use targeted Web search whenever it can materially improve a design or debugging decision, especially for version-specific ESP-IDF behavior, upstream defects, hardware constraints, or established implementation patterns that local source and diagnostics do not fully resolve. Prefer authoritative Espressif documentation, upstream source, release notes, and issue trackers; verify that findings apply to ESP-IDF 6.0.2 and classic ESP32, and do not substitute Web advice for the production build or hardware validation gates.

## Espressif MCP

The active Codex CLI setup provides the Espressif Documentation, ESP-IDF Tools, and Playwright MCP servers. Start Codex from the repository root so the ESP-IDF Tools server operates on this project. Use the Espressif Documentation MCP before generic Web search for ESP-IDF APIs, version-specific behavior, hardware constraints, release notes, and official examples; confirm results apply to ESP-IDF 6.0.2 and classic ESP32. Use broader Web research for upstream defects and implementation patterns not resolved by official material.

Use the ESP-IDF Tools MCP `build_project` operation as a fast iterative compiler check. It writes the normal in-tree build output and does not replace host tests, Unity compilation, or `tools/verify-production.sh`; the production verifier remains the final clean build, size, image, and partition gate. MCP success does not prove RF timing, range, recovery, NVS persistence, browser behavior, or hardware behavior.

Do not call `set_target` without approval because it can regenerate project configuration. Do not call `clean_project` without approval because it removes build artifacts. Never call `flash_project` without explicit approval and a confirmed port and board. An MCP startup failure is not permission to modify, reinstall, repair, or delete anything under `~/.espressif`; fall back to the documented shell commands and report the failure.

## Browser Validation

Use the Playwright MCP server for Web UI behavior changes. Validate against a deterministic local mock first, then against the device when hardware access is authorized. Cover desktop and mobile layouts, horizontal overflow, console and page errors, polling behavior, exact same-origin mutation headers, and reconnect flows such as OTA reboot recovery.

Intercept RF transmit and destructive mutation requests unless the user explicitly authorizes those hardware effects. Browser validation complements rather than replaces host tests, the production build, and hardware validation.

## Code Conventions

- Keep the firmware native ESP-IDF C/C++; do not introduce Arduino APIs.
- Use four spaces and follow nearby brace/layout style.
- Use `snake_case` for functions and locals, `PascalCase` for types, `kPascalCase` for constants and enum values, and `s_snake_case` for file-static state.
- Keep public C++ APIs in namespace `rfbridge`; use anonymous namespaces for file-private implementation.
- Prefer fixed-width integers, `std::size_t`, value initialization, `nullptr`, explicit bounds checks, and `esp_err_t` propagation.
- Keep callbacks and ISRs short. Preserve queue ownership, synchronization, cooldown, and RMT RX/TX lifecycle invariants.
- Add focused host or Unity regression tests for behavior changes.

## Safety and Generated Files

- Never flash, erase, open a serial monitor, or run on-device Unity tests without explicit approval and a confirmed port/board.
- Do not run `menuconfig`, `set-target`, or `reconfigure`, or edit generated `sdkconfig`, without approval. Put approved persistent defaults in `sdkconfig.defaults`.
- Do not edit `managed_components/` or modify, reinstall, repair, or delete anything under `~/.espressif`.
- Preserve the documented 3.3 V-only CC1101 wiring, SPI3 ownership, RMT allocation, half-duplex behavior, antenna requirement, and local RF regulations.
- Compilation does not prove RF timing, range, recovery, NVS persistence, or hardware behavior. Report hardware validation only when it was actually performed.

## Plan Persistence

- Whenever a final plan is proposed, automatically save its Markdown body, without client rendering tags, under `plan/` using `YYYY-MM-DD_<descriptive-kebab-case-slug>.md`.
- Revisions of the same plan must update the original file in place and retain its filename. Distinct plans receive distinct files; append `-2`, `-3`, and so on when a filename already belongs to another plan.
- Do not request confirmation or announce routine plan-file writes. Report only failures or deferrals.
- If a higher-priority mode prohibits writing when the plan is proposed, retain the plan and save it before any implementation in the next write-enabled turn.
- Never delete or rename a plan file unless the user explicitly requests it.

## Git Safety

Inspect `git status --short` and the relevant diff before and after changes. Preserve unrelated user work. Do not run destructive Git commands, initialize repositories, commit, amend, rebase, push, or force-push unless explicitly requested.
