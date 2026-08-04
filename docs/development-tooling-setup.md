# Development Tooling Setup

This document records the machine-local ESP-IDF, MCP, coding-agent, and browser-testing setup used for this project. It is intentionally more specific than the general build instructions in `README.md`.

Last verified with ESP-IDF 6.0.2, classic ESP32, and Codex CLI 0.146.0.

## Paths at a Glance

| Purpose | Path |
|---|---|
| ESP-IDF checkout | `$HOME/.espressif/v6.0.2/esp-idf` |
| Bash activation | `$HOME/.espressif/v6.0.2/esp-idf/export.sh` |
| Fish activation | `$HOME/.espressif/v6.0.2/esp-idf/export.fish` |
| Canonical MCP-capable Python environment | `$HOME/.espressif/python_env/idf6.0_py3.14_env` |
| Canonical Python executable | `$HOME/.espressif/python_env/idf6.0_py3.14_env/bin/python` |
| Older VS Code Python environment | `$HOME/.espressif/tools/python/v6.0.2/venv` |
| Active Codex MCP configuration and trust | `$HOME/.codex/config.toml` |
| Legacy Pi MCP configuration | `.pi/mcp.json` |
| Legacy Pi package declaration | `.pi/settings.json` |

All committed configuration should use `$HOME` or discover the Git root. Do not commit user-specific absolute home paths, OAuth credentials, browser profiles, or generated package caches.

## ESP-IDF Installation

The project uses the checkout at:

```text
$HOME/.espressif/v6.0.2/esp-idf
```

Use the stable checkout export scripts directly. Do not depend on generated EIM activation helpers.

Bash:

```bash
source "$HOME/.espressif/v6.0.2/esp-idf/export.sh"
```

Fish:

```fish
source "$HOME/.espressif/v6.0.2/esp-idf/export.fish"
```

The production scripts source `export.sh` themselves, so they do not require a pre-activated shell:

```bash
tools/verify-production.sh
```

## Python Environments

Two ESP-IDF Python environments exist for different historical consumers.

### Canonical MCP Environment

The environment that contains `mcp[cli]` is:

```text
$HOME/.espressif/python_env/idf6.0_py3.14_env
```

This is the canonical environment for:

- `idf.py mcp-server`
- Pi's legacy local ESP-IDF Tools MCP server
- the active Codex ESP-IDF Tools MCP server
- command-line production verification after activation

Validate it with:

```fish
source "$HOME/.espressif/v6.0.2/esp-idf/export.fish" >/dev/null
printf '%s\n' "$IDF_PYTHON_ENV_PATH"
"$IDF_PYTHON_ENV_PATH/bin/python" -c \
    "from mcp.server.fastmcp import FastMCP; print('MCP_OK')"
```

Expected output includes:

```text
$HOME/.espressif/python_env/idf6.0_py3.14_env
MCP_OK
```

The shell prints the expanded absolute path rather than the literal `$HOME` form.

### Older VS Code Environment

The older environment is:

```text
$HOME/.espressif/tools/python/v6.0.2/venv
```

It still supports the ESP-IDF VS Code extension's normal build and flash buttons, but it does not contain the MCP package. Do not use it to launch `idf.py mcp-server`.

VS Code and agent MCP configuration are independent. A working VS Code build/flash setup does not prove that the MCP environment is correct, and changing the Pi or Codex MCP configuration does not replace the VS Code extension configuration.

## EIM State

The ESP-IDF checkout and toolchains are intact, but the EIM registration was emptied by a failed repair attempt. The repair failed because this local archive was unavailable:

```text
$HOME/.local/share/eim/offline_archives/archive_v6.0.2_linux-x64.zst
```

A generated helper currently exists at `$HOME/.espressif/tools/activate_idf_v6.0.2.fish`, but it is not suitable for MCP startup. It exits with status 1 when sourced by non-interactive `fish -lc` and hard-codes the older Python environment. Redirecting its output hides the explanatory error and leaves Codex reporting a closed MCP initialize connection.

Do not rerun `eim fix` unless the required archive and recovery procedure have been deliberately verified. An MCP startup failure is not permission to modify, reinstall, repair, or delete anything under `$HOME/.espressif`.

The direct `export.sh` and `export.fish` workflow is the supported local path for this project.

## Legacy Pi MCP Setup

