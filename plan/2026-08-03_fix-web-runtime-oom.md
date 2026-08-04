# Fix Web Runtime OOM After DHCP

## Context

Hardware now proves `esp_wifi_init()` succeeds, leaving 32,504 bytes of internal 8-bit heap with a 31,744-byte largest block. The Web server remains stopped after DHCP because its current persistent requests exceed that budget before allocator overhead:

- SSE event queue: `12 * sizeof(BridgeEvent)` = 10,176 bytes
- Operation worker stack: 8,192 bytes
- SSE worker stack: 10,240 bytes
- HTTPD stack: 10,240 bytes

Compiler stack-usage output reports 2,432 bytes for the operation worker, 5,168 bytes for the SSE worker, and a 6,384-byte maximum API handler frame. Web startup failures are currently silent and are not reflected by `ota status` when API initialization fails before `httpd_start()`.

## Approach

1. **Set a hardware-derived Web memory budget**
   - Reduce the operation worker stack to 4,096 bytes and the SSE worker stack to 7,168 bytes, retaining approximately 1.6 KiB and 2 KiB above compiler-reported usage.
   - Set the HTTPD stack default to 8,192 bytes, retaining approximately 1.8 KiB above the largest compiled handler frame.
   - Reduce the Web event queue from 12 to 6 full events. Preserve sequence-gap reporting and status reconciliation so a slow SSE client remains isolated and detects dropped bursts.
   - Reduce retained operation records/payloads from 12 to 8, the existing enforced minimum, reclaiming about 3.2 KiB of static DRAM while preserving request-ID retry deduplication.
   - Add compile-time assertions for event queue bytes, worker stack floors, and operation table/queue relationships. Do not alter ESP-IDF Wi-Fi buffer counts, RF queues, or OTA transfer buffering.

2. **Make initialization transactional and diagnosable**
   - Keep Web API allocation deferred until network-online notification.
   - Report the exact failed Web initialization stage and error while preserving complete task, queue, mutex, and broker-sink cleanup for retry.
   - Log free internal heap and largest block before API allocation, after API workers start, and after HTTPD starts.
   - Propagate pre-HTTPD API failures to OTA/Web status so `ota status` reports `ESP_ERR_NO_MEM` instead of `ESP_OK`; log throttled retry failures from the Web service task.
   - Confirm disconnect/reconnect reuses one API runtime and does not duplicate tasks, queues, sinks, or HTTP servers.

3. **Verify behavior and memory limits**
   - Extend hardware-independent contract checks for the configured HTTPD stack and bounded queue/table constants where practical.
   - Re-run host tests, JavaScript and shell syntax checks, Web asset contracts, `git diff --check`, clean ESP-IDF production verification, size/image inspection, and Unity image compilation.
   - Inspect production DWARF/stack-usage output to confirm the task and handler margins still hold after optimization.
   - Do not rerun Playwright unless frontend assets change.

## Files

- `components/web_ui/web_api.cpp`: Web queue/table depths, worker stacks, compile-time budgets, staged initialization diagnostics, and retry cleanup.
- `components/web_ui/web_ui.cpp`: HTTPD heap diagnostics, visible retry failures, and OTA status propagation.
- `components/ota_update/Kconfig`: 8 KiB HTTPD stack default.
- `sdkconfig.defaults`: persistent production HTTPD stack setting.
- `tools/verify-production.sh`: enforce the new production stack configuration.
- Focused host/Unity tests only if implementation introduces testable helpers or contracts.

## Hardware Acceptance

After the verified image is flashed with explicit approval:

1. Cold boot with saved credentials reaches DHCP without the original Wi-Fi OOM.
2. UART reports the Web allocation stages and `Web UI ready at http://<device-ip>:8032/`.
3. `ota status` reports `LISTENING : 8032`, and the authenticated UI loads at the assigned address.
4. REST, SSE, and authenticated OTA remain usable; slow SSE delivery reports sequence gaps rather than blocking RF/UART.
5. Disconnect/reconnect stops and restarts port 8032 without increasing task/sink counts or reducing stable free heap on each cycle.
