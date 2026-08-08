# Review, Repair, and Commit Pending RF Signal Work

## Summary

Perform a complete review of the uncommitted persistent recent-history and manual signal-saving features. Fix every actionable correctness, safety, or regression finding, repeat the review on the corrected diff, and create one commit only after all required gates pass.

## Review Outcome

- Complete: the full pending diff was reviewed with no unresolved correctness, safety, or regression findings.
- Complete: no source repair was required; the reviewed implementation and tests remained unchanged.
- Complete: native, sanitizer, Web contract, dual-target Unity, four-profile production, deterministic browser, and approved classic ESP32 non-transmitting hardware gates passed on the final firmware candidate.
- Complete: temporary hardware-validation records were removed and the device's Web-only service mode was preserved.

## Review and Fixes

- Audit the complete current diff, including both existing feature plans and this review plan. Do not include generated or ignored artifacts.
- Review the versioned `rf_recent/history` NVS format, CRC and bounds checks, rotation, stable IDs, corruption recovery, commit atomicity, locking, and receive-path failure isolation.
- Review catalog creation and refresh behavior, create-only conflicts, MQTT/Home Assistant catalog events, worker ownership, mutex ordering, and replay/transmit separation.
- Review UART and HTTP parsing, 64-bit IDs, decimal/hex codes, protocol and pulse bounds, exact HTTP statuses, same-origin enforcement, and route capacity.
- Review Web polling, revision refreshes, DOM safety, duplicate-name feedback, 64-bit ID handling, desktop/mobile layouts, and absence of accidental RF actions.
- Check README behavior claims against the implementation.
- Add focused regressions for every defect found. Repeat the full diff review after fixes. If any finding or required gate remains unresolved, do not commit.

## Verification

- Run `git diff --check`, JavaScript syntax checks, and all Web/source contract tests.
- Run the clean six-test native host suite plus an ASan/UBSan host build.
- Compile the Unity images for both ESP32 and ESP32-S3.
- Validate the Web UI against a deterministic Playwright mock on desktop and mobile, including successful manual save, duplicate rejection, recent-history actions, overflow, clipping, polling, and console/page errors.
- Reuse the already successful four-profile production verifier and classic ESP32 hardware results only if review changes no production input. Otherwise rerun `tools/verify-production.sh` once on the final unchanged candidate and repeat the approved non-transmitting classic ESP32 flash/monitor checks.
- Do not change the device's Web-only service mode merely to test MQTT. Validate Home Assistant reconciliation through the existing MQTT host/source contracts and report the live MQTT limitation.

## Commit

- Stage an explicit inventory containing all reviewed source, tests, documentation, and the three plan files.
- Inspect `git diff --cached --check`, the staged file list, staged diff, and staged statistics.
- Create one commit using the configured repository identity with message:
  `feat: add persistent RF signal history and manual saves`
- Do not amend or push.
- Verify the resulting commit with `git show --stat`, record its hash, and confirm the working tree is clean.

## Assumptions

- "Everything" means the complete current uncommitted feature set and its plans; no unrelated files are presently pending.
- Passing requires no unresolved review findings and every applicable automated gate succeeding.
- Existing user NVS data must remain intact, and review validation must not replay or transmit RF.