Pi reads `.pi/mcp.json` when started from the repository root. These files remain as transition history and are not used by the active Codex setup.

The remote Espressif Documentation MCP currently uses `mcp-remote`:

```text
npx -y mcp-remote https://mcp.espressif.com/docs
```

The local ESP-IDF Tools MCP launches fish, activates ESP-IDF 6.0.2, and runs:

```fish
idf.py -C (pwd) mcp-server
```

The local MCP request timeout is ten minutes to allow firmware builds to complete.

The Tools server exposes:

- `build_project`
- `clean_project`
- `flash_project`
- `set_target`

Safety policy:

- `build_project` is an iterative compiler check.
- `set_target` requires approval because it can regenerate project configuration.
- `clean_project` requires approval because it removes build artifacts.
- `flash_project` requires explicit approval and a confirmed board and port.
- MCP builds do not replace host tests, Unity compilation, or `tools/verify-production.sh`.

Pi's local npm installation cache is intentionally ignored through:

```gitignore
/.pi/npm/
```

## Codex MCP Setup

Codex CLI reads the active MCP registrations and project trust from `$HOME/.codex/config.toml`. That user-level file also contains personal model settings and must not be committed to this repository.

The active servers are:

| Name | Transport | Purpose |
|---|---|---|
| `espressif-docs` | Streamable HTTP with OAuth | Search official Espressif documentation |
| `esp-idf-tools` | Local stdio | Build, clean, set target, and flash ESP-IDF projects |
| `playwright` | Local stdio | Drive a browser for Web UI validation |

The working ESP-IDF Tools launcher is equivalent to:

```fish
fish -lc 'source "$HOME/.espressif/v6.0.2/esp-idf/export.fish" >/dev/null; and test "$IDF_PYTHON_ENV_PATH" = "$HOME/.espressif/python_env/idf6.0_py3.14_env"; and exec "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" -C "$HOME/esp-projects/esp32-cc1101" mcp-server'
```

The environment guard deliberately rejects the older non-MCP Python environment. A protocol-level initialization test of this launcher exposed exactly `build_project`, `clean_project`, `flash_project`, and `set_target`.

Espressif Documentation uses browser-based OAuth. Codex stores its credential outside the repository. Never put bearer tokens, OAuth data, personal model settings, or project trust configuration in versioned files.

`codex mcp list` reports `Auth: Unsupported` for the two local stdio servers. This is normal: they do not use Codex's OAuth mechanism. It does not mean that startup or tool discovery failed.

## Playwright for Codex

The active Codex browser workflow uses Microsoft's Playwright MCP server through `npx @playwright/mcp@latest`. In a fresh Codex session, `/mcp` should list browser navigation, interaction, console, network, viewport, snapshot, and screenshot tools under `playwright`.

Validate against a deterministic local mock before using an authorized device. Intercept RF transmit, replay, destructive maintenance, OTA, and configuration-changing requests unless those effects are explicitly authorized. Do not reuse a personal authenticated browser profile.

Do not commit browser profiles, storage state, screenshots, traces, npm caches, or OAuth credentials.

## Verification Checklist

Use this checklist after environment changes:

1. Activate `export.fish` or `export.sh`.
2. Confirm `$IDF_PATH` is `$HOME/.espressif/v6.0.2/esp-idf`.
3. Confirm `$IDF_PYTHON_ENV_PATH` is `$HOME/.espressif/python_env/idf6.0_py3.14_env`.
4. Import `FastMCP` with the activated Python executable.
5. Run native host tests from a clean, non-IDF shell.
6. Build the Unity image when component behavior changed.
7. Run `tools/verify-production.sh` as the final compiler, size, image, and partition gate.
8. Run `codex mcp list`, start a fresh Codex session, and use `/mcp` to confirm all three servers expose their expected tools.
9. Exercise only `build_project` during ESP-IDF MCP setup validation.
10. Do not test clean, target changes, flash, serial monitor, or on-device Unity without the required approval.

## Recovery Boundaries

If activation or MCP startup fails:

1. Record `$IDF_PATH`, `$IDF_PYTHON_ENV_PATH`, and the exact error.
2. Verify the checkout, export script, Python executable, and `FastMCP` import without changing them.
3. Fall back to the documented shell build commands.
4. Do not run `eim fix`, reinstall ESP-IDF, delete environments, clean the project, change the target, or flash hardware without explicit approval.
