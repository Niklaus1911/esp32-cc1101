# Complete Code Review Plan — esp32-cc1101

Date: 2026-08-21. Report-only engagement; no production code is modified.

## Role and objective

Act as a senior embedded-firmware reviewer. Perform a complete, evidence-based code review of this repository and produce an accurate written report saved to `plan/2026-08-21_complete-code-review.md`.

## Hard ground rules

- Strictly read-only on the repo: no file edits to production code, no commits, no `menuconfig`/`set-target`, no generated-`sdkconfig` edits, nothing under `managed_components/` or `~/.espressif`.
- No hardware: no flash, monitor, erase, or on-device tests. No RF timing/range/NVS-persistence claims may be presented as validated.
- Editor/clangd diagnostics are non-authoritative. Every claim must be verified by reading the actual source.
- Review the working tree as-is, including uncommitted changes; use `git diff` for context on modified files.
- Host tests may be run from a clean shell to confirm existing behavior; building firmware is optional and never gates findings.

## Scope inventory (verified at Phase 0)

- 18 components under `components/` plus `main/`: ~32,000 lines of C/C++; ~2,700 lines of Web assets (JS/CSS/HTML).
- `host_tests/` (portable C++17) and `host_tests/test_web_assets.mjs`; `test_apps/unit/` Unity image (compile-level only).
- Board profiles under `boards/<profile>/sdkconfig.defaults`, partition tables, `sdkconfig.defaults`, `tools/*.sh`.
- Uncommitted change set (~506 insertions, 16 files) centered on a new `web_events`/SSE path (`web_events.cpp/.hpp`, `web_api.cpp`, `app.js`, `bridge_events`, `rf_automation`, `rf_console`) — given proportionally deep attention.

## Review dimensions

1. Correctness: logic errors, off-by-one, wrong bounds, unreachable/error paths.
2. Concurrency: queue ownership, ISR safety, task/lock discipline, RMT RX/TX lifecycle, half-duplex pauses, cooldown/generation invalidation.
3. Memory safety: buffer bounds, truncation, integer overflow, uninitialized values, leak paths (RMT/SPI/NVS/HTTP handles), failed-cleanup paths.
4. Error propagation: `esp_err_t` discipline, fail-closed behavior for corrupt NVS records, no silent defaulting of unreadable state.
5. Security posture: unauthenticated trusted-LAN surface, same-origin/Host validation, header parsing bounds, secret handling, OTA image validation chain.
6. Convention compliance (AGENTS.md): naming (`snake_case`/`PascalCase`/`kPascalCase`/`s_`), namespace `rfbridge`, four-space style, fixed-width ints.
7. Documentation drift: README numeric contracts vs. code constants.

## Execution phases

- Phase 0 — Baseline: `git status --short`, inventory LOC/files, map README claims to owning components.
- Phase 1 — Doc-vs-code audit: verify every numeric/behavioral contract against implementation constants and parsers.
- Phase 2 — Cluster deep reviews (parallel subagents A–I), each returning structured findings with `file:line`, quoted evidence, severity, confidence:
  - A: `rf_codec` + codec host tests
  - B: `cc1101` + `rf_ook`
  - C: `rf_storage` + `platform_nvs`
  - D: `rf_automation`
  - E: `rf_console`
  - F: `network_wifi` + `network_mdns` + `network_mqtt`
  - G: `ota_update` + `app_maintenance`
  - H: `web_ui` (incl. new SSE/web_events code) + `web_auth`
  - I: `bridge_control` + `bridge_events` + `rf_signals` + `main`
- Phase 3 — Cross-cutting pass: concurrency, overflow, leaks, error paths, PSRAM/internal-RAM split claims, security posture summary.
- Phase 4 — Test-gap analysis.
- Phase 5 — Verification and dedup: personally re-read every candidate finding at its cited location before inclusion; discard false positives; label anything unverifiable statically as "needs verification".
- Phase 6 — Report written to `plan/2026-08-21_complete-code-review.md`.

## Report format (accuracy contract)

- Executive summary: overall health verdict, top risks, counts by severity.
- Findings table + details: ID, severity (Critical/Major/Minor/Observation), confidence (Confirmed/Needs-verification), component, `file:line`, quoted evidence, impact, suggested direction (described, not applied).
- Separate sections: documentation drift, convention violations, test gaps, security posture.
- Explicit limitations section: static analysis only.
- No padding: sections may be empty; speculation is forbidden.

## Out of scope

Fixes, refactors, commits, flashing, monitors, `verify-production.sh`, hardware or browser-on-device validation.
