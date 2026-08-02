# Repository Guidelines

## Project Map

- `main/`: application startup and subsystem orchestration.
- `components/cc1101/`: CC1101 SPI driver and radio configuration.
- `components/rf_codec/`: rc-switch-compatible decode/encode and raw identity logic.
- `components/rf_ook/`: ESP-IDF RMT receive/transmit lifecycle and timing bounds.
- `components/rf_storage/`: versioned NVS signal and automation-rule storage.
- `components/rf_automation/`: receive-to-replay rules, queues, cooldowns, and event reporting.
- `components/rf_console/`: UART commands and bounded command parsing.
- `host_tests/`: portable C++17 codec/parser/storage/automation tests.
- `test_apps/unit/`: dedicated ESP-IDF Unity image. Building it does not run its on-device tests.
- `README.md`: authoritative hardware, command, RF, and safety documentation.

## Environment and Commands

Run commands from the repository root. Start Pi from this directory so AFT indexes only this project.

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

Build the dedicated Unity image separately when component behavior changes:

```bash
source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
cd test_apps/unit
idf.py -B build build
```

The production verifier runs a clean out-of-tree firmware build, size report, and image inspection under `/tmp`. It does not access hardware. `idf.py build` is the authoritative compiler gate.

## AFT and Diagnostics

Generic and Espressif clangd both report false ESP-IDF cross-toolchain diagnostics for this project. AFT 0.49 only permits disabling LSP servers in user-level configuration, so keep the global server available and treat these diagnostics as non-authoritative. Keep `validate_on_edit: "syntax"` globally because AFT syntax rejection and rollback remain useful. `idf.py build` is the firmware compilation gate.

Use indexed `grep`, `read`, `aft_outline`, and `aft_zoom` for exploration. Use `edit` for focused changes and `aft_safety` checkpoints before risky multi-file work. Treat dead-code and call-graph results as hints around callbacks, function pointers, FreeRTOS tasks, and hardware entry points.

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

## Git Safety

Inspect `git status --short` and the relevant diff before and after changes. Preserve unrelated user work. Do not run destructive Git commands, initialize repositories, commit, amend, rebase, push, or force-push unless explicitly requested.
