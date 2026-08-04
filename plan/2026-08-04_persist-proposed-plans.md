# Persist Proposed Plans

## Summary

Update `AGENTS.md` so every final proposed plan is automatically preserved in the existing Git-ignored `plan/` directory, including later revisions.

## Changes

- Add a `Plan Persistence` section requiring plan files named `YYYY-MM-DD_<descriptive-kebab-case-slug>.md`, matching the existing convention.
- Save only the Markdown plan body, without the `<proposed_plan>` rendering tags.
- Revisions of the same plan must update the original file in place and retain its filename.
- Distinct plans receive distinct files; append `-2`, `-3`, and so on if a name already belongs to another plan.
- Do not request confirmation or announce routine plan-file writes. Report only failures or deferrals.
- Never delete or rename a plan file unless the user explicitly requests it.
- If a higher-priority mode prohibits writing when the plan is proposed, retain the plan and save it before any implementation in the next write-enabled turn.
- On that next turn, first save this plan as `plan/2026-08-04_persist-proposed-plans.md`, then update `AGENTS.md`.

## Verification

- Confirm the saved plan matches the final proposed Markdown body.
- Inspect the focused `AGENTS.md` diff and run `git diff --check`.
- Confirm `git status --short` shows only the intended tracked documentation change; `plan/` remains ignored.
- No firmware builds or tests are needed for this documentation-only update.

## Interfaces

No firmware, API, storage, or hardware behavior changes.
