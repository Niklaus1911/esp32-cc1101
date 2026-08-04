# Colored Console and OTA Progress Plan

## Goal

Add two presentation features without weakening the existing RF, Wi-Fi, OTA, queue, or persistence behavior:

1. Show live OTA percentage progress in `tools/push-ota.sh` and on UART.
2. Replace the dense application console output with a readable, color-coded `tio` experience.

The default console style will be `pretty`; `console style plain` will provide the existing machine-oriented records without ANSI sequences. Style is runtime-only and resets to `pretty` at boot.

## Console Design

### Presentation layer

Add a small, bounded formatter inside `rf_console` rather than scattering ANSI sequences through command and event handlers:

- Use portable ANSI 16-color foreground/style sequences only.
- Always terminate colored output with a reset sequence so errors or interrupted events cannot leak color into the prompt.
- Build each event line in a fixed buffer and emit it with one console-owned `printf` where practical.
- Keep SSIDs, names, codes, raw durations, URLs, IP addresses, and error identifiers uncolored and directly copyable.
- Keep the linenoise prompt ASCII and uncolored (`rf> `); embedded escape codes would break its visible-width/editing calculations.
- Use ASCII-only section borders and progress bars for reliable UART rendering.

Semantic palette for a dark terminal:

- Green: ready, online, successful command/action, enabled.
- Cyan: RF receive, neutral state transitions, addresses and informational headings.
- Magenta: RF transmit, replay, and automation actions.
- Yellow: waiting, retry, suppression, paused/maintenance, and recoverable warnings.
- Red: failed commands, faults, drops, validation failures, and unavailable subsystems.
- Dim white: labels, metadata, counters, and inactive values.

Add `console style <pretty|plain>` and expose the current style in help/status. Store it in an atomic runtime value because commands and the asynchronous event worker both read it. Do not add NVS storage.

### Structured status views

Split the current shared status handler into bounded subsystem renderers:

- `status`: full dashboard containing System/RF, Automation, Wi-Fi, and OTA sections.
- `radio info`: RF and CC1101 section only.
- `wifi status`: Wi-Fi section only.
- `ota status`: OTA section only.

Pretty mode will use compact fixed-width ASCII sections, for example:

```text
+-- Wi-Fi ----------------------------------------------------------+
| State       ONLINE        Signal     -60 dBm                     |
| Network     iliadbox-2.4ghz          192.0.2.17                |
| Saved       yes           OTA lock   no                           |
+------------------------------------------------------------------+
```

Keep lines narrow enough for a normal 80-column `tio` window, wrap long values onto a labeled continuation row, and never truncate identifiers silently. Plain `status` retains the existing subsystem record formats; scoped commands print only their corresponding legacy record.

### Events and command results

Give asynchronous output consistent fixed tags such as `[RF RX]`, `[RULE]`, `[WIFI]`, `[OTA]`, `[ OK ]`, and `[FAIL]`. Apply color to the tag and important state token rather than the entire line.

Cover:

- Decoded/raw RX and `last` output.
- Learn armed/completed/timeout/error records.
- Replay/TX and command success/error results.
- Automation trigger/action/suppress/skip/drop events.
- Wi-Fi state, scan, DHCP, disconnect, retry, and persistence errors.
- OTA ready/progress/complete/failure/server events.
- Startup banner and concise help text.

Plain mode preserves the stable `RX`, `RULE`, `WIFI CONNECTED`, `OTA`, and error records used by existing tests and logs. The event callbacks remain bounded queue producers; only the existing console worker renders asynchronous output.

### ESP-IDF and tio integration

Enable ESP-IDF application and bootloader log colors through persistent `sdkconfig.defaults` and matching Unity defaults, using ESP-IDF's native severity palette rather than rewriting framework logs.

Document `tio` usage at 115200 baud, including a practical command such as `tio --color none /dev/ttyUSB0`, and note:

