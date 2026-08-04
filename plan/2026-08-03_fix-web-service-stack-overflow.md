# Fix Web Service Stack Overflow

## Context

Hardware now reaches DHCP and successfully allocates the deferred Web API runtime:

- Before Web API allocation: 34,180 bytes free, largest block 31,744 bytes.
- After Web API allocation: 16,988 bytes free, largest block 15,872 bytes.
- The device then detects a stack overflow in `web_service` during synchronous HTTP server startup and reboots.

The 4,096-byte service stack was sized from only the local compiler frame. `start_server()` currently reserves 1,840 bytes while calling into `httpd_start()`. Most of that frame comes from the 848-byte lifecycle `BridgeEvent` and the by-value publish copy, even though the event is not needed until HTTPD startup has completed. ESP-IDF's nested HTTP server creation path then exceeds the remaining service stack.

Espressif documentation and issue research confirms that HTTPD/task stacks require contiguous internal RAM, URI handlers execute on the HTTPD task, FreeRTOS queues copy their items, and runtime high-water measurements under worst-case requests are the authoritative sizing method. It also supports allocating the largest remaining persistent block before smaller queues/tasks to reduce fragmentation risk.

## Approach

1. **Remove lifecycle-event storage from the HTTP start/stop call frames**
   - Move Web started/stopped `BridgeEvent` construction and publication into a dedicated `[[gnu::noinline]]` helper.
   - Call the helper only after `httpd_start()` and handler registration have succeeded, or after `httpd_stop()` has completed.
   - Re-run compiler stack-usage analysis and require `start_server()` and `stop_server()` to remain at or below 512 bytes. Keep the event helper isolated so future inlining cannot reintroduce the overlap.

2. **Restore a conservative Web service stack and allocate largest-first**
   - Increase `kServiceTaskStackSize` from 4,096 to 6,144 bytes.
   - Replace the local-frame-only assertion with a budget that includes at least 4 KiB for nested ESP-IDF lifecycle calls in addition to the measured application frame.
   - After DHCP, call `httpd_start()` before allocating the Web API queues/workers so the 8 KiB HTTPD stack receives a contiguous block first. Initialize the Web API and register all asset, OTA, and API handlers only after HTTPD creation succeeds.
   - If Web API initialization or handler registration fails, stop the just-created HTTP server, preserve transactional API cleanup, update OTA/Web status, and retry from a clean state. Do not leave a listening server with a partial route set.
   - Keep the HTTPD stack at 8,192 bytes, the operation/SSE worker stacks at 4,096/7,168 bytes, the six-event SSE queue, the eight-operation retention table, and the 2 KiB mutation body limit unchanged.
   - The larger service stack consumes 2 KiB, but largest-first HTTPD allocation avoids requesting its stack after the heap has been split by the API event queue and worker stacks. Any later allocation failure must return a logged/status-visible error rather than corrupting the service stack or rebooting.

3. **Add runtime stack evidence**
   - Log `uxTaskGetStackHighWaterMark(nullptr)` in bytes before HTTPD startup and after successful handler registration, alongside free/largest internal heap.
   - Report the service stack high-water value in the existing Web startup/failure UART diagnostics without adding frontend assets or a new control endpoint.
   - Preserve the existing five-second transactional retry, OTA status propagation, and deduplicated failure events.

## Files

- `components/web_ui/web_ui.cpp`: lifecycle event helper, 6 KiB service stack, largest-first HTTPD/API startup, transactional rollback, revised compile-time budget, and service stack high-water diagnostics.
- `components/web_ui/web_api.cpp` and its private header only if a cleanup entry point is required to roll back a successfully allocated API runtime after later handler-registration failure.
- No changes are expected in Wi-Fi buffers, Web API queue sizes, HTTPD configuration, OTA transfer behavior, frontend assets, or generated `sdkconfig`.

## Verification

1. Run `git diff --check`, shell/JavaScript syntax checks, and the three host tests.
2. Run `tools/verify-production.sh` and confirm the production image still uses an 8,192-byte HTTPD stack and unchanged Wi-Fi buffer counts.
3. Generate `-fstack-usage` output for `web_ui.cpp`; require start/stop frames no larger than 512 bytes and verify the no-inline lifecycle helper owns the large event frame.
4. Verify failure injection or focused lifecycle tests cover HTTPD failure, API failure after HTTPD creation, handler-registration failure, and retry without leaked tasks, sinks, or a partial server.
5. Build the Unity image without running or flashing it.
6. Skip Playwright because no frontend assets change.

## Temporary Hardware Authorization

This request grants temporary permission to flash and monitor the connected ESP32 only while resolving the current Wi-Fi/Web startup failures.

1. Run `tio --list` to discover the serial port. Continue automatically only when one ESP32 candidate is unambiguous; stop and ask if multiple plausible ports are present.
2. Stop any active serial monitor before flashing, activate ESP-IDF 6.0.2, and flash only the clean production image/configuration on the discovered port. Do not erase NVS or flash, run `menuconfig`, change target/partitions, or run the on-device Unity image.
3. Reopen the discovered port with `tio`, capture the complete boot and Web lifecycle output, and use bounded monitor sessions so tooling does not remain attached indefinitely.
4. Iterate implementation, clean build, production flash, and serial monitoring as needed until the acceptance checks below pass or a new hardware blocker requires user input.
5. This authorization expires when the current Wi-Fi/Web startup problem is fixed and validated. Any later flashing, erasure, or on-device tests require new explicit approval.

## Hardware Acceptance

1. Cold boot reaches DHCP, Web API runtime ready, and `Web UI ready` without a stack overflow or reboot.
2. The reported `web_service` minimum free stack is at least 1,536 bytes after HTTPD and all handlers start.
3. `ota status` reports `LISTENING : 8032`, and the authenticated UI loads at the assigned address.
4. Exercise `GET /api/v1/rules`, a maximum 256-pulse raw mutation, two SSE clients, and OTA status/upload authorization without stack overflow.
5. Disconnect and reconnect Wi-Fi twice; confirm one HTTP server is recreated each time, heap/stack minima remain stable, and no task or broker sink is duplicated.
