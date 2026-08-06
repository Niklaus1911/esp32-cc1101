# Clean, Commit, And Push

## Summary

Remove the four untracked validation screenshots, preserve the two existing feature commits, record this plan as the required tracked plan document, verify the complete repository, and push `main` to `origin` with a fast-forward-only update.

## Execution

1. On the implementation turn, save this plan body as `plan/2026-08-06_cleanup-commit-push.md`.
2. Recheck the worktree and remote:
   - Confirm only the four named `n16r8-*.png` files are untracked.
   - Confirm `main` remains two commits ahead of `origin/main`.
   - Run whitespace and commit-diff checks.
3. Move only these files to the desktop trash using the available `gio` utility:
   - `n16r8-desktop-1440.png`
   - `n16r8-desktop-full.png`
   - `n16r8-mobile-390-visual.png`
   - `n16r8-mobile-390.png`
4. Leave `.vscode/`, `build/`, generated `sdkconfig` files, `managed_components/`, and other ignored machine-local artifacts untouched. Do not modify `.gitignore`.
5. Run the repository verification gates:
   - Native host build and `ctest --test-dir ... --output-on-failure`.
   - `tools/verify-production.sh` for classic ESP32, ESP32-S3 N16R8, and XIAO ESP32-S3.
   - `bash -n tools/build-board.sh tools/verify-production.sh tools/push-ota.sh`.
   - Validate local `.vscode/tasks.json` with `jq empty` without tracking it.
6. Add and commit the persisted plan file as:
   - `docs: record cleanup and push plan`
   Existing commits `a04eebb` and `5bc951b` remain unchanged and are not squashed or rewritten.
7. Before pushing, recheck the remote head. Push with ordinary `git push origin main`; never force-push. If `origin/main` advances or the push is non-fast-forward, stop and report rather than rebasing or overwriting remote history.
8. Verify the final state:
   - `main` and `origin/main` resolve to the same commit.
   - `git status --short --branch` is clean.
   - The four screenshots are absent from the repository and recoverable in the desktop trash.

## Acceptance Criteria

- All three production profiles still pass their clean build, image, partition, flash-size, and board-descriptor checks.
- Host tests pass without failures.
- No generated or device-specific editor files are committed.
- GitHub contains the two existing feature commits plus the plan-document commit.
- No hardware is flashed, monitored, or otherwise accessed during this cleanup.

## Assumptions

- The four root-level screenshots are disposable generated validation artifacts, as documented by the repository tooling guide.
- The existing two commits are the intended history and should remain separate.
- `.vscode/tasks.json` is a local convenience file containing machine-specific serial paths and must remain ignored.
