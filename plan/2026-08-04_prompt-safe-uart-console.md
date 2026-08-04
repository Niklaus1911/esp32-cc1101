# Prompt-Safe UART Console and Startup Logging

## Summary

- Preserve the immediate editable `rf> ` prompt while RF, Wi-Fi, DHCP, OTA, and Web UI initialization continue.
- Fix prompt corruption by coordinating application output and native `ESP_LOG*` output with the active editor state.
- Use a repository-owned derivative of ESP-IDF 6.0.2 linenoise. Current and upstream Espressif APIs expose no active-buffer redraw operation; official documentation supports building applications directly on the underlying linenoise and `esp_console` APIs.
- Keep the existing event architecture, informational logs, console styles, command syntax, and memory bounds. Add no new log queue.

## Implementation Changes

- Add a symbol-prefixed linenoise derivative under `rf_console`, retaining its BSD license and recording the ESP-IDF 6.0.2 source revision.
- Extend it with synchronized suspend/redraw operations that retain the live buffer, cursor, hints, history position, terminal width, and multiline row count while external output is printed.
- Replace `esp_console_new_repl_uart()` with repository-owned UART/VFS initialization and an 8 KiB REPL task using public `esp_console_init()`, completion/hint callbacks, and `esp_console_run()`. Preserve the 2,048-byte command limit, history, arrows, completion, hints, paste handling, and immediate startup banner.
- Upgrade `OutputGuard` into a recursive output coordinator. Only the outermost nested guard clears and redraws the editor; existing event queues and output ordering remain unchanged.
- Install a reentrant `esp_log_set_vprintf()` hook before starting the REPL. Save and invoke the previous handler inside the coordinator so native RF, Wi-Fi, Web, and OTA logs retain their exact content and severity formatting. Treat the prefix, body, and newline callbacks used by ESP-IDF text logging as one output transaction, with a bounded fragment fail-safe, so the prompt is not redrawn inside a native log record.
- Restore the prior log handler before destroying coordinator state on any startup failure, then release commands, help registration, console state, VFS/UART ownership, tasks, queues, and mutexes in reverse order.
- Replace the direct Wi-Fi password reader with a coordinated hidden-input mode. Asynchronous records may redraw `WiFi password: `, but must never display the entered value, add it to history, or retain it after completion, cancellation, or failure.
- In dumb-terminal mode, emit asynchronous output on a fresh line and reprint the prompt plus current unmasked buffer afterward. No ANSI cursor movement is allowed; visual duplication of the partial line is acceptable because editing/history are already unavailable in this mode.
- Update serial-console documentation to state that asynchronous events and native logs preserve active input, including during Web startup, and document the dumb-terminal limitation.

## Interfaces

- Keep the public `start_rf_console()`, style APIs, commands, plain records, event DTOs, and subsystem APIs unchanged.
- Add only component-private editor and coordinator interfaces for normal input, hidden input, external-output suspension, redraw, injected test I/O, and fixed test terminal widths.
- Keep ROM, bootloader, and application logs emitted before console initialization unchanged.

## Test Plan

- Add deterministic host tests for single-line redraw, cursor-middle editing, multiline clearing/redraw, hints, completion, history navigation, paste input, backspace, nested output guards, repeated concurrent logs, dumb mode, maximum-length commands, and hidden-input interruption without secret disclosure.
- Run the normal host suite, ASan/UBSan host tests, Unity image build, ESP-IDF Tools MCP build, `tools/verify-production.sh`, size inspection, and `git diff --check`.
- Confirm the post-Web heap baseline remains comparable to the observed approximately 32 KiB free heap, no large persistent allocation or queue is introduced, and repeated redraw activity causes no restart, watchdog, stack overflow, or event-drop regression.
- Flash the production image without erasing NVS to the confirmed classic ESP32 on `/dev/ttyUSB0`, using ESP-IDF Tools MCP where available.
- The MCP server has no serial-monitor operation, so use `idf.py monitor` as the preferred fallback and perform one final compatibility pass with the documented `tio` command.
- After opening the monitor, pause and ask the user to press the physical RESET button. Keep the monitor attached and capture the complete ROM-to-Web-ready startup sequence.
- During that startup, type part of `status`, wait through RF/Wi-Fi/Web logs, finish the command, and verify the exact original input executes once.
- Validate cursor movement, arrows, TAB, backspace, history, pretty/plain switching, a multiline `raw append` command without `raw send`, RF receive output, Web learn/cancel events, and Wi-Fi reconnect logs.
- Interrupt a disposable invalid Wi-Fi password entry with an asynchronous event and confirm neither the partial nor complete password appears in serial output.

## Assumptions

- Immediate prompt availability is required; startup will not be delayed until DHCP or Web readiness.
- Native informational logs remain visible rather than being filtered or downgraded.
- Hardware validation may reset, build, flash, and monitor the confirmed board, but will not erase NVS or intentionally transmit RF during this work.

## Implementation Results

- Added the repository-owned, symbol-prefixed ESP-IDF 6.0.2 linenoise derivative and custom UART REPL with synchronized normal, multiline, dumb-terminal, and masked-input redraw.
- Coordinated application output and fragmented native `ESP_LOG` output without adding a persistent queue. Full Wi-Fi records remain intact instead of receiving a prompt between their prefix, body, and newline.
- Host tests, ASan/UBSan host tests, the ESP-IDF Tools build, Unity image build, clean production verification, image inspection, and `git diff --check` passed.
- Flashed the production image to the confirmed classic ESP32 on `/dev/ttyUSB0` without erasing NVS. The final image is `0xfb2a0` bytes with `0xe4d60` bytes (48%) free in the app partition.
- Captured complete ROM-to-Web-ready startup sequences after physical and controlled resets. Native RF, Wi-Fi, DHCP, OTA, and Web logs remained intact; Web became ready at `192.0.2.17`; post-Web heap remained about 34 KiB free with a 32 KiB largest block.
- Typed `sta` during startup, allowed all remaining startup records to complete, then typed `tus`; the preserved input executed exactly one `status` command. Reported console, RF, automation, and Wi-Fi queue/drop counters remained zero.
- Verified pretty/plain switching, wrapped `raw append` staging followed by `raw clear`, hidden-password interruption, Web learn/cancel events, ANSI cursor-middle insertion, backspace, TAB completion, and history recall. No RF transmit command was run and no RF receive frame occurred during this validation window.
- ESP-IDF monitor correctly exercised the documented dumb-terminal path because the tool PTY does not emulate terminal status replies. The final `tio` pass used scripted terminal replies to exercise ANSI editing and redraw on the same hardware.
