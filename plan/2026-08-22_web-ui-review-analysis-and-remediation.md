# Web UI Review — Analysis and Remediation Plan

Date: 2026-08-22. Analysis of `plan/2026-08-21_web-ui-review.md` against current HEAD `72d86ba`,
followed by an approved remediation of its verified findings. The assets are unchanged since the
review was written, so every finding was checked directly against source; backend contract claims
were additionally checked against `web_events.cpp`, `web_api.cpp`, `ota_update.cpp`, `web_ui.cpp`,
and `host_tests/test_web_assets.mjs`.

## Verification results

### Major findings — all 10 confirmed at source except B2* (plausible, unverified)

| ID | Verdict | Evidence |
|---|---|---|
| C1 no timeout on `requestAction` fetch | ✅ Confirmed | `app.js:199`; contrast `readJson`:177 / `pollLive`:227 which use 3.5 s AbortControllers. Busy-gate strands UI. |
| D1 OTA XHR no timeout/stall watchdog | ✅ Confirmed | `app.js:1172-1210`; no `request.timeout`, no stall detection; `setBusy(true)` locks all controls. Blanket XHR timeout would risk false aborts on slow uploads; a stall watchdog is the right shape. |
| D2/J5 `renderLive` unguarded derefs | ✅ Confirmed | `app.js:724,726,739,747-756` dereference mandatory snapshot fields bare; failure path flips badge to "Disconnected" via `pollLive` catch. Secondary hazard: `setBusy(false)`→`renderLive(state.live)` runs outside any try/catch (`app.js:162-165`). Contrast: `renderSystem` is defensive throughout. |
| A1 tabs lack aria-current/tablist semantics | ✅ Confirmed | `index.html:28-32` plain buttons; `app.js:964` toggles class only. |
| A2 `setBusy` drops focus to body | ✅ Confirmed | `app.js:158-165` disables the focused control with no save/restore. |
| A3 `replaceChildren` loses focus after delete/remove | ✅ Confirmed | `renderRecent`:806, `renderSignals`:855, `renderRules`:910. |
| A4 input border ≈1.7:1 | ✅ Recomputed independently | `--line-2:#384447` vs surface `#161c1e` = 1.71:1; vs input bg `#0f1416` = 1.84:1. Below WCAG 1.4.11 (3:1). |
| A5 connection/learning badges silent to SR | ✅ Confirmed | `index.html:22`,`:59` plain spans; inconsistent with `#notice` (:37) and `#radio-hardware-feedback` (:167), which have role=status. |
| B1 tab focus rings clipped | ✅ Confirmed statically | `.tabs{overflow-x:auto}` clips vertically too; `.tabs-inner{padding:0 12px}` gives 0 px vertical room for the 4 px ring. |
| B2\* mid-width (≈821–865 px) recent-row clip | ⚠️ Plausible, UNVERIFIED | Static estimate ≈834 px minimum > 821 px window floor with `.item-list{overflow:hidden}` clipping. Settled by rendered measurement before fixing. |

### Minor findings checked

