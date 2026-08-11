# Review, Commit, and Publish Pending RF Fixes

## Summary

Review the complete uncommitted candidate on `main`, repair any confirmed issue, validate it offline, commit the exact reviewed inventory, and push normally to `origin/main`. Hardware access remains out of scope.

## Review and Repair

1. Save this plan as `plan/2026-08-11_review-uncommitted-code-and-publish.md`.
2. Review the RF storage changes for malformed-key detection, iterator cleanup, error compatibility, catalog callers, and Unity-test cleanup.
3. Review stopped RF status reporting for persisted/runtime ordering, read-only state access, start-override behavior, error reporting, and all status consumers.
4. Check the source-contract regression for precise coverage and review both plan documents for accuracy, secrets, or machine-specific leakage.
5. Fix any confirmed issue narrowly, add or adjust regression coverage, and repeat the affected review. Do not commit while any finding remains unresolved.

## Verification

- Reuse the completed host, ASan/UBSan, syntax/contract, Unity, four-profile, and production-verifier results only while the reviewed source/test candidate remains unchanged.
- After any code or test edit, rerun `git diff --check`, the clean host suite, ASan/UBSan suite, all shell/JavaScript checks, affected contract tests, and both Unity compilation targets.
- Inspect final unstaged and staged diffs, statistics, file modes, generated-file status, and exact staged inventory.
- Playwright and hardware tests are not applicable because there are no Web changes and flashing, monitoring, OTA, RF, NVS, and on-device Unity remain prohibited.

## Commit and Push

1. Stage only the four modified implementation/test files and the two August 11 plan files; preserve any unrelated concurrent changes.
2. Commit with `fix: harden RF storage and radio status reporting`.
3. Run `tools/verify-production.sh` after committing because the Git-derived firmware version changes. If it fails, do not push; fix the cause in a follow-up commit and repeat the gate without amending.
4. Confirm a clean worktree and expected commit contents, then run a normal `git push origin main`; never force-push.
5. Verify `origin/main` resolves to the new local `HEAD` and report the commit hash, push result, verification results, and hardware limitations.

## Interfaces and Assumptions

- Preserve all CLI, HTTP, MQTT, NVS, partition, and public C++ contracts.
- Malformed learned-signal keys remain visible as `ESP_ERR_INVALID_RESPONSE`; no automatic NVS deletion is introduced.
- Stopped status reads report persisted hardware without mutating runtime backend state.
- `main` currently matches `origin/main`; every current uncommitted file belongs to this candidate.
