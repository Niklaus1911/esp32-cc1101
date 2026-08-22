# Web UI Review Plan — code, style, UX (Playwright-driven)

Date: 2026-08-21. Dedicated review of the RFBridge trusted-LAN Web UI frontend. Report-only by default; mock-only (no device access); a11y target is best-effort WCAG AA.

## Scope

- `components/web_ui/assets/index.html` (245 lines): semantics, landmarks, forms, ARIA.
- `components/web_ui/assets/app.css` (285 lines): architecture, breakpoints 320/560/820, contrast, motion.
- `components/web_ui/assets/app.js` (1,222 lines): state, SSE/polling lifecycle, renderers, OTA flow.
- `host_tests/test_web_assets.mjs`: coverage-gap check.
- `web_api.cpp` response shapes as contract reference. All five tabs in scope.

## Phases

- Phase 0 — Baseline: deterministic mock server (real assets, faithful API/SSE mocks, mutations 403), baseline screenshots desktop 1280 / mobile 390 / tablet 768.
- Phase 1 — Five parallel static subagents: A HTML semantics/a11y; B CSS architecture/style; C app.js lifecycle; D app.js renderers/forms; E contract cross-check (assets ↔ API ↔ README ↔ tests).
- Phase 2 — Playwright scenario matrix (single headless browser, sequential): fresh-load+SSE ticks, SSE-rejected fallback, second-client 409, long strings, max items, degraded JSON errors, tab navigation, keyboard-only pass, intercepted mutation payload asserts — across desktop/mobile viewports with console+network assertions per cell.
- Phase 3 — Visual acceptance per AGENTS.md: hierarchy, density, scanability, navigation discoverability, first-viewport value, whitespace, scrolling; desktop↔mobile↔tablet comparison; ranked UX issue list referencing screenshots.
- Phase 4 — Personal verification of every finding at file:line; false-positive removal; Blocker/Major/Minor/Nit/A11y classification; runtime-dependent items marked needs-verification.
- Phase 5 — Report at `plan/2026-08-21_web-ui-review.md`: verdict, findings tables, screenshot-referenced UX issues, a11y summary, test gaps, top-10 improvements.

## Constraints

Report-only; no production fixes. Mock-only browser target; RF/destructive mutations never reach hardware (mock 403 + Playwright interception).
