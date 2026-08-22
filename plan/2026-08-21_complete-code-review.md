# Complete Code Review Report — esp32-cc1101

Date: 2026-08-21. Report-only engagement; no production code was modified.
Scope: working tree as-is, including the uncommitted SSE/`web_events` change set (~506 insertions, 16 modified files, 6 new files).
Method: nine parallel component-cluster reviews plus a README-vs-code contract audit, followed by personal re-verification of every Major finding and all documentation drift at its cited source location. Direct full-file reviews were performed for `cc1101`, `rf_ook`, `rf_storage` core, `platform_nvs`, and `rf_activity_led`.

---

## 1. Executive summary

**Overall verdict: a high-quality, defensively engineered codebase with no Critical findings.** Bounds checking, fail-closed NVS handling, QoS/retention enforcement, transactional lifecycle management (OTA maintenance, hardware switching, MQTT retirement), and convention compliance are consistently strong. The uncommitted SSE work is structurally sound but carries the highest concentration of new risk.

Top risks, in order:

1. **G1 — OTA maintenance can latch permanently active** if any maintenance release fails through its short retry window; RF and automation stay paused until reboot, with no background reclamation (`app_maintenance.cpp:89-94`).
2. **I1 — the documented learning-cancel safety net does not exist as stated**: learning watches only the internal frame-queue drop counter, not console/event-queue overflow (`rf_signals.cpp:334-335` vs `README.md:542`).
3. **R1/R2 — recent-history ID monotonicity has two holes**: a torn NVS write resets IDs to 1 (stale IDs can replay a *different* signal), and clearing a corrupt record also resets IDs (`rf_storage.cpp:244`, `:737`).
4. **H1/I3 — new SSE shutdown path races**: fd-recycle window during stop, and a wedged stream blocks `httpd_stop` (both need hardware verification).

| Severity | Count | Confirmed | Needs-verification |
|---|---:|---:|---:|
| Critical | 0 | — | — |
| Major | 5 | 3 | 2 |
| Minor | 38 | 29 | 9 |
| Observation | ~45 | — | — |

Findings marked **Needs-verification** depend on runtime behavior (ESP-IDF internals, timing, hardware) that static review cannot settle.

---

## 2. Major findings

### G1 — OTA maintenance release failure latches maintenance indefinitely
- **Severity:** Major · **Confidence:** Confirmed (re-verified by direct read)
- **Files:** `components/app_maintenance/app_maintenance.cpp:89-94`; retry loop at `components/ota_update/ota_update.cpp:418-424`
- **Evidence:** `s_active` is cleared only when *all four* acquisitions are released; the uploader retries `end_ota_maintenance()` just 3× at ~10 ms spacing.
- **Impact:** A transient release failure persisting >30 ms (automation lock held by an in-flight ≤5 s transmission, or an RF hardware switch in progress) leaves RF + automation paused, the Wi-Fi OTA lock held, and no background reclamation. Status surfaces `maintenance_error` but nothing acts on it.
- **Direction:** Bounded backoff or low-priority reclamation retry; surface a degraded state that defers conflicting work (see G4).

### I1 — Learning-cancel contract gap (also doc drift)
- **Severity:** Major · **Confidence:** Confirmed (re-verified by direct read)
- **Files:** `components/rf_signals/rf_signals.cpp:334-339` vs `README.md:542`
- **Evidence:** Cancel checks only `s_queue_drops != pending.drops_at_arm` — the rf_signals internal queue. Console-sink overflow increments broker `s_sink_drops` (`bridge_events.cpp:122`) with no feedback path.
- **Impact:** "If the console event queue overflows, learning is cancelled with an error" is not implemented; a display-queue storm during an armed window cancels nothing. The internal-queue check that *does* exist still prevents wrong-name saves, so this is a contract/documentation defect rather than a corruption bug.
- **Direction:** Feed broker sink drops into the arm baseline, or correct README to describe the actual trigger.

### R1 — Torn write can reset recent-history IDs to 1
- **Severity:** Major · **Confidence:** Confirmed code path; trigger is a narrow power-loss window
- **Files:** `components/rf_storage/rf_storage.cpp:244-247` (append is a single-key blob replace, `:665-667`)
- **Evidence:** `if (error == ESP_ERR_NVS_NOT_FOUND) { *history = {}; }` — upstream NVS erases mismatched/incomplete blob indexes at mount, so a torn append reappears as NOT_FOUND after reboot.
- **Impact:** Violates "IDs … remain stable across reboot" (`README.md:493`). All five entries are lost and `next_id` restarts at 1, so a stale UI/console ID such as `recent replay 3` can resolve to a different decoded signal and transmit an unintended RF frame.
- **Direction:** Persist a high-water ID counter separately, or document the torn-write semantics.