U1 (Connected during polling-only fallback) — confirmed logically at `pollLive:234`.
J1/D8 (validation-parity gaps) — confirmed; note `formBody` deliberately does not percent-encode,
so `&`/`=` in code fields inject extra body fields until the server rejects.
D3/D4 (raw repeats reads, positional selector) — confirmed (`app.js:1004,:1043` vs `:1058`).
D5/D6/D7 — all confirmed (unconditional refetches; "Firmware undefined confirmed on undefined";
hardware-switch failure notice overwriting the server's specific error).
C3 stale watchdog after SSE onerror — confirmed, benign. C5 hidden-tab OTA reconciliation stall —
confirmed, recovers on visibilitychange. C6 non-JSON error bodies leak SyntaxError text — confirmed.
B7 learning pill gets text-color-only treatment — confirmed (`state value-ok` vs `state state-ok`).
B9–B12 and B3–B8 nits — mostly confirmed (unprefixed `backdrop-filter`, `:focus-visible`-only,
never-loaded `Inter`, duplicated pulse amber literal, missing input disabled styling, magic 68 px).

Dropped findings: **C2** (coalescer-timer leak) does not hold up as stated — `closeEvents`:86 clears
`sseTimer`; **B5** ("two un-wrapped dynamic sinks") is not reproducible — grep shows zero HTML sinks
in app.js; all dynamic text flows through text nodes.

## Report-quality meta-findings

1. **Missed hard constraint:** `test_web_assets.mjs:569` caps app.js at 49,152 B; the file is
   49,097 B — 55 bytes of headroom. Every substantive fix collides with this ceiling.
2. **Contract-test coupling not mentioned:** the test pins exact strings/CSS values that fixes
   would touch (e.g. mobile `.tabs-inner{padding:0}` pin conflicts with a tab-padding fix).
3. **J3 silently dropped:** declared "re-confirmed" yet absent from the findings list and top-10
   (`refreshRules` lacks the single-flight guard given to recent/signals; `app.js:913-919`).
4. **B2\* left unverified** despite the review having a running Playwright harness.
5. **E-audit count off by one:** 6 unread OTA-status fields (`server`, `port`, `bytes`, `total`,
   `maintenance_error`, `initialization_error`), not 7. The SSE `system` event is reachable
   (kWebStarted/kWebStopped fall through `event_name()`'s default) but unsubscribed — harmless.
   Additional find: `errors.radio` is also unread. Route agreement is exact (21+2 routes, all
   called; 23/23 handler slots used, hardcoded maximum at `web_ui.cpp:119`). Test-gap priorities
   are accurate (the contract test is purely static text/regex assertion).

**Verdict:** trustworthy and actionable. Defects are omissions/compressions, not wrong analysis.

## Approved remediation scope

Majors + correctness minors: C1, D1, D2/J5, A1–A5, B1, B2\*, U1, J3, D3–D7, C3, C6, B7.
Deferred: J1/D8 validation parity, B9–B12 style nits. Dropped: C2, B5.
Size budget: app.js test ceiling raised 48 KiB → 56 KiB.

### Batch 1 — transport robustness (C1, D1, U1, C3, C6)
- `requestAction`: AbortController + 10 s timer; AbortError mapped to "Request timed out".
- OTA XHR: stall watchdog (15 s without upload progress) aborting only before `uploadComplete`;
  post-upload stalls stay governed by the existing 120 s confirmation deadline.
- `pollLive` label distinguishes SSE-connected "Connected" from polling-only "Polling".
- Clear `sseWatchdog` in `source.onerror`.
- Guard `response.json()` parse failures to `HTTP <status>` messages.

### Batch 2 — render/state correctness (D2/J5, J3, D6, D7, D3/D4)
- Fail-soft `renderLive` per section group so one malformed section cannot flip the badge.
- Single-flight guard in `refreshRules` mirroring `sigBusy`/`recentLoading`.
- Validate restored OTA-success storage fields before building the notice.
- Hardware-switch failure feedback goes to the `#radio-hardware-feedback` span instead of a second
  notice that overwrites the server's specific error (keeps the test-pinned string).
- Shared `clampRepeats()` (NaN→8, 1–20) on the three unvalidated button paths; add
  `data-role="repeats"` to the signal-list repeats input.

### Batch 3 — accessibility (A1, A5, A2, A3, A4, B1, B2\*, B7)
- `aria-current="true"` on the active tab; `role="status"` on `#connection` and `#learning-state`.
- `setBusy` preserves focus across disable/enable cycles.
- Delete/remove rebuilds restore focus to the nearest surviving row action (fallback: list
  container with `tabindex="-1"`).
- Input border raised to ≥3:1 against its surface.
- `.tab:focus-visible{outline-offset:-4px}` inset ring — unclippable, layout-neutral, avoids the
  pinned mobile `.tabs-inner{padding:0}` contract.
- B2\*: measure at ~830 px first; if confirmed, lower desktop grid floors (desktop values are not
  pinned by the contract test).
- Learning pill emits full tone-badge classes.

### Explicitly out of scope
J1/D8 client-side validation parity, B9–B12 style/token nits, E-audit field cleanup (pinned by the
contract test; zero behavior gain), any component `.cpp` change.

## Verification chain

1. `node host_tests/test_web_assets.mjs .` green after each batch.
2. ESP-IDF Tools MCP `build_project` as the iterative compiler gate.
3. All-device build validation without hardware: all four profile builds through
   `tools/build-board.sh <profile> build`, plus compile-only Unity images for esp32 and esp32s3.
   Hard boundary: no flash / monitor / erase / on-device execution of any kind.
4. Playwright MCP against a deterministic local mock serving the real assets: SSE tick + 503
   fallback (Polling badge), hung-mutation timeout, OTA stall watchdog, keyboard/focus pass,
   aria-current snapshot, widths at 390/768/830/1280, console cleanliness; full-page desktop 1280 +
   mobile 390 screenshots compared before/after. RF transmit and destructive mutations are always
   intercepted.
5. Native host suite once from a clean shell.
6. `tools/verify-production.sh` exactly once on the final unchanged candidate — required because
   embedded asset bytes change produced firmware output.

## Outcome (2026-08-22)

All items implemented and validated; no flashing or hardware access occurred.

| Gate | Result |
|---|---|
| `node host_tests/test_web_assets.mjs .` | Passed after every batch (final app.js 53,055 B / 57,344 B ceiling) |
| Profile builds (`tools/build-board.sh <p> build`) | esp32-devkit, esp32s3-devkitc-n16r8, xiao-esp32s3, esp32s3-supermini-fh4r2 — all passed |
| Unity compiles (esp32 + esp32s3, isolated /tmp) | Both passed |
| Native host suite | 8/8 tests passed |
| `tools/verify-production.sh` (single final run) | Four-profile verification passed; e.g. devkit image 0x13aff0 in 0x1e0000 slot |
| Playwright mock matrix | SSE tick → "Connected"; SSE 503 → "Polling" (U1); hung mutation aborted at ~10.0 s with "Request timed out", controls freed, focus restored (C1+A2); mid-flight OTA failure cleans up fully (D1); buffered-upload + >30 s post-upload silence produced NO false abort (D1 guard); HTML error body → "HTTP 500" (C6); delete focus falls back to surviving row button (A3); aria-current tracks tabs incl. initial state (A1); armed pill `state state-warn` (B7); corrupt restore shows "Firmware unknown confirmed" (D6); hardware-switch failure keeps server error + span feedback (D7); widths 390/768/830/1280 overflow-free; console clean on clean loads |

Findings resolved: C1, C3, C6, D1, D2/J5, D3, D4, D6, D7, J3, U1, A1–A5, B1, B7. B2\* was refuted
by rendered measurement (no clip at 821–830 px; row needs ≈810 px viewport) — no change made.
Deferred by scope: J1/D8 validation parity, B9–B12 style nits. Dropped as invalid: C2, B5.

Adversarial review subagent confirmed every fix and caught one real defect pre-release: XHR
`abort()` fires `abort`, not `error`, so the stall-watchdog path initially had no handler — fixed
with a shared `transportFailed` handler before final verification. Accepted caveats documented
there: J3 single-flight can skip one post-mutation rules re-render (matches existing signal/recent
pattern), D7 feedback text is overwritten at next poll (real firmware then reports the switch
error through `hardware_switch_error` diagnostics).

Known tooling note: ESP-IDF Tools MCP `build_project` is structurally unusable here (no root
partitions.csv; profile-owned tables) — wrapper builds used per the tooling guide.

## Hardware validation (2026-08-22, user-authorized)

Flashed `build/esp32s3-devkitc-n16r8/esp32-cc1101.bin` (the verify-production-passed candidate)
to the N16R8 via `tools/build-board.sh esp32s3-devkitc-n16r8 flash --port <approved by-id>`.
Hash verified; board reset cleanly. Browser validation ran against the live device
(`esp32-cc1101-a2d3c0.local`, advertising `version=72d86ba-dirty`), strictly read-only — no
replay/transmit/learn/delete/save/OTA actions were issued:

- SSE 409 takeover by another client → badge showed **"Polling"** with live poll data; after that
  client left, our tab acquired the stream and the badge flipped to **"Connected"** — both U1 label
  paths demonstrated on real hardware.
- `aria-current` tracked all five views exactly one-at-a-time; `role="status"` present.
- System dashboard rendered true N16R8 identity: profile `esp32s3-devkitc-n16r8`, ESP32-S3,
  7.9 MiB PSRAM, Wi-Fi Online −56 dBm / Good, OTA `idle / 72d86ba-dirty`, generic backend active.
- Console errors were only the expected SSE 409 resource entries while the slot was held; no
  application errors or warnings.
- Screenshots: `/tmp/rfbridge-ui-mock/shots/hw-desktop-system.png`, `hw-mobile-overview.png`.

## Follow-up fix: SSE-retry must not repaint the dashboard (2026-08-22)

Reported on hardware: with a second client holding the single SSE slot, every 10 s (the retry
backoff cap) the badge flipped red→green and the whole dashboard "flashed". Root cause was
two-layer: a leftover Playwright validation tab held the device's one SSE slot (operational
trigger), and `source.onerror` called `setConnection(false,"Reconnecting")`, which runs the full
red disconnected sweep over all System metrics even though polling was healthy — pre-existing
overreach that matches the review's minor 409-contention finding and only became visible under
contention. Fix: `onerror` now flags only the badge ("Reconnecting", red); the sweep remains owned
by `pollLive`'s failure path (real reachability loss). Mock-reproduced across a full retry cycle:
System metrics identical before/after, `is-stale` never engaged, no dashboard repaint. Contract
test green; image rebuilt and production verifier rerun before reflashing.
