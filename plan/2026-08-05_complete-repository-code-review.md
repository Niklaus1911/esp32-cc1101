# Complete Repository Code Review and Remediation

## Summary

- Review current `HEAD` (`4edf507`) across all tracked firmware, tests, Web assets, build configuration, scripts, and documentation.
- Audit managed mDNS/MQTT dependencies only at their integration boundaries and against ESP-IDF 6.0.2 documentation and applicable upstream issues.
- Record severity-ranked findings with evidence, impact, reproduction path, and remediation.
- Automatically fix every confirmed correctness, safety, security, persistence, or reliability defect. Exclude cosmetic churn and speculative refactors.
- Give each independently testable root cause its own regression, verification cycle, and commit.

## Review Passes

1. **Architecture and concurrency**
   - Trace startup, fallback, shutdown, reconnect, and maintenance ordering across NVS, RF, console, event delivery, Wi-Fi, Web, MQTT, mDNS, and OTA.
   - Audit ISR/callback/task ownership, queues, atomics, locks, acknowledgement waits, stale generations, disconnect epochs, cleanup, and failure propagation.
   - Check for races, deadlocks, use-after-free, lost notifications, stale replay, double initialization, and blocking work in time-sensitive contexts.

2. **RF, automation, and persistence**
   - Review CC1101 SPI and RMT timing, half-duplex transitions, timeout recovery, RX restoration, protocol/raw bounds, consensus, duplicate handling, and queue overflow.
   - Validate automation matching, cooldowns, generation invalidation, single-action behavior, log delivery, and replay ownership.
   - Audit every NVS schema for bounds, versions, CRCs, malformed records, atomic commits, power-loss behavior, iterator cleanup, credential handling, and the no-automatic-erase invariant.

3. **Network and input surfaces**
   - Audit console parsing and secret prompts; HTTP body limits, Host/Origin policy, CSP, JSON escaping, handler lifecycle, and OTA validation.
   - Review MQTT QoS/retain rules, fragmented inputs, exact command matching, outbox handling, discovery ledgers, tombstones, retries, retained snapshots, reconnect semantics, and Home Assistant contracts.
   - Check Wi-Fi candidate persistence, DHCP transitions, mDNS conflict handling, profile exclusivity, browser polling, reconnect behavior, and failure diagnostics.

4. **Resources, tests, and release configuration**
   - Inspect all allocation and cleanup paths, queue sizes, task stacks, internal-heap fragmentation, static DRAM/IRAM use, and low-memory fallbacks.
   - Review existing tests for false positives, missing boundary cases, cleanup failures, and gaps between host policy tests and ESP-IDF lifecycle code.
   - Validate CMake dependencies, Kconfig defaults, OTA partitions, scripts, shell safety, dependency locks, documentation accuracy, generated-file hygiene, and accidental secret/local-path exposure.

## Remediation Protocol

- Confirm each suspected issue against source and tests before editing; append a short fix plan to this review plan.
- Apply the smallest root-cause fix, preserving public console commands, HTTP/MQTT contracts, NVS formats, and RF behavior unless that contract is itself defective.
- Add a focused host or Unity regression. For concurrency-only defects, add an extractable policy test where practical and verify the real ESP-IDF path by build and hardware observation.
- Run focused tests, all host tests, the applicable firmware build, and `git diff --check` before each `fix: ...` commit.
- Repeat the affected subsystem review after every fix, then perform a complete second pass until no confirmed defect remains.
- Do not amend, rebase, push, alter unrelated files, or touch the pre-existing `.playwright-mcp/` directory.

## Verification and Hardware

- Establish pre-fix baselines with normal host tests, ASan/UBSan host builds, JavaScript and shell syntax checks, Unity-image compilation, and `tools/verify-production.sh`.
- After all commits, repeat every automated gate and inspect image metadata, partitions, dependency versions, compiler output, final diff, commit ordering, and flash/DRAM/IRAM changes.
- Validate the Web UI first against a deterministic mock and then read-only on the device at desktop and mobile viewports, including screenshots, overflow, console/network errors, polling, reconnects, numeric-IP and `.local` access, and DNS-SD.
- Before hardware work, confirm the classic ESP32 and CP2102 by-id port, back up the `0x9000/0x6000` NVS partition, and flash the exact final production image without erasing NVS.
- Test Web and MQTT profiles for three Wi-Fi stop/start cycles and a five-minute settled soak each. Verify boot ordering, profile exclusivity, service recovery, persisted 5-signal/3-rule state, MQTT reconciliation, retained-state counters, mDNS/HTTP lifecycle, and error/drop counters.
- Require at least 20 KiB settled free internal heap, a 16 KiB largest block, 768-byte task-stack margins, cumulative minimum heap above 2 KiB, and no more than 1 KiB settled degradation across reconnect cycles.
- Treat any reset, assertion, watchdog, allocation failure, monotonic memory loss, stuck service state, unexpected outbox growth, or unexplained event drop as blocking and return it to remediation.
- RF reception is observational only. Do not transmit, replay, learn, delete signals, mutate rules, upload OTA, erase flash, or run Unity tests on-device.
- Restore MQTT mode at the end and confirm `ready`, Wi-Fi at the expected address, persisted catalog counts, stable memory, and `ESP_OK` status fields.