### H1 — SSE stop path has an fd-recycle race *(Needs-verification)*
- **Severity:** Major · **Confidence:** Needs-verification (timing-dependent)
- **Files:** `components/web_ui/web_events.cpp:475-476` vs `:543-546`
- **Evidence:** Stop sets `s_stopping`, triggers session close; the stream task may still execute its final chunk send and `httpd_req_async_handler_complete()` while the fd is recycled to a new connection.
- **Impact:** Final chunk bytes could reach an unrelated client; async-request flag cleared on a recycled session slot. New uncommitted code.
- **Direction:** Cooperative-first stop (flag + bounded join, close as last resort) or re-validate socket identity before every send and before `complete()`.

### I3 — Wedged SSE stream blocks HTTP server stop *(Needs-verification)*
- **Severity:** Major · **Confidence:** Needs-verification
- **Files:** `components/web_ui/web_ui.cpp:185-191` (also rollback path `:79-87`)
- **Evidence:** `web_events_stop()` timeout returns early — `httpd_stop()` never runs.
- **Impact:** One stream stuck ≥2 s in a chunked send leaves the server and socket alive across Wi-Fi stop / OTA maintenance entry; status reports "stopped" while sessions continue. Self-heals via the 5 s service-task retry only if the stream eventually exits.
- **Direction:** Force stream exit synchronously (join via task handle) or proceed with `httpd_stop` after logging; report both errors distinctly.

---

## 3. Minor findings

### rf_codec (cluster A)
- **A1** `rf_codec.cpp:289-292` vs `:561-564` — fragment vs full-frame identifiers use two different implementations of the 11/12 alias-window policy; latent divergence if the alias set changes. Confirmed.
- **A2** `rf_codec.cpp:668-684` — raw fingerprint normalizes by minimum pulse, so jitter that moves the minimum re-quantizes every bucket (`{200,400}`→`{8,16}` vs `{210,395}`→`{8,15}`). Display metadata only; duplicate suppression correctly uses `raw_signals_match`. Confirmed.
- **A3** `host_tests/host_tests.cpp:427-431` — round-trip test accepts `kAmbiguous` as success; a regression making every frame ambiguous would pass. Confirmed.

### rf_automation (cluster D)
- **D1** `rf_automation.cpp:180-182` — config-change notification dropped silently on 7 s sink-lock timeout; MQTT/SSE subscribers stay stale with no drop counter. Confirmed.
- **D2** `rf_automation_engine.cpp:89` + `rf_ook.cpp:1030-1031` — frames with invalid capture timing (`captured_us = 0`) bypass the time half of generation-currency check; narrow window where a pre-change frame fires under new configuration. Confirmed.
- **D3** `rf_automation.cpp:355-359` — TX executes under the automation mutex (up to 7 s wait); every automation API call (status polls, add/remove/enable, pause requests) blocks for the full transmission. Confirmed.
- **D6** `rf_automation.cpp:24-35` — hand-copied storage-backend prototypes duplicate `rf_storage_rule_backend.hpp`; drift caught only at link time. Confirmed.
- **D7** `rf_automation.cpp:934-948` — unavailable-path `get_status` performs up to three uncached NVS reads per call; polling against failed storage generates continuous NVS chatter. Confirmed.

### rf_console (cluster E)
- **E1** `rf_console_parse.cpp:15-25` — `strtoull` accepts leading `+` (`send +11043138 …` parses), loosening the documented decimal/0x grammar. Confirmed.
- **E2** `rf_console.cpp:2950-2957` — every usage/validation failure additionally prints untagged boilerplate `Command returned non-zero error code: 0x1`, polluting plain-style machine-readable output. Confirmed.
- **E3** `rf_console.cpp:2021-2026` — Ctrl-C during password prompt misreported as a format-validation error (two errors printed). Confirmed.
- **E4** `rf_console_linenoise.c:351` — column query byte-reads have no timeout while holding editor/output mutex; a terminal passing boot probe but failing `ESC[6n` would hang the REPL. Upstream-parity; likely unreachable. Needs-verification.
- **E5** `rf_console_linenoise.c:1396-1397` — masked input spins on persistent `read()==0` (USB Serial/JTAG disconnect semantics). Needs-verification.

