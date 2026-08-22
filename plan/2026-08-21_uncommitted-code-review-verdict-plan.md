# Uncommitted-Code Review & Commit-Readiness Verdict Plan

Date: 2026-08-21. Scope: the uncommitted SSE live-updates feature plus its automation config-event fan-out (16 modified files, ~506 insertions; 6 new files). Verdict-only engagement: blockers and non-blockers are reported, no fixes applied. If READY, two logical commits are prepared per user instruction (1: automation config-event plumbing; 2: SSE stream + web assets + docs).

## Phases

- Phase 0 — Snapshot & drift check: hash the diff, record file list, confirm nothing moved since the 2026-08-21 full review so prior findings map onto hunks.
- Phase 1 — Static diff review hunk by hunk, seeded with known findings from the full review (H1 fd-recycle race, H2 half-open slot hold, H3 stop-timeout half-stopped state, H4 stack headroom, H5 sequence-0 resync, D1 silent config-notification drop, convention items). Classify each as blocker / non-blocking / accept-as-is; hunt for anything missed at diff granularity.
- Phase 2 — Wiring & docs consistency audit: CMakeLists/Kconfig/source wiring, asset-API contract, README Web UI section vs behavior, docs/sse-feasibility.md claims vs reality, feature-plan doc vs shipped result.
- Phase 3 — Test gates: host tests from clean shell; node test_web_assets.mjs; Unity image compile esp32 + esp32s3; iterative compiler check of an affected profile via tools/build-board.sh.
- Phase 4 — Browser validation against a deterministic local mock (desktop + mobile visuals, SSE connect/fallback flows, intercepted mutations). If browser tooling is unavailable in the environment, defer with documented reason rather than claim coverage.
- Phase 5 — tools/verify-production.sh once on the final unchanged candidate. Concrete reason: firmware source, embedded assets, component/build metadata, and Kconfig all change production output. Verdict-only mode keeps the candidate unchanged throughout, so one run is valid.
- Phase 6 — Verdict report saved under plan/: READY / NOT READY to commit, numbered blockers vs non-blockers, suggested two-commit split with messages.

## Decision criteria for READY

All gates green; zero unresolved blockers; docs consistent; conventions satisfied; new behavior covered by added tests.

## Constraints

No code fixes (verdict only). Mock-only browser validation. Commits executed only if verdict is READY, excluding untracked plan/ files.
