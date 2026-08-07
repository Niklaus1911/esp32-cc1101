# Limit Production Verifier Usage

## Summary

Update `AGENTS.md` so `tools/verify-production.sh` is treated as an expensive, risk-based final gate rather than a routine check. Preserve its authority for deployable firmware while explicitly skipping it for documentation, planning, Git cleanup, and other non-production changes.

## Implementation

- Persist this plan as `plan/2026-08-06_limit-production-verifier-usage.md` before implementation.
- Add one consolidated verifier policy under Environment and Commands:
  - Use host tests, syntax checks, MCP builds, or `tools/build-board.sh <affected-profile> build` during iteration.
  - Run the full verifier only once on the final unchanged candidate when production firmware inputs, board/build configuration, partitions, dependencies, flash/PSRAM settings, image constraints, verifier contracts, or a new binary intended for flash/OTA/release are affected.
  - Skip it for plans, documentation, comments, Git cleanup, local editor configuration, and test-only changes that cannot alter production output.
  - Reuse a successful result for the same revision; rerun only after a relevant change or an incomplete/failed verification.
  - State the concrete reason before invoking it.
- Reconcile the existing MCP paragraph so it still identifies the verifier as the final clean multi-profile gate, but only when the new criteria apply.
- Do not alter the verifier script, README, tooling documentation, or build behavior.

## Validation

- Inspect the complete `AGENTS.md` verifier guidance for contradictions or accidental unconditional language.
- Run `git diff --check` and focused text searches for every `verify-production` reference in `AGENTS.md`.
- Do not run `tools/verify-production.sh`, firmware builds, or hardware operations for this documentation-only policy change.

## Assumptions

- "Production firmware inputs" includes firmware source, embedded Web assets, component/build metadata, board profiles, partition tables, defaults, dependency locks, and image-validation rules.
- The verifier remains mandatory when those inputs materially change or an unverified deployable image is being prepared; this rule only prevents unnecessary and duplicate runs.
- No public firmware API or user-visible behavior changes.