## Assumptions

- The trusted-LAN unauthenticated Web UI and plaintext MQTT 3.1.1 are documented product choices; the review checks containment and implementation correctness rather than redesigning them.
- No public API or persisted-format change is planned. Any unavoidable compatibility correction must include migration/compatibility handling, tests, and documentation in the same defect commit.
- Completion requires a clean second review pass, all automated gates passing, both hardware profiles meeting acceptance thresholds, and a final report listing findings, commits, verification evidence, and residual hardware limitations.

## Confirmed Defect 1: Preserve Unattempted MQTT Retained Snapshots

- **Root cause:** `publish_pending_states()` removes both the last-RX and last-automation snapshots before it attempts either publish. If the RX topic/payload cannot be formatted or its QoS 1 publish fails, only the RX snapshot is restored and the unattempted automation snapshot is lost.
- **Fix:** take the RX snapshot first, publish it, and only then take the automation snapshot. Keep the existing restore-if-unset behavior so a failed older publish cannot overwrite a newer producer snapshot.
- **Regression/verification:** inspect all early-return paths for ownership symmetry, run the MQTT host suite and full host suite, compile the production image, and exercise reconnect/failure recovery during the MQTT hardware pass.

## Confirmed Defect 2: Reconcile MQTT Readiness on Every Wi-Fi Loss State

- **Root cause:** the MQTT bridge sink treats only `kDisconnected` as offline. DHCP/IP loss and operator stop are delivered as `kStateChanged` events with a non-online state, so MQTT can retain `network_online=true` and `network_ready=true` after the interface has lost usable connectivity.
- **Fix:** centralize the event-to-availability mapping in a pure MQTT policy helper. Connected/state/error events report online only when their state is `kOnline`; explicit disconnects always report offline; scan-only events leave availability unchanged.
- **Regression/verification:** add host coverage for online, offline, disconnect, and scan-only mappings; run all host tests and the production verifier; observe readiness transitions during the three Wi-Fi reconnect cycles in both hardware profiles.

## Confirmed Defect 3: Roll Back Partial RF-Signals Initialization

- **Root cause:** `initialize_rf_signals()` returns after allocation or task-creation failure without deleting mutexes and queues already created. Because initialization is retryable, later attempts overwrite the handles and leak additional internal RAM.
- **Fix:** add one initialization-only cleanup routine and invoke it on every failure after resource allocation. Reset catalog ownership with the resources so a retry starts from a coherent empty state.
- **Regression/verification:** inspect every initialization exit for complete ownership transfer or rollback, compile the Unity image containing the RF-signals component tests, run all host tests, and run the production verifier with the final low-memory size audit.

## Confirmed Defect 4: Keep OTA Initialization Status Consistent

- **Root cause:** `initialize_ota_update()` fills `s_status` as available before creating the reboot task. If task creation fails, the atomic availability flag and return value report failure while `get_ota_update_status()` can still report an available idle service with `initialization_error == ESP_OK`.
- **Fix:** publish the available/idle status only after all required resources, including the reboot task, exist. Record every initialization failure in the status snapshot while preserving the mutex for diagnostics.
- **Regression/verification:** inspect all OTA initialization exits for consistent atomic and status state, build the production image and Unity image, run all host tests, and verify the runtime status remains unavailable on any failed initialization path.

## Confirmed Defect 5: Keep Wi-Fi Initialization Status Consistent

- **Root cause:** `initialize_network_wifi()` marks `s_status.available` before creating the network task. A task allocation failure cleans up the queues and mutexes but leaves a stale success-looking status snapshot.
- **Fix:** defer status publication and the saved-credential snapshot publication until after task creation succeeds; record the initialization error before cleanup on failure.
- **Regression/verification:** inspect the Wi-Fi allocation failure path, compile the Unity image and production image, run all host tests, and verify no public status reports an available manager without its owner task.

## Confirmed Defect 6: Converge Wi-Fi Stop Races

- **Root cause:** operator stop and scan-then-stop paths ignore `ESP_ERR_WIFI_NOT_STARTED` after setting `kStopping`. If the driver has already stopped or its stop event was lost, the manager can remain stuck in `kStopping` and fail to notify network consumers that the interface is offline.
- **Fix:** treat `ESP_ERR_WIFI_NOT_STARTED` as an already-completed stop, clear driver/IP/association state, emit the offline notification, and converge to `kOff` in every operator-owned stop path.
- **Regression/verification:** add focused stop-result policy coverage, run the host suite, compile the Unity and production images, and observe repeated Wi-Fi stop/start cycles for a stable `off`/`online` transition.

## Confirmed Defect 7: Make Sink Quiesce Waits Tick-Wrap Safe

