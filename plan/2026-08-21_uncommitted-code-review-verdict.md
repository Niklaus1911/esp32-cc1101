# Uncommitted-Code Review Verdict — SSE Live Updates + Automation Config Events

Date: 2026-08-21. Candidate: working-tree diff hash `33532becd951c9b85ff47417261b3dabf2d08db4` (16 modified files, 506 insertions, 99 deletions; 6 new files). Verdict-only engagement; no code was changed during this review.

## Verdict: READY to commit

Zero blocking defects found. All executable gates passed. Non-blocking follow-ups are listed below and should be addressed as follow-ups (two are recommended before the next on-device release).

## Gate results

| Gate | Result |
|---|---|
| Phase 0 drift check | Diff unchanged since the 2026-08-21 full review; findings map cleanly |
| Phase 1 hunk-by-hunk diff review | Complete; no new blockers beyond known needs-verification items |
| Phase 2 wiring/docs consistency | CMake/Kconfig/route wiring correct and pinned by tests; README Web UI section accurate; `docs/sse-feasibility.md` has two wording imprecisions (doc-only) |
| Host tests (clean shell) | 8/8 passed, including new `test_web_sse_format` (exact framing, capacity fit, truncation rejection, event-name and data-newline injection rejection) |
| `test_web_assets.mjs` structural contracts | Passed (`node host_tests/test_web_assets.mjs $(pwd)`); pins route table 23/23, socket limit 3, SSE stop ordering, admission CAS ordering, worker-failure completion ordering, config-sink locking, client fallback/coalescing/watchdog, OTA resumeLive coverage |
| Unity compile esp32 + esp32s3 | Both images built successfully |
| Firmware profile build (`build-board.sh esp32-devkit build`) | Passed |
| Browser validation (Playwright mock) | **DEFERRED** — no browser tooling available in this execution environment. The structural mjs contract suite is the substitute. Interactive desktop/mobile validation remains outstanding before relying on UI behavior. |
| `tools/verify-production.sh` (reason: firmware source, embedded assets, component metadata, Kconfig all change production output) | **Passed all four profiles.** Smallest free OTA margin 0xa6090 (~680 KB) on esp32-devkit; xiao 0x2a58f0; n16r8 0x6a3800; supermini 0xa6ec0 |

## Diff assessment highlights

- Config-sink design is sound: events are built under `AutomationLock` but delivered after release via a dedicated recursive mutex; the unlocked setter path is reachable only when automation permanently failed (no concurrency). Header contract matches implementation except the overstated "waits for an in-flight callback" wording (see follow-up 6).
- `kAutomationConfig` handled in every sink (console silent-ack, MQTT full reconcile, SSE forward); union payload placement-new pattern consistent with existing members and covered by a new Unity copyability test.
- SSE admission/stop ordering is deliberate and test-pinned: sink removal before queue deletion, client-slot CAS before shared state, async-handler completion before slot release.
- app.js lifecycle verified sound: single stream, exponential backoff 1–10 s, visibility-aware close/resume, kind-coalesced debounced refreshes, action-ID deduplicated rule pulses, SSE closed for OTA upload with `resumeLive()` on every recoverable outcome.
- README updated accurately (SSE-first with `/api/live` authoritative, four documented fallbacks).

## Non-blocking follow-ups

1. **H1 (needs-verification):** SSE stop fd-recycle race — final chunk send / `httpd_req_async_handler_complete()` can race session deletion. Recommend cooperative-first stop or socket re-validation before each send. Recommended before next on-device release.
2. **H2 (needs-verification):** half-open connection holds the single SSE slot indefinitely (no TCP keepalive, no max stream duration). Contained by polling fallback. Recommend `SO_KEEPALIVE` or server-initiated lifetime cap.
3. **H4:** SSE worker stack headroom unmeasured (~2.9 KB buffers on a 6144-byte stack). Measure `uxTaskGetStackHighWaterMark` on hardware before release; add a `static_assert` floor like the other two stacks.
4. **H3 (confirmed):** `web_events_stop()` timeout leaves status "stopped" while already-open sessions continue until the service-task retry self-heals. Report both errors distinctly.
5. **H5/I4:** broker-status query failure emits resync with sequence 0, causing client churn. Skip resync when `bridge_events_get_status` fails.
6. **D1 + header wording:** config-notification dropped silently on sink-lock timeout (no drop counter); `rf_automation_event.hpp` overstates the replacement guarantee ("waits" vs wait-up-to-7 s then drop).
7. **F9:** every automation-config event forces full discovery republication at retained QoS 1 — traffic amplification only; consider distinguishing catalog vs state-only changes.
8. **J3:** `refreshRules` lacks the single-flight guard given to signals/recent refreshes.
9. **Conventions:** `lwip` belongs in `PRIV_REQUIRES`; brace-style split between new files (K&R) and `web_api.cpp` (Allman); manual `#ifndef CONFIG_WEB_SSE_ENABLE` fallback deviates from sdkconfig.h norm; implicit 2 s stop budget via 80×25 ms constants deserves named totals.
10. **Docs:** `docs/sse-feasibility.md` — heartbeats alone do not detect dead connections (MSG_PEEK probe does), and stop uses a timed poll, not a join.
11. **Headroom:** route table at exactly 23/23 handlers (test-pinned); next route addition requires bumping `max_uri_handlers`.

## Pre-flash hardware checks (not commit blockers)

SSE worker stack high-water mark (item 3), SSE behavior under real browser load incl. second-tab 409 fallback, OTA-reboot reconnect flow, and the deferred interactive mock/device browser validation.

## Commit plan (approved two-commit split)

1. `feat: broadcast automation configuration changes` — bridge_events, network_mqtt sink arm, rf_automation config-sink rework, rf_console case, Unity payload test. Self-contained and compilable.
2. `feat: add bounded SSE live updates to the web UI` — web_ui SSE sources/Kconfig/CMake, app.js, host tests (CMake, sse format test, asset contracts), README Web UI section, docs/sse-feasibility.md.

Untracked `plan/` files remain uncommitted per repository convention.
