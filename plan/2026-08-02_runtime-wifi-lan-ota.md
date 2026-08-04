# Runtime Wi-Fi and LAN OTA Plan

## Goal

Add optional, runtime-controlled Wi-Fi station support and reliable application OTA for the classic 4 MB ESP32 DevKit. Wi-Fi must use DHCP, remain off when no network is saved, and nonblockingly auto-connect when valid saved credentials exist without becoming a startup dependency for RF. OTA will accept an unsigned application image uploaded directly from a trusted PC to the ESP32 over plain HTTP, require no UART command or physical device interaction, validate that it is an `esp32-cc1101` image for the running ESP32 and revision, pause RF safely while flash is written, and use dual OTA slots with boot rollback.

No implementation step flashes, erases, opens a monitor, or runs on-device tests without separate approval.

## Settled Decisions

- With no saved network, Wi-Fi initializes lazily only after a runtime command. A valid saved network queues a nonblocking connection after normal boot initialization.
- `wifi connect <ssid>` obtains the password through a bounded non-echoing prompt. Empty passwords support open LANs.
- Candidate credentials stay in RAM while connecting. Only `IP_EVENT_STA_GOT_IP` atomically replaces the saved plaintext credentials; failed attempts preserve the previously saved network.
- The ESP-IDF Wi-Fi driver uses RAM storage so it cannot create an independent credential copy. Physical flash confidentiality is out of scope for this personal-use device.
- IPv4 configuration comes only from the default DHCP client. `online` means `IP_EVENT_STA_GOT_IP`, not merely AP association.
- OTA uses ESP-IDF's HTTP server and native `app_update` APIs. A PC initiates and streams the update directly; HTTPS, authentication, signed applications, secure boot, flash encryption, and eFuse anti-rollback are out of scope.
- Same-version and downgrade images are allowed, but wrong target, chip revision, project, format, or size are rejected.
- Existing RF NVS data must survive the partition migration. The first OTA-capable image requires one approved wired flash without whole-chip erase.
- OTA temporarily pauses automation, rejects all new RF TX/replay, waits for current RF work, disables RX, and restores the exact prior runtime state after failure or cancellation.

## Runtime Interfaces

### Wi-Fi commands

- `wifi status`: state, saved/persistence flags, active and saved SSID, disconnect reason, retry count, DHCP IPv4/netmask/gateway/DNS, RSSI, and last error. Never print the password.
- Every `IP_EVENT_STA_GOT_IP`, including boot auto-connect and later reconnects, must deliver one stable high-priority line through the console owner: `WIFI CONNECTED ssid=<ssid> ip=<ip> netmask=<netmask> gateway=<gateway>`.
- `wifi connect <ssid>`: prompt for a password without linenoise history/echo, then asynchronously connect. A successful DHCP lease automatically and atomically saves these credentials.
- `wifi start`: asynchronously connect using saved credentials after an operator-issued `wifi stop`.
- `wifi stop`: suppress reconnect for the current boot, disconnect, stop the Wi-Fi driver, clear DHCP/address state, and remain idempotent without deleting credentials.
- `wifi forget`: erase only the saved network key, clear candidate credentials, disconnect/stop Wi-Fi, and prevent reconnect; do not erase an NVS namespace or partition.
- `wifi scan`: perform a bounded asynchronous station scan and report a capped, sorted result set without blocking the REPL.

States are explicit: `off`, `starting`, `connecting`, `waiting_dhcp`, `online`, `retry_wait`, `stopping`, and `fault`. Initial failures use bounded exponential backoff; an operator stop never reconnects during that boot. AP loss after a working connection retries with a capped delay until stopped. A failed candidate connection leaves the prior saved record untouched; a DHCP-successful connection stays online even if persistence fails and reports `active_saved=0` plus the storage error while retaining the prior saved record. Event handlers only enqueue state changes and never block or write UART.

### PC-initiated OTA

The normal workflow is one noninteractive PC command:

```bash
tools/push-ota.sh <esp32-ip> /tmp/esp32-cc1101-production-build/esp32-cc1101.bin
```

The ESP32 exposes an unauthenticated LAN HTTP API while Wi-Fi is online:

- `GET /api/v1/ota/status`: JSON containing running/update partition, current project/version, OTA state, bytes/content length, progress, and last error.
- `POST /api/v1/ota`: require `application/octet-stream` and a valid `Content-Length`, stream exactly one application image, return a final JSON success/error response, and schedule reboot only after the success response is sent.

No UART command, confirmation prompt, button, or other device interaction starts OTA. Closing or losing the upload connection aborts the incomplete update. The UART may retain read-only `ota status` diagnostics, but it is not part of the update workflow.

Only one upload may run. OTA requires Wi-Fi `online`; `wifi stop`, Wi-Fi reconfiguration, radio reset/start, manual TX/replay, rule mutation that could transmit, and a concurrent upload return busy while maintenance is active. Successful OTA reports completion, allows bounded console/HTTP drain time, and restarts. Failure, disconnect, or abort never restarts.

## Architecture

### Shared NVS initialization

