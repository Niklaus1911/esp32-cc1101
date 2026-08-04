# Fix Wi-Fi startup out-of-memory

## Context

The device now reaches RF startup but `esp_wifi_init()` fails with `ESP_ERR_NO_MEM` while allocating Wi-Fi configuration state. This is a startup allocation-order regression introduced by the Web/event architecture:

- `BridgeEvent` is 848 bytes; the 32-entry broker queue reserves 27,136 bytes before Wi-Fi starts.
- Web API initialization currently allocates a 12-entry event queue plus 8 KiB and 10 KiB worker stacks before Wi-Fi starts.
- The console keeps another 8-entry full `BridgeEvent` queue even though that queue only carries low-rate learn/TX/Web lifecycle fields.
- The production size report leaves 58,372 bytes in the reported DRAM region before runtime queues, task stacks, ESP-NETIF, the event loop, and Wi-Fi driver allocations.

Preserve the existing Wi-Fi buffer defaults and OTA throughput. Reclaim and defer application memory instead of masking the regression by shrinking ESP-IDF Wi-Fi buffers.

## Implementation

### 1. Defer the heavy Web runtime

Files: `components/web_ui/web_ui.cpp`, `components/web_ui/web_api.cpp`, `components/web_ui/private_include/web_api.hpp`

- Keep authentication and the lightweight Web lifecycle service initialized at boot so UART token provisioning/recovery remains available.
- Remove `initialize_web_api()` from `initialize_web_ui()`.
- Transactionally initialize Web API queues, tasks, mutexes, and the broker sink from `start_server()` only after the network-online notification confirms DHCP connectivity.
- Make Web API initialization retryable after complete cleanup on transient `ESP_ERR_NO_MEM`; prevent duplicate tasks or broker sinks across retries.
- Recheck network-online state before starting HTTPD so a disconnect during allocation cannot leave a server running offline.
- Preserve existing SSE quiescence and OTA-aware shutdown behavior.

Expected startup saving before `esp_wifi_init()`: approximately 28 KiB.

### 2. Reduce early event memory without losing burst isolation

Files: `components/bridge_events/bridge_events.cpp`, `components/rf_console/rf_console.cpp`

- Reduce the ordered broker ingress queue from 32 to 16 entries. This still exceeds the bounded 12-result Wi-Fi scan burst while saving 13,568 bytes.
- Add compile-time assertions for `BridgeEvent` size and total broker queue storage so future payload growth cannot silently recreate the problem.
- Replace the console's 8-entry full `BridgeEvent` queue with a compact fixed `ConsoleSystemEvent` containing only the fields used for learn, TX, and Web lifecycle rendering. Keep the existing queue depth and independent frame/automation/network/OTA queues.
- Preserve zero-wait publication, per-consumer drops, global sequence ordering, and UART/Web isolation.

Expected additional startup saving: roughly 19 KiB.

### 3. Improve Wi-Fi failure handling and diagnostics

File: `components/network_wifi/network_wifi.cpp`

- Log free internal 8-bit heap and largest free block immediately before `esp_wifi_init()` and on allocation failure.
- Track whether `esp_wifi_init()` actually succeeded and only call Wi-Fi deinitialization/unregistration paths that own initialized resources. Avoid the current secondary unregister/deinit warnings after the primary OOM.
- Keep failures nonfatal to RF and UART, and retain the existing saved-network and NVS policies.

### 4. Regression coverage and documentation

Files: `test_apps/unit/main/test_rf_console_component.cpp` or a focused new test, `README.md` only if diagnostics/user-visible behavior changes

- Add focused coverage for compact console system-event conversion/rendering where it can remain hardware-independent.
- Keep the existing host Web asset, uploader, learned matching, and parser tests.
- Document only user-visible startup/recovery changes; do not add generated `sdkconfig` changes.

## Verification

1. Run host tests:
   `cmake -S host_tests -B /tmp/esp32-cc1101-host-tests && cmake --build /tmp/esp32-cc1101-host-tests && ctest --test-dir /tmp/esp32-cc1101-host-tests --output-on-failure`
2. Run `tools/verify-production.sh` and compare firmware, DRAM, and IRAM totals.
3. Build the Unity image with ESP-IDF 6.0.2; do not flash or run it without explicit approval.
4. Re-run JavaScript syntax, Web asset contract, `git diff --check`, and responsive Playwright checks if frontend assets change.
5. Hardware acceptance after an approved user flash:
   - No `wifi nvs cfg alloc out of memory` or secondary Wi-Fi cleanup warnings.
   - Saved station reaches `WIFI CONNECTED` and Web starts on port 8032.
   - Logged pre-init free heap/largest block provide explicit margin.
   - RF/UART remain operational before, during, and after Wi-Fi connection.
   - Wi-Fi disconnect/reconnect stops and restarts the shared HTTP service without duplicate tasks/sinks.
