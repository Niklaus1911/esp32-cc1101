# Web UI Review Report — code, style, UX (Playwright-driven)

Date: 2026-08-21. Scope: `index.html` (245 lines), `app.css` (285), `app.js` (1,222), contract-checked against `web_api.cpp`/`web_events.cpp`, README, and `test_web_assets.mjs`. Method: five parallel static reviews + live headless Playwright scenario matrix against the deterministic mock (real assets, faithful API/SSE, mutations intercepted). Report-only; no code was changed.

## Verdict

**High-quality frontend with no blockers.** The XSS discipline (zero HTML sinks), OTA identity chain, route/method agreement across all four sources, and reduced-motion handling are exemplary. The main weaknesses are **accessibility gaps around dynamic state (tabs, status badges, focus loss)** and **two missing network timeouts that can strand the UI**. One new runtime finding: the connection badge conflates data availability with stream health.

| Severity | Count | Notable |
|---|---:|---|
| Blocker | 0 | |
| Major | 10 | C1, D1, D2/J5, A1–A5, B1, B2* |
| Minor | ~25 | incl. runtime-verified badge conflation |
| Nit | ~20 | style/token hygiene |

\* needs-rendered-verification (narrow 821–865 px window).

## Playwright scenario matrix results (all run live)

| Scenario | Result |
|---|---|
| Fresh load + SSE ticks, desktop/mobile/tablet 1280/768/390 | Pass — zero console errors/warnings |
| Tab navigation, all five panels | Pass — System dashboard dense but organized; Degraded pills correctly fired on mock cumulative-drop counters |
| Keyboard pass (real key presses) | Focus traverses all controls; `:focus-visible` matches with 2px accent outline |
| **Focus geometry in tab bar** | **FAILS**: `.tabs` has `overflow:auto` with 0 px above / 1 px below buttons → 4 px focus ring clipped (confirms B1) |
| SSE endpoint returns 503 | Fallback machinery fully engaged: EventSource discarded, backoff doubled to 4 s, retry scheduled, polling active. **But badge flipped back to "Connected"** via poll success (`setConnection(true,…)` app.js:234) — masks transport degradation (new finding U1) |
| Long strings (37-char hostname/SSID, 15-char rule names) @390 px | No horizontal overflow (390/390) — `overflow-wrap:anywhere` discipline holds |
| `/api/recent` returns 500 | Graceful: "Unavailable" count text + error notice, console clean apart from expected resource error |
| Mutation intercept (POST /api/replay) | Payload captured (`name=&repeats=8`), never reached hardware, success feedback correct |

## Major findings

- **C1** `app.js:199` — `requestAction` fetch has no timeout/AbortController; combined with busy-gated `schedulePoll`/retry paths, a hung POST while SSE is dead strands the UI (the only non-guaranteed cell of the transport matrix).
- **D1** `app.js:1172-1210` — OTA XHR has no timeout/stall watchdog; frozen upload locks all controls until reload.
- **D2/J5 (holds)** `app.js:724-751` — `renderLive` dereferences mandatory sections unguarded; contract drift renders as permanent "Disconnected".
- **A1** `index.html:28`, `app.js:964` — tab active state is CSS-class only; no `aria-current`/tablist semantics; SR users cannot tell the displayed view.
- **A2** `app.js:158-162` — `setBusy` disables the focused control; keyboard focus drops to `<body>` on every mutation. (Verified at source.)
- **A3** `app.js:806,855,910` — wholesale `replaceChildren` after delete/remove loses focus with no restoration.
- **A4** `app.css:73` — input border ≈1.7:1 vs surface; fields identifiable only by sub-WCAG-1.4.11 boundary.
- **A5** `index.html:22` + `app.js:169` — connection/learning state changes are not announced (no live region).
- **B1** `app.css:94,96` — tab-bar focus rings clipped (runtime-measured above).
- **B2*** `app.css:202,209` — recent-row grid minimums exceed width between ~821–865 px; `.item-list{overflow:hidden}` would clip controls.

