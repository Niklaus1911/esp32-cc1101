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
| Pi MCP configuration | `.pi/mcp.json` |
| Pi package declaration | `.pi/settings.json` |
| Planned Codex project configuration | `.codex/config.toml` |
| Codex user configuration and trust | `$HOME/.codex/config.toml` |

All committed configuration should use `$HOME` or discover the Git root. Do not commit `$HOME` paths, OAuth credentials, browser profiles, or generated package caches.

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
- Pi's local ESP-IDF Tools MCP server
- the planned Codex ESP-IDF Tools MCP server
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

Do not rerun `eim fix` unless the required archive and recovery procedure have been deliberately verified. An MCP startup failure is not permission to modify, reinstall, repair, or delete anything under `$HOME/.espressif`.

The direct `export.sh` and `export.fish` workflow is the supported local path for this project.

## Pi MCP Setup

Pi reads `.pi/mcp.json` when started from the repository root.

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

## Planned Codex MCP Setup

Codex supports committed, trusted project configuration in `.codex/config.toml`. Keep Pi and Codex configuration side by side during migration.

The planned Codex configuration uses native Streamable HTTP for Espressif documentation and stdio for the local ESP-IDF server:

```toml
[mcp_servers.espressif-docs]
url = "https://mcp.espressif.com/docs"
enabled = true
startup_timeout_sec = 30
tool_timeout_sec = 60
default_tools_approval_mode = "approve"

[mcp_servers.esp-idf-tools]
command = "fish"
args = [
    "-c",
    "set project_root (git rev-parse --show-toplevel); and source \"$HOME/.espressif/v6.0.2/esp-idf/export.fish\" >/dev/null; and test \"$IDF_PYTHON_ENV_PATH\" = \"$HOME/.espressif/python_env/idf6.0_py3.14_env\"; and exec idf.py -C \"$project_root\" mcp-server",
]
enabled = true
startup_timeout_sec = 60
tool_timeout_sec = 600
default_tools_approval_mode = "auto"

[mcp_servers.esp-idf-tools.tools.build_project]
approval_mode = "approve"

[mcp_servers.esp-idf-tools.tools.set_target]
approval_mode = "prompt"

[mcp_servers.esp-idf-tools.tools.clean_project]
approval_mode = "prompt"

[mcp_servers.esp-idf-tools.tools.flash_project]
approval_mode = "prompt"
```

The environment guard deliberately rejects the older non-MCP Python environment.

Project trust belongs in the uncommitted user configuration:

```toml
[projects."<absolute-checkout-path>"]
trust_level = "trusted"
```

For this checkout, replace `<absolute-checkout-path>` with the expanded value of `$HOME/esp-projects/esp32-cc1101`; TOML does not expand `$HOME`.

Espressif Documentation requires browser-based OAuth. Authenticate after the project configuration exists:

```bash
codex -C "$HOME/esp-projects/esp32-cc1101" mcp login espressif-docs
```

Codex should store the resulting credentials in the operating-system keyring. Never put bearer tokens or OAuth data in `.codex/config.toml`.

## Playwright for Codex

The planned Codex browser workflow uses Microsoft's Playwright CLI rather than a third MCP server. This keeps MCP context limited to the two Espressif servers and avoids adding Node project metadata to the firmware repository.

Install the CLI into the existing user-owned npm prefix without `sudo`:

```bash
npm install -g @playwright/cli@latest
playwright-cli install-browser
playwright-cli --help
```

Install Microsoft's `playwright-cli` Codex skill under the user Codex skills directory, not in this repository:

```text
$HOME/.codex/skills/playwright-cli
```

Browser validation must use isolated sessions and temporary outputs under `/tmp`. Validate against a deterministic local mock before using an authorized device. Intercept RF transmit, replay, destructive maintenance, OTA, and configuration-changing requests unless those effects are explicitly authorized.

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
8. Validate Codex configuration with `codex doctor --json` and `codex mcp list --json` after `.codex/config.toml` is added.
9. Exercise only `build_project` during MCP setup validation.
10. Do not test clean, target changes, flash, serial monitor, or on-device Unity without the required approval.

## Recovery Boundaries

If activation or MCP startup fails:

1. Record `$IDF_PATH`, `$IDF_PYTHON_ENV_PATH`, and the exact error.
2. Verify the checkout, export script, Python executable, and `FastMCP` import without changing them.
3. Fall back to the documented shell build commands.
4. Do not run `eim fix`, reinstall ESP-IDF, delete environments, clean the project, change the target, or flash hardware without explicit approval.