- Normal output mode passes firmware ANSI sequences.
- `--log-strip` removes ANSI/control sequences from recorded logs.
- Local echo must remain disabled so hidden Wi-Fi password input stays hidden.
- `tio --color` controls tio's own messages and is independent of firmware colors.

## OTA Percentage Progress

### PC uploader

Keep curl as the upload engine and preserve all current validation and HTTP failure behavior.

- Keep the status preflight silent.
- For an interactive stderr, run the upload with curl's native `--progress-bar`, which provides live percentage, transferred bytes, rate, and ETA while the JSON body remains captured separately.
- For redirected/noninteractive stderr, use `--no-progress-meter` so CI/log files stay clean while curl errors remain visible.
- Preserve `--fail-with-body`, timeouts, exact `--data-binary`, content type, disabled `Expect`, final JSON validation, and cleanup traps.
- Ensure a failed upload leaves a newline and prints the server JSON/error clearly.

### UART progress

Use the existing bounded OTA progress events; no HTTP callback will print directly. Add tested helpers for:

- Overflow-safe percentage: `min(100, received * 100 / total)` using 64-bit arithmetic.
- Fixed 20-cell ASCII bar.
- Human-readable KiB totals while retaining exact byte counters in plain mode/status.

Pretty output will be discrete, not in-place:

```text
[OTA]  42% [########------------] 394 KiB / 936 KiB
```

The current roughly 64 KiB event cadence remains bounded and produces a final 100% event before validation/completion. Avoid `\r` updates because ESP-IDF logs, RF events, and the linenoise prompt can interleave with UART output.

## Files

Primary changes:

- `components/rf_console/rf_console.cpp`
- `components/rf_console/CMakeLists.txt`
- New private formatter source/header under `components/rf_console/`
- `components/rf_console/test/test_rf_console_parse.cpp` or focused formatter tests
- `host_tests/CMakeLists.txt`, `host_tests/host_tests.cpp`
- `tools/push-ota.sh`
- `sdkconfig.defaults`
- `test_apps/unit/sdkconfig.defaults`
- `test_apps/unit/main/CMakeLists.txt` and console component coverage as needed
- `tools/verify-production.sh`
- `README.md`

Avoid changing OTA streaming, boot-slot selection, RF maintenance, credential storage, or event queue ownership unless a formatter test exposes a required contract adjustment.

## Verification

### Automated

1. Host tests for percentage boundaries, zero totals, overflow safety, fixed bar width, ANSI reset discipline, pretty/plain selection, long value wrapping, and exact legacy plain records.
2. Script tests with a mocked curl for interactive progress arguments, noninteractive suppression, successful JSON, HTTP failure, and malformed success responses; run `bash -n` and `shellcheck` when available.
3. Existing normal and ASan/UBSan host suites.
4. Clean production build/verifier and clean Unity build.
5. Verifier assertions for application/bootloader log-color configuration, unchanged OTA partitions, image validity, and slot growth margin.
6. Stack-usage review for the REPL and event worker, plus image/DRAM/IRAM size reports.
7. Independent blocker review focused on ANSI reset leaks, buffer bounds, prompt/editing regressions, event-line compatibility, and uploader error handling.

### Hardware with explicit approval

1. Use `tio` on a dark terminal and inspect startup, help, dashboard sections, long SSIDs/names/raw frames, warnings, and errors.
2. Verify history, arrows, completion, backspace, hidden password entry, and prompt behavior while asynchronous RF/Wi-Fi events arrive.
3. Switch repeatedly between `pretty` and `plain`; confirm reboot returns to `pretty` and `tio --log-strip` produces escape-free logs.
4. Upload an OTA image and compare live PC percentage with discrete UART percentage through 100%, completion, reboot, slot change, and confirmation.
5. Abort an upload and confirm both displays report failure while RF/automation/Wi-Fi maintenance state is restored.

No flashing, monitor access, or on-device tests are part of implementation verification without separate approval.

## References

- tio upstream documentation: <https://tio.github.io/>
- curl command-line progress/output behavior: <https://curl.se/docs/manpage.html>
- ESP-IDF logging library: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/log.html>