## Minor findings (selected)

- **U1 (new, runtime-confirmed):** badge shows "Connected" during pure-polling fallback with dead SSE; consider distinguishing "Live stream" vs "Polling".
- **J1/D8 (hold):** client-side validation parity gaps — code inputs lack patterns (index.html:99,113), raw durations unchecked per-value, trigger≠target unchecked; server rejects but browser submits first.
- **D3/D4:** repeats read raw from unread-enforced inputs (min/max attributes never validated); signal list selects repeats positionally vs recent list's `data-role`.
- **D5/D6/D7:** unconditional refetches after failed actions; unvalidated OTA-success storage restore ("Firmware undefined confirmed on undefined"); hardware-switch failure message overwrites the specific server error.
- **C2/C3/C5/C6:** stale-coalescer timer nulling without clearTimeout; watchdog not cleared on onerror; hidden-tab stalls OTA reboot reconciliation; non-JSON error bodies leak "Unexpected token '<'"; 409 extra-tab case shown as generic "Disconnected".
- **B7:** Learning pill gets `state value-ok` (color-only) vs full tone-badge treatment elsewhere. **B9/B10:** unprefixed `backdrop-filter`; no `:focus` fallback for Safari <15.4. **B11:** `Inter` first in font stack but never loaded — rendering varies by OS fonts. **B12:** no `scroll-padding-top` under 106 px of sticky chrome. **B3/B4/B5/B6/B8:** disabled-field styling gap, duplicated pulse amber literal, two un-wrapped dynamic sinks, >100ch disclosure line length, magic 68px sticky offset.
- **E-audit:** unused emits are one-directional and harmless (`learning.result`, `automation.configuration_revision`, 7 OTA-status fields, `services.configured/reboot_required`, `errors.services`); `/api/rules` revision/available unread; SSE `system` event name never subscribed.

## Consistency audit summary

Route table: **exact agreement** across app.js ↔ web_api.cpp ↔ ota_update.cpp ↔ mjs (21+2 routes). Field drift strictly one-directional (no read lacks an emitter). README quantitative claims each map to a single named constant. Prior findings H7 (23/23 handlers, zero headroom), J1, J3, J5 all re-confirmed at current HEAD.

## Test gaps (prioritized)

1. SSE fallback matrix cells (409 takeover, visibilitychange close/reopen, delay reset on open, kind-listener membership).
2. Keyboard/a11y pins (dynamic aria-labels, real-button tabs, role=status notice).
3. Error-state renders (recent Unavailable, last.error branch, invalid-rule suffix, Rebooting badge).
4. Truncation behaviors (50-entry slice execution, >64 KiB discard, maxLength gates).
5. Validation-parity negatives (code grammar, durations bounds, trigger≠target).

## Top-10 improvements

1. Add 3.5 s abort to `requestAction`; set XHR `timeout` for OTA upload (C1+D1).
2. Fail-soft `renderLive` per-section like `renderSystem` (D2/J5).
3. `aria-current="true"` on active tab; `role="status"` on connection/learning badges (A1+A5).
4. Preserve focus across `setBusy` and list rebuilds (A2+A3).
5. Input border ≥3:1 (A4); tab-bar vertical padding for focus rings (B1).
6. Client validation parity: code patterns, duration bounds, trigger≠target pre-checks (J1/D8).
7. Badge semantics: show "Polling" during SSE outage instead of "Connected" (U1).
8. Clamp replay repeats before send; use `data-role` selectors consistently (D3+D4).
9. Resolve B2 mid-width clip risk (lower grid floors or reflow at 820 px block).
10. Style decisions: load or drop `Inter`; normalize weights; `-webkit-backdrop-filter`; `:focus` fallback; promote pulse amber to a token.

## Limitations

Single-browser (Chromium) coverage; Safari/Firefox specifics are static-analysis inferences. Contrast values computed from declared hexes, not rendered pixels. Mock responses modeled on `web_api.cpp` — device-level divergence remains possible until authorized hardware validation.
