# Complete Repository Code Review and Autofix

## Summary

Review the entire current repository at `dfc016d`, fix every confirmed correctness, safety, security, reliability, resource, test, tooling, or documentation defect, and leave the fixes uncommitted. Preserve existing CLI, HTTP, MQTT, partition, and NVS contracts unless a confirmed defect makes compatibility handling unavoidable.

Hardware is explicitly out of scope: no flash, serial monitor, OTA, RF transmission, device browser validation, NVS reads, erase, or on-device Unity execution. Only offline tests, deterministic mocks, and firmware builds are allowed.

## Review And Remediation

1. Freeze the clean baseline and inventory all tracked firmware, tests, Web assets, CMake/Kconfig/defaults, dependency locks, partition definitions, scripts, and documentation. Review managed components only at their integration boundaries; do not edit `managed_components/`, generated `sdkconfig`, `build/`, or `.playwright-mcp/`.
2. Re-audit the stale `plan/2026-08-05_complete-repository-code-review.md` findings against current source rather than assuming they remain applicable.
3. Trace startup, shutdown, retry, reconnect, maintenance, and failure paths across NVS, bridge events, RF/RMT, automation, UART, Wi-Fi, mDNS, Web, MQTT, and OTA. Check ownership, locks, queues, atomics, callbacks, task lifetimes, tick arithmetic, cleanup, and status consistency.
4. Audit RF and persistence boundaries for timing/lifecycle invariants, input bounds, half-duplex ownership, malformed/corrupt NVS records, power-loss behavior, replay/automation safety, and the no-automatic-erase policy.
5. Audit all network and input surfaces for bounded parsing, secret handling, Host/Origin validation, HTTP lifecycle, JSON/HTML escaping, MQTT retain/QoS/fragmentation/reconnect behavior, OTA identity validation, and browser polling/recovery.
6. Audit resource and release behavior: internal-RAM fragmentation, task/queue sizing, allocation rollback, static memory, compiler warnings, CMake dependencies, Kconfig defaults, partition/image contracts, shell quoting/error handling, dependency locks, generated-file hygiene, documentation contracts, and accidental local-path/secret exposure.
7. Record each finding with severity, exact source evidence, impact, reproduction path, root cause, and regression strategy. Fix every confirmed finding, including low-severity contract/test/tooling/documentation defects; exclude cosmetic churn, speculative refactors, and unsupported redesigns.
8. Apply the smallest root-cause patch, add a focused host or Unity regression where practical, update affected documentation/contracts, run the affected build/test, and repeat the local subsystem review before taking the next finding. Hardware-only observations remain explicitly unverified rather than being “fixed” speculatively.

## Verification

Run all checks in isolated temporary locations and never use hardware:

- Clean native host suite in `/tmp/esp32-cc1101-host-tests`.
- Separate host ASan/UBSan C/C++ build with `-fsanitize=address,undefined` and `-fno-omit-frame-pointer`, then the complete CTest suite.
- Shell and JavaScript syntax checks, followed by all contract tests.
- Both isolated ESP-IDF Unity image builds using the documented ESP32 and ESP32-S3 defaults/locks; compilation only.
- Build all four production profiles only through `tools/build-board.sh <profile> build`; never invoke `flash` or `monitor`.
- If production inputs change, run `tools/verify-production.sh` once on the final unchanged candidate.
- For Web changes, validate a deterministic local mock with Playwright at desktop and mobile widths. Intercept RF, destructive, configuration, and OTA mutations; check polling/reconnect behavior, same-origin headers, overflow, clipping, screenshots, page errors, and console errors.
- Finish with `git diff --check`, a full second review of the final diff and affected call chains, generated-file inspection, and a residual-risk report.

## Completion Criteria

- No confirmed defect remains without a documented blocker or explicit hardware-only limitation.
- All applicable automated gates pass, all four offline production profiles build, and final production verification passes when required.
- Existing public contracts and persistence semantics remain compatible or have explicit additive/migration coverage.
- The final handoff lists findings and fixes, changed files, exact verification commands/results, production-image status, and unverified hardware behavior.
- Leave the resulting changes uncommitted and do not push.