Add `components/platform_nvs/` as the sole idempotent owner of default NVS initialization and status. It must propagate `ESP_ERR_NVS_NO_FREE_PAGES`, version, and corruption errors without erasing anything. Refactor RF storage to consume this initializer while preserving current namespace-level fault isolation and existing public behavior.

Add a separate versioned, CRC-protected `net_cfg` record with fixed bounds for SSID and password. Missing data means no saved network and no boot connection. Malformed data disables only saved Wi-Fi auto-connect; runtime connection remains available. A DHCP-successful connection or explicit `wifi forget` repairs only the selected key.

### Wi-Fi component

Add `components/network_wifi/` with:

- A bounded owner task/control queue for all start, stop, connect, scan, and retry transitions.
- Lazy `esp_netif_init`, default event-loop creation, STA netif creation, and `esp_wifi_init` using `nvs_enable = 0` and `WIFI_STORAGE_RAM`.
- Instance event handlers for `WIFI_EVENT` and `IP_EVENT_STA_GOT_IP`/lost-IP. Handlers copy minimal data into a zero-wait queue.
- Default DHCP client behavior only; no static-IP API or fallback address. Set a stable DHCP hostname but treat the assigned IPv4 address as authoritative.
- Snapshot status protected independently from callbacks.
- Typed console delivery that separates critical connection events from lossy diagnostics. The network owner retains each `GOT_IP` record until the console accepts it, without blocking the ESP event handler; retries, scans, and other logs remain bounded and cannot starve RF/learn events.
- Normal modem power save, temporarily changed to `WIFI_PS_NONE` by OTA and restored afterward.

Network initialization/configuration errors are nonfatal to RF, storage, automation, and the console. If a valid saved record exists, its boot connection is queued only after the console and RF startup attempts complete; startup never waits for association or DHCP.

### RF maintenance contract

Extend `rf_automation` with a nonpersistent runtime pause that takes its existing mutex, waits for an in-flight action, advances the generation, and prevents queued frames from becoming actions. Do not alter persisted rule enable state.

Extend `rf_ook` with an exclusive maintenance acquisition owned by the radio task:

1. Atomically close the public TX/replay gate so later requests fail immediately.
2. Serialize behind any already-running radio command.
3. Disable RX and record the prior desired RX state.
4. Reject TX, replay, reset, and radio-start commands until release.
5. On release, discard stale captures and restore the prior desired RX state unless the radio became faulted.

Create a small `components/app_maintenance/` coordinator that applies lock ordering consistently: pause automation, acquire RF maintenance, and unwind in reverse order on every error path. OTA receives this coordinator through a narrow API rather than embedding RF internals.

### OTA component

Add `components/ota_update/` with an ESP-IDF `esp_http_server` lifecycle, one-upload state machine, status mutex, delayed-reboot worker, and typed event sink. Start the server on a fixed documented port when Wi-Fi becomes online and stop it after Wi-Fi loss when no upload owns the connection. Network callbacks only queue this lifecycle work.

The upload handler must:

- Accept only `POST /api/v1/ota` with `application/octet-stream`, a positive `Content-Length`, and no chunked body; cap header/body receive timeouts.
- Require the online interface, reject a concurrent update, select the inactive OTA partition, and reject a body larger than that slot before any erase.
- Buffer and validate the complete ESP image header and `esp_app_desc_t` before entering maintenance or writing flash. Require image magic, classic ESP32 chip ID, compatible chip revision bounds, and `project_name == "esp32-cc1101"`; report candidate version/IDF version and permit same-version or downgrade updates.
- Enter RF maintenance, call native `esp_ota_begin`, receive exactly the declared number of bytes in bounded chunks, and write sequentially with `esp_ota_write`. A timeout, short body, socket close, or write error calls `esp_ota_abort` and restores maintenance state.
- Call `esp_ota_end` for ESP-IDF's complete segment, checksum, appended SHA-256, chip/revision, and image validation, then call `esp_ota_set_boot_partition` only after validation succeeds.
- Send the final JSON response before queueing a bounded delayed restart. Throttle console progress events by byte/percentage thresholds.

Use only public ESP-IDF 6.0.2 image/app-update APIs and structures; do not duplicate private bootloader validation logic. Wrong images may invalidate only the inactive slot and can never change the boot partition.

Enable bootloader app rollback. On a pending-verification boot, mark the image valid only after platform NVS initialization, console startup, partition sanity, and bounded RF startup attempts complete. External AP availability and CC1101 wiring must not determine image health. A crash/reset before confirmation lets the bootloader select the previous slot.

### Console integration

Keep domain work out of command handlers. Extend `rf_console` with strict Wi-Fi parsers, hidden bounded password input, optional read-only OTA diagnostics, and dedicated bounded Wi-Fi/OTA event queues consumed by the existing console worker after frame/learn events. Deliver every `WIFI CONNECTED ... ip=...` record through the critical system-event path; event callbacks and worker components never call `printf` directly. Extend global `status` with concise network, OTA server, running partition, rollback, and maintenance fields.

## Partition and Build Changes