### Network stack (cluster F)
- **F1** `network_wifi.cpp:793-794` — Wi-Fi credentials zeroized only on successful forget; `s_pending_credentials` and queue slots retain passwords after disconnect (MQTT component diligently memsets its equivalents). Confirmed.
- **F2** `network_wifi.cpp:928`, `:1026` — `strcmp` on `wifi_ap_record_t.ssid` (uint8_t[33]); IDF does not guarantee NUL termination for a full 32-byte SSID → potential OOB read. Pattern confirmed; IDF behavior Needs-verification.
- **F3** `network_wifi.cpp:654-656` — silent lock-failure returns leave retry scheduling/state transitions unupdated; stall mode with no diagnostic counter. Confirmed.
- **F4** `network_wifi.cpp:209` — hostname apply latches attempted-generation before `esp_netif_set_hostname`; transient failure leaves DHCP hostname stale until next forced apply. Confirmed.
- **F5** `network_mdns.cpp:48-51` — first-caller mDNS ownership handle never revalidated/released; safe with current long-lived tasks. Confirmed.
- **F6** `network_mdns.cpp:352-353` — lifecycle failure latched forever; recovery depends entirely on external callers re-invoking start. Needs-verification of Web online-sink path.
- **F7** `network_mqtt_storage.cpp:42-45` — PSRAM cold allocations have no internal-pool fallback; exhausted SPIRAM disables MQTT for the boot even when internal heap could serve ≤2.6 KB buffers. Confirmed.
- **F8** `network_mqtt.cpp:1929-1931` — cross-task `vTaskDelete` of MQTT worker without quiesce handshake (boot-time activation-failure path only). Needs-verification.

### OTA + maintenance (cluster G)
- **G2** `ota_update.cpp:389-402` — no whole-upload deadline; a dribbling client holds RF + automation paused arbitrarily long (per-stall 10 s socket timeout keeps restarting). Confirmed.
- **G3** `ota_update.cpp:726` + `main/main.cpp:176` — running image confirmed unconditionally early in boot, before any health/network checkpoint; blunts the documented rationale for rejecting MQTT-only next boots. Tradeoff undocumented. Needs-verification of intent.
- **G4** `app_maintenance.hpp:9` — `ota_maintenance_is_active()` has zero callers; Web mutations (RF hardware switch, radio stop/start) are still served during an active upload — exactly what drives G1's release failures. Confirmed.

### web_ui native (clusters H/I)
- **H2** `web_events.cpp:464-473` — half-open SSE connection (no FIN/RST) holds the single SSE slot indefinitely: heartbeats absorbed by socket buffer, MSG_PEEK probe sees EAGAIN forever, no TCP keepalive, no max stream duration. Contained by documented polling fallback. Needs-verification.
- **H3** `web_ui.cpp:183-190` — on `web_events_stop()` timeout, status reports stopped while already-open sessions continue; original stop cause overwritten in rollback path. Confirmed.
- **H4** `web_events.cpp:30,37,291,460` — ~2.9 KB stack buffers in a 6144-byte worker with no `static_assert` floor and no high-water-mark logging (unlike the other two stacks). Needs-verification on hardware.
- **H5** `web_events.cpp:436-448` — broker-status query failure emits resync with sequence 0, regressing client `last_sequence` and causing churn. Confirmed. (Same root cause independently found as I4.)
- **I2** `rf_signals.cpp:334-341` — learning drop detection is lazy (loop-top only); `LEARN FAILED` can be delayed up to 30 s. Fail-safe direction. Confirmed.
- **I4** = H5 (deduplicated).
- **I5** `bridge_events.cpp:230-232` — sink removal is index-only; maintained `generation` never returned/checked; stale ID after start/stop cycle removes whatever occupies the slot. Latent (all current callers guard). Confirmed.
- **I6** `bridge_control.cpp:234-240` — replay-last TOCTOU: TX events report the fetched frame while `kReplayLast` re-reads mutable `s_last_frame` in the radio task; a newer frame can be transmitted while events report the older one. Confirmed.
- **I10** `rf_signals.cpp:161` vs `:478` — save path ignores catalog-refresh error, forget path propagates it; inconsistent policy. Confirmed.
- **I12** `network_mqtt.cpp:1646-1649` — unbounded retry loop quiescing MQTT sink; dispatcher starvation would leak the worker task. Practically unreachable. Needs-verification.