- **Root cause:** OTA and Wi-Fi sink-detach functions compare the current tick against `start + timeout`. The comparison is invalid when the FreeRTOS tick counter wraps, so callbacks can be detached too early or wait indefinitely roughly every counter period.
- **Fix:** measure elapsed ticks using unsigned subtraction from the captured start tick, matching the already-correct bridge-event implementation.
- **Regression/verification:** inspect all sink-detach wait loops for wrap-safe arithmetic, run the host suite and production verifier, compile the Unity image, and include sink detach/rebind observation in the hardware soak.

## Confirmed Defect 8: Preserve MQTT Retirement Errors

- **Root cause:** retained state-topic retirement combines topic formatting and QoS publish into one condition and replaces either failure with generic `ESP_FAIL`. The status and retry path therefore lose whether retirement failed because of a bounded-buffer contract or an acknowledgement timeout/publish error.
- **Fix:** report `ESP_ERR_INVALID_SIZE` for topic formatting failure and otherwise preserve the exact `wait_for_publish()` result.
- **Regression/verification:** inspect every retirement stage for first-error preservation, run all MQTT and host tests, compile the Unity image, run the production verifier, and confirm retirement diagnostics during the hardware profile transition without completing a destructive retirement.

## Confirmed Defect 9: Restore Lazy UART Web-Auth Administration

- **Root cause:** the `web auth status/rotate` commands remain registered, but no startup path has ever called `initialize_web_auth()`. Status therefore times out on a null mutex and rotation always rejects the unavailable component. Repeated failed initialization can also overwrite and leak the retained diagnostic mutex.
- **Fix:** make initialization single-shot and diagnostic-safe, load existing records without automatically creating an unused trusted-LAN token, allow missing/corrupt records to be repaired only by explicit rotation, make uninitialized status readable, and trigger initialization lazily from the UART commands.
- **Regression/verification:** review all never-initialized, valid, missing, corrupt, and hard-failure transitions; compile the Unity image; run all host tests and the production verifier; and verify status/rotation only through non-mutating status during hardware validation so persisted data is preserved.

## Confirmed Defect 10: Preserve RF-Signals Failure Diagnostics After Rollback

- **Root cause:** failed RF-signals initialization now correctly deletes its diagnostic mutex, but `get_rf_signals_status()` unconditionally tries to lock that deleted mutex and returns `ESP_ERR_TIMEOUT`. UART, Web, and bridge snapshots therefore lose the real initialization error precisely when diagnostics are needed.
- **Fix:** return a bounded lock-free unavailable snapshot from atomics whenever the service is unavailable, and retain mutex-protected status copying for the running service.
- **Regression/verification:** inspect uninitialized, failed, retrying, and available transitions; compile the Unity image; run all host tests and the production verifier; and confirm the hardware status path still reports the running catalog and queue counters.

## Confirmed Defect 11: Preserve Wi-Fi Failure Diagnostics After Rollback

- **Root cause:** every failed Wi-Fi initialization path deletes the status mutex, but `get_network_wifi_status()` unconditionally tries to lock it. The public snapshot therefore replaces the stored allocation, MAC, or task-creation failure with `ESP_ERR_TIMEOUT` after cleanup.
- **Fix:** return a lock-free unavailable snapshot containing the atomic initialization error and counters when the manager is unavailable, while retaining the mutex-protected full snapshot for the running manager.
- **Regression/verification:** inspect all pre-initialization and cleanup exits; compile the Unity image; run all host tests and the production verifier; and confirm normal hardware status retains saved credentials, network state, counters, and the successful initialization result.

## Confirmed Defect 12: Publish Event-Broker Availability Only After Initialization

- **Root cause:** the bridge-event broker uses `s_available` as both an initialization guard and the public readiness flag, setting it before the mutex, queue, and dispatcher task exist. Concurrent callers can observe a partially initialized broker, while failed cleanup deletes the mutex and makes status return a misleading timeout instead of the allocation failure.
- **Fix:** add a dedicated retryable initialization guard, publish availability only after all resources exist, retain the exact initialization error, and return an unavailable counter snapshot plus that error without locking after failure.
- **Regression/verification:** inspect pre-init, in-progress, allocation-failure, retry, and ready transitions; compile the Unity image; run all host tests and the production verifier; and confirm the hardware broker status shows both registered sinks with stable drop counters.

## Confirmed Defect 13: Publish RF-Signals Availability Only After Initialization

- **Root cause:** RF signals also uses `s_available` as its retry guard, setting public readiness before its mutexes and queue exist. A concurrent learning request can pass the availability check and send to a null queue, while frame and status callers can observe a partially initialized service.
- **Fix:** use a dedicated retryable initialization guard, keep the service unavailable until the catalog, task, queues, and mutexes are ready, and clear the guard on every success or rollback exit.
- **Regression/verification:** inspect learning, frame, status, failure, retry, and successful startup interleavings; compile the Unity image; run all host tests and the production verifier; and confirm hardware startup reports the persisted catalog before accepting receive events.