Add a tracked custom 4 MB partition CSV that preserves NVS exactly at offset `0x9000`, size `0x6000`, then adds `otadata`, `phy_init`, and two equal, 64 KiB-aligned OTA slots. Preferred layout:

```text
nvs       data nvs     0x009000 0x006000
otadata   data ota     0x00f000 0x002000
phy_init  data phy     0x011000 0x001000
ota_0     app  ota_0   0x020000 0x1e0000
ota_1     app  ota_1   0x200000 0x1e0000
```

There is no factory slot; a blank `otadata` selects `ota_0`. The one-time wired migration writes the bootloader, new partition table, and application at `0x20000` without erasing `0x9000..0xefff`.

Update `sdkconfig.defaults` with the custom table, 4 MB header, rollback, and bounded HTTP-server settings. Give the Unity build a compatible OTA table/config so integration code compiles against the same assumptions. Do not enable signing, secure boot, flash encryption, anti-rollback, TLS, or HTTP authentication.

Extend `tools/verify-production.sh` to fail unless:

- NVS offset/size are unchanged and `otadata`, `ota_0`, and `ota_1` have exact expected offsets/subtypes/equal sizes.
- The flashed app offset is `0x20000`, both OTA slots fit the binary with explicit growth margin, and the bootloader still fits before `0x8000`.
- Rollback and the bounded OTA HTTP server are active, the image remains classic ESP32/4 MB/ESP-IDF 6.0.2, and checksum/hash validation passes.

Add `tools/push-ota.sh <esp32-ip> <image>` as a noninteractive PC client. It must require an explicit verified `.bin`, inspect its local ESP32 metadata, query device status, upload with `curl --data-binary` and strict failure handling, print the device's final JSON response, and optionally poll for the new version after reboot. It never uses serial, flashes through a cable, prompts on the device, or chooses an image implicitly.

## Files

New components/files:

- `components/platform_nvs/**`
- `components/network_wifi/**`
- `components/ota_update/**`
- `components/app_maintenance/**`
- `partitions_ota.csv`
- `test_apps/unit/partitions_ota.csv`
- `tools/push-ota.sh`

Primary integrations:

- `main/main.cpp`, `main/CMakeLists.txt`
- `components/rf_storage/rf_storage.cpp`, component CMake/tests
- `components/rf_automation/include/rf_automation.hpp`, runtime/tests
- `components/rf_ook/include/rf_ook.hpp`, owner lifecycle/tests
- `components/rf_console/rf_console.cpp`, parser headers/sources/tests
- `sdkconfig.defaults`, `test_apps/unit/sdkconfig.defaults`
- `tools/verify-production.sh`, `README.md`, `AGENTS.md`
- `host_tests/CMakeLists.txt`, `host_tests/host_tests.cpp`

## Verification

### Host and build gates

- Host tests for credential record round trips/CRC/bounds, Wi-Fi state transitions, retry/backoff/disconnect classification, DHCP readiness, exact connection-line formatting/delivery, command parsing, HTTP method/content-type/content-length/body policy, image descriptor/project/chip/revision policy, upload disconnect/progress, and maintenance unwind ordering.
- ASan/UBSan host run.
- Unity compile coverage for NVS missing/wrong-type/out-of-range/corrupt records, save only after DHCP success, preservation of the previous record after failed connection, forget/repair, boot auto-connect decisions, and namespace isolation without erase.
- Clean production verifier, clean Unity image build, partition dump, image inspection, component/DRAM/IRAM size reports, and stack-usage reports for Wi-Fi, OTA, console, automation, and radio owner tasks.
- Independent blocker review focused on event-handler lifetime, repeated start/stop, queue ownership, OTA abort paths, rollback confirmation, lock ordering, partition migration, and preservation of RF behavior.

### Hardware gates requiring later approval

1. Back up the existing `0x9000/0x6000` NVS partition, perform the one-time wired no-erase migration, and prove learned signals/rules/log mode survive.
2. Exercise open/WPA2 networks, hidden password input, automatic save after DHCP, nonblocking boot auto-connect, one `WIFI CONNECTED ... ip=...` line for every DHCP success/reconnect, failed replacement preserving the old network, persistence failure while remaining online, `wifi forget`, wrong credentials, delayed DHCP, AP loss, reconnect, repeated start/stop, scans, and heap/stack stability.
3. Start every OTA solely with `tools/push-ota.sh`; reject wrong method/content type, missing/false length, wrong-project, wrong-chip/revision, malformed, truncated, and oversized uploads before activation; test client disconnect and network loss during each OTA phase.
4. Alternate `ota_0`/`ota_1`, verify successful confirmation, force a pre-confirmation crash/reset and prove rollback, and test power interruption at multiple write points.
5. Prove OTA maintenance emits no RF TX, automation action, or accepted RX; then prove failure/cancellation restores the prior RX/automation state and successful OTA reboots cleanly.
6. Record RF receive/decode quality and Wi-Fi/OTA heap high-water marks before and after networking. Compilation alone is not reported as RF, DHCP, NVS migration, or OTA validation.