### app.js (cluster J)
- **J1** `app.js:65` + `index.html:99` — `formBody` performs no percent-encoding (deliberate: server rejects `%`/`+`), but `code` inputs are unvalidated free text; `&`/`=` inject extra form fields into mutation bodies. Confirmed.
- **J2** `app.js:199`, `:1172-1210` — no timeout/abort on mutation fetch or OTA XHR; a stalled transfer locks the whole UI (`setBusy(true)`), unlike `readJson` which has abort handling. Confirmed.
- **J3** `app.js:913-915` — `refreshRules` lacks the single-flight guards given to recent/signals; out-of-order responses can overwrite newer rules. Confirmed.
- **J5** `app.js:724,749` — `renderLive` assumes mandatory snapshot fields (only `radio` has rollback compat); a render exception flips the badge to "Disconnected" while SSE stays healthy. Needs-verification whether any real firmware omits these fields.

### rf_storage core (direct review)
- **R2** `rf_storage.cpp:737-739` — clearing a *corrupt* recent record resets `next_id` to 1 (healthy clears preserve it, proven by Unity test at `test_rf_storage.cpp:499-502`). Same stale-ID consequence as R1, user-triggered. Confirmed. (Doc drift D-doc#4.)

---

## 4. Observations (compressed)

**rf_codec:** `decoded_signal_is_valid` ignores `inverted` (defense-in-depth handled at storage layer, `rf_storage_format.cpp:144-145`) (A4); `shifted_index` relies on caller-side validation (A5); full-frame alias ambiguity practically unreachable but fails closed (A6); worst-case identification cost heavy inside automation loops (A7).

**rf_ook (direct review):** ISR callback is minimal and correct; double-buffer generation scheme sound including release/acquire ordering of the split 64-bit armed timestamp (`rf_ook.cpp:702-706`); classic ESP32 RMT RAM budget is exact (RX 448 + TX 64 = 512 blocks); TX-timeout ladder forces generic TX low and marks faulted-reboot-required per contract (`:936-953`); hardware-switch rollback matrix complete including persistence-failure rollback (`:1938-1958`). Noted: one timed-out command (~14 s) forces whole-service stop (`send_command`, `:1531-1537`) — aggressive but fail-safe; merged glitch pulses can push a capture over the 29 ms validity cap, rejecting the whole capture (fail-closed); software counters survive service restarts (cosmetic).

**cc1101 (direct review):** PA table matches TI 433 MHz values; frequency word formula correct with rounding; MDMCFG profile matches the standard ASK/OOK ~4.8 kBaud configuration; status-register burst-bit access, CHIP_RDYn polarity/wait, and manual-reset sequence all datasheet-correct; transfer bounds match `max_transfer_sz`. Noted: no internal locking — correctness relies on single-owner serialization by rf_ook (documented architecture); early `initialize()` failure paths leave SCLK/MOSI configured as GPIO outputs; `read_stable_status(RSSI)` can return INVALID_RESPONSE under fluctuating signal, surfacing as diagnostics sample errors.

**platform_nvs (direct review):** deliberate no-erase policy correctly skips the standard NO_FREE_PAGES erase-retry pattern, matching the README contract; second concurrent caller during init receives the initial `ESP_ERR_INVALID_STATE`, indistinguishable from not-started.

**rf_storage core (direct review):** create-only atomicity enforced under lock; iterator leak-free on all paths; 15-char names fit NVS key limits exactly; hardware-record rewrite avoidance matches README; missing-key defaults (enabled=true, log=actions) correct; loader re-validates everything the saver enforced. Version-check-after-CRC ordering shared with recent/rules formats (taxonomy only, fails closed identically) (R3); `enabled_set`/`log_mode_set` erase-and-retry on type mismatch is a narrow self-heal in slight tension with "never automatically erases NVS" wording (C13); stray non-blob key in `rf_rules` disables listing and blocks all signal forgets until manually removed — deliberate fail-closed, operationally harsh (R5).

**Recent/rules formats (cluster R):** CRC coverage, endianness, eviction ordering, ID-overflow guard, and loader-vs-saver validation symmetry verified sound (R6-R7).

**Console (cluster E):** E6-E16 observations — correct switch-exhaustiveness fix in uncommitted diff; dead help-branch duplication; style snapshot race yields one transient mixed block; untagged `radio hardware` line; lazy `web auth` init side effect; username validated only after password prompt; masked-input latch requires Ctrl-C recovery; dumb-mode backspace/arrow quirks; upstream completion-buffer defect unreachable today; duplicated password readers; duplicated pulse-bound math vs codec.

**Network (cluster F):** F9-F11 — automation-config events force full discovery republication (traffic amplification only); function-local throttle static survives context recreation; TCP memory reserves protect only first start.

**OTA (cluster G):** G5-G8 — dead exported API; sink-clear timeout advisory; cosmetic status field timing; HTTP task stack headroom unmeasured (~3.7 KB handler locals on 8 KB default).

**Web (clusters H/J):** H6-H11, J4, J6, J8 — SSE responses omit the standard security headers applied elsewhere; route table at exactly 23/23 handlers (next addition fails at runtime); hostname/name JSON fields unescaped but charset-constrained upstream (defense-in-depth suggestion); serialize-failure kills stream instead of degrading to resync; Kconfig disabled-variant compiled nowhere; busy-guarded refetches drop rather than defer updates; watchdog polls every 30 s even on healthy SSE (heartbeat is an invisible comment); uncommitted diff SSE lifecycle/mutual-exclusion/OTA-recovery wiring verified sound.

**Bridge/signals/main (cluster I):** automation TX bypasses bridge_control so no kTxStarted/kTxCompleted events (visibility via automation telemetry only) (I7); sink count 2→3 exactly fits console+MQTT+SSE (I8); console-start failure under NO_MEM panics via `ESP_ERROR_CHECK` rather than degrading to UART-only (I9); bridge_events union payload technically UB-but-mitigated (I11); SSE sequence-gap resync on fresh connect is self-healing overhead (I13); handler/stop TOCTOU can strand a no-op sink slot (I14).

**Activity LED (direct review):** clean. Coalescing, deadline extension, startup sequencing, and failure cleanup all match README; gpio writes inside critical sections are register writes (acceptable).

---

## 5. Documentation drift

| # | README/docs claim | Reality | Verdict |
|---|---|---|---|
| 1 | `README.md:436`: ambiguous matches print `learned_ambiguous=<count>` | Code prints `learned=ambiguous(N)` (`rf_console.cpp:455`, `:2289`); string absent from components/ | **Confirmed drift** |
| 2 | `README.md:542`: console event queue overflow cancels learning | Trigger is rf_signals internal queue drops (`rf_signals.cpp:334`) | **Confirmed drift** (= I1) |
| 3 | `README.md:496`: `recent clear` preserves next valid ID | True except corrupt-record clear path, which resets to 1 (`rf_storage.cpp:737`) | **Partial** |
| 4 | `README.md:492`: sixth entry atomically evicts oldest | True per commit; torn write loses entire key incl. ID counter (power-loss semantics undocumented) | **Partial** |
| 5 | `README.md:394`: retirement "clears ledger then records retired" | Code commits `kRetired` checkpoint first, then erases (`network_mqtt.cpp:1582-1598`); both orders resume correctly | Order inverted |
| 6 | `README.md:572`: generation invalidation limited to add/remove/enable/disable | Runtime pause/resume also advances generation and discards queued frames | Incomplete |
| 7 | `ota_update.hpp:62`: "Runs on the OTA service or HTTP task" | No dedicated OTA service task exists; HTTP owned by web_ui service task | Stale wording |
| 8 | `rf_automation_event.hpp:58-59`: sink replacement "waits for in-flight callback" | Waits up to 7 s, then drops with `ESP_ERR_TIMEOUT` | Overstated |
| 9 | `docs/sse-feasibility.md`: heartbeats detect dead connections; streams joined before shutdown | Detection actually relies on MSG_PEEK probe (cannot detect half-open); stop uses timed poll, not join | Imprecise |

All other audited contracts matched: 350 ms suppression, 1000 ms cooldown, 30 s learn window, raw bounds (100–29000 µs, even 8–256), send/save/rule bounds (bits 4–64, protocols 1–12, repeats 1–20, default 8, 5 s TX bound), name rules, five-entry history, hostname rules, full MQTT parameter/topic/QoS matrix, rule cap 32, LED timings and strapping-pin rejection, Web UI behaviors (50-entry sessionStorage, SSE-first fallback, thresholds), service-mode matrix, OTA contracts (120 s, inactive slot, ELF SHA256, 25% margin), console style/prompt, all four pin maps, partition tables, WIFI CONNECTED format, RX/RAW line formats. **19 of 20 audit groups fully matched.**

---

## 6. Convention violations

- **Duplication (drift-prone):** `BoundedWriter` ×2 (`mqtt_discovery.cpp`/`mqtt_telemetry.cpp`); crc32/u32/u64 helpers ×3 in rf_storage anonymous namespaces; `NvsHandle` RAII ×3 across network components; printable-ASCII validators triplicated; result-mapping helpers named inconsistently; storage backend prototypes hand-copied (D6); password readers duplicated (E15); pulse-bound math duplicated between console and codec (E16).
- **Style:** app.js uses zero indentation throughout (AGENTS.md specifies four spaces; file-consistent legacy); brace-style split between new `web_events*.cpp` (K&R) and existing `web_api.cpp` (Allman); stray double blank lines in rf_automation/rf_storage; namespace-comment spacing inconsistency; missing semicolons on app.js boot sequence.
- **Build hygiene:** `lwip` in public `REQUIRES` should be `PRIV_REQUIRES` (`web_ui/CMakeLists.txt:5`); manual `#ifndef CONFIG_WEB_SSE_ENABLE` fallback deviates from sdkconfig.h norm.

---

## 7. Security posture (summary)

**Verified strong:** same-origin enforcement is exact — Host required on every route, mutations require present-and-matching Origin (absent ⇒ 403, fail-closed), lowercased both sides, port stripped only as exact `:80`, IPv6/trailing-dot rejected (H12, defect-free); app.js has zero HTML-injection sinks — all device-derived strings flow through `createTextNode`/`textContent` (J7); web_auth stores only a SHA-256 verifier with constant-time compare, CSPRNG tokens, CRC-guarded record, and non-extendable lockout (H13); OTA validates prefix identity before any flash write and never selects the incomplete slot (G trace); MQTT commands enforce exact-topic/non-retained/QoS0/exact-payload.

**Hardening opportunities:** half-open SSE connections hold the single stream slot indefinitely (H2); three device-derived `/api/live` fields rely on upstream charset constraints instead of escaping (H8); Wi-Fi credential remnants in RAM after disconnect (F1); form-body field injection via unvalidated `code` inputs (J1); route-table headroom is zero (H7).

The intentional unauthenticated trusted-LAN posture is correctly implemented and consistently documented.

---

## 8. Test gaps (prioritized)

1. **Automation end-to-end on seeded NVS:** fail-closed startup matrix (over-capacity, dangling, cyclic, ambiguous persisted graphs) has zero automated coverage; queue-drop reporting, config-sink ordering, paused-mutation rejection untested.
2. **Storage corruption classes:** truncated/short blobs, unsupported version, reserved-byte, cooldown-byte corruption, post-corruption-clear ID behavior (would pin R2).
3. **OTA rejection matrix:** chip/segment/descriptor/project mismatches, busy 409, truncated body, mid-stream write failure, restore-on-abort, maintenance begin/end symmetry incl. G1 give-up path.
4. **SSE lifecycle:** single-client CAS/409, stop-during-active-stream, overflow/gap resync, heartbeat/disconnect probing (device-level candidates).
5. **Learning state machine:** arm/replace/cancel/timeout interleavings and the drops_at_arm cancel path (only the pure window classifier is tested).
6. **Codec boundaries:** fingerprints (never tested despite printing on every RX line), negative `raw_signal_is_valid` cases, candidate-overflow ambiguity, score-margin ambiguity, fragment identification, 64-bit round trip.
7. **Parsers:** leading `+` (E1), DEL boundary for printable-ASCII validators, wrong-QoS rejection beyond button parser, `parse_replay_arguments` list-reservation.
8. **Web assets:** negative assertion forbidding HTML-injection sinks (cheap XSS regression guard for J7); `code` input grammar; refresh concurrency guards; XHR timeout recovery.
9. **Compile-rot:** `CONFIG_WEB_SSE_ENABLE=n` variant compiled nowhere.

---

## 9. Limitations

Static analysis only. No hardware, RF timing/range, NVS wear, power-loss injection, browser-on-device, or network-fault validation was performed. Items marked Needs-verification require runtime evidence (ESP-IDF 6.0.2 internals, timing windows, or hardware measurement). Editor/clangd diagnostics were not used as evidence. Host tests were not executed as part of this review; findings derive from source reading and cross-tracing only.

## 10. Suggested follow-up order

1. Fix G1 (+G4 admission guard) — prevents indefinite RF/automation pause during OTA.
2. Resolve I1 (code or README) — documented safety net currently doesn't exist.
3. Address R1/R2 ID monotonicity — wrong-signal replay risk after power loss.
4. Hardware-verify H1/H3/H4/H2 on the new SSE path before committing it.
5. Batch the small confirmed minors (E1-E3, F1, J1-J3, A1-A3, D6) into a hygiene pass.
6. Reconcile the nine documentation-drift items.
