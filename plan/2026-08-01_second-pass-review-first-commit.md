# Second-Pass Review and First Commit

## Context and guardrails

The native ESP-IDF firmware, host tests, and Unity test image already build, but the directory is not yet a Git repository. This pass will independently challenge the implementation rather than merely repeat the first review. It will not flash hardware or claim on-device/RF validation.

Git author name and email are currently unset. Do not invent an identity or modify global Git configuration; obtain an identity from the user (or have the user configure one) before creating the commit.

## 1. Establish a clean review baseline

- Inventory all source, test, documentation, editor, generated, and cache files before initializing Git.
- Extend `.gitignore` so `.cache/clangd`, ESP-IDF build trees, generated `sdkconfig`, binaries, maps, and compile databases cannot enter the first commit. Review `.vscode/`, `.devcontainer/`, `.clangd`, and plan files individually rather than staging blindly.
- Re-run host tests and clean production/Unity builds under ESP-IDF 6.0.2 to establish an authoritative baseline. Treat the current AFT `stddef.h` diagnostics as cross-toolchain/index configuration findings unless reproduced by the real compiler.

## 2. Perform independent deep-review tracks

### CC1101 driver — `components/cc1101/`

- Recheck manual-CS SPI framing, CHIP_RDYn handling, reset preconditions, stable status reads, state deadlines, calibration/recovery escalation, and cleanup after every partial-initialization failure.
- Audit the complete 433.92 MHz ASK/OOK register profile and PATABLE against TI documentation, including GDO routing, asynchronous mode, data rate/RX bandwidth, AGC, frequency calculation, and 26 MHz assumptions.
- Prove that every TX, timeout, stop, and deinitialization path forces GDO0/carrier inactive; recheck custom-GPIO rejection before any unsafe pin is touched.

### RMT/radio service — `components/rf_ook/`

- Trace ISR-to-task buffer ownership, generation invalidation, queue-full recovery, RX rearming, truncation detection, callback teardown, and resource cleanup for use-after-free, stale-event, or wedged-state risks.
- Audit all start/stop/reset/TX/replay interleavings, command/reply timeouts, owner-task lifetime, duplicate suppression, half-duplex recovery, and startup retry behavior.
- Recalculate classic-ESP32 RMT allocation and boundary conditions: seven RX blocks plus one TX block, 100–29000 µs pulses, 30 ms stop threshold, 256-pulse raw periods, exact repeat capacity, five-second airtime, integer overflow, and end-level behavior.

### Codec/raw logic — `components/rf_codec/`

- Compare all 12 protocol definitions against independent rc-switch golden definitions, not generated self-round-trips.
- Review segmentation, trusted boundaries, repeated-frame consensus, protocol 11/12 ambiguity, inversion, bit-width/code bounds, pulse estimation, and every multiplication/addition for overflow or narrowing.
- Stress raw period discovery, cyclic canonicalization, jitter/scale matching, fingerprint stability, embedded decoded fragments, malformed alternation, min/max durations, and 256-pulse limits.

### Console/application/configuration — `components/rf_console/`, `main/`, CMake/Kconfig

- Review every command’s argc handling, decimal/hex parsing, bounds, raw staging atomicity/capacity, stale last-frame behavior, replay/send errors, status output, and concurrent startup/retry calls.
- Check component dependencies, C++ linkage, task stack/queue sizing, boot-failure behavior, Kconfig defaults/ranges, target assumptions, and consistency with `README.md`.

Use targeted subreviews and local ESP-IDF 6.0.2 source for API contracts. Consult primary TI/ESP-IDF documentation where hardware semantics are uncertain, and label anything that remains hardware-unverified.

## 3. Fix confirmed defects and strengthen regressions

- Fix only reproducible or well-supported defects; keep changes bounded and preserve the public CLI unless correctness requires otherwise.
- Add a focused regression for every corrected logic bug. Prioritize host-testable boundary/property cases and hardware-free lifecycle helpers; add Unity tests where ESP-IDF types/APIs are required.
- Run host tests with `-Wall -Wextra -Werror` plus AddressSanitizer/UndefinedBehaviorSanitizer in a clean native shell.
- Rebuild the production image and Unity image, run `idf.py size`, inspect compiler warnings and linker/resource output, and rerun code-health/duplicate checks.
- Do not flash either image. Clearly separate compile/test evidence from unperformed on-device Unity and RF acceptance testing.

## 4. Create the first commit

- Initialize Git only after review fixes and all non-hardware gates pass.
- Stage an explicit allowlist of intentional source, tests, documentation, project configuration, tooling configuration, attribution, and plan files. Confirm no build output, cache, generated `sdkconfig`, credentials, or machine-local files are staged.
- Review `git diff --cached --check`, the complete staged file list, and staged diff/stat before committing.
- Once a valid user-supplied Git identity is available, create the root commit with message: `feat: add native ESP32 CC1101 RF tool`.
- Verify the resulting commit with `git status --short --branch` and `git log -1 --stat`; report the commit hash, verification results, review fixes, and remaining hardware-only risks.

## Completion criteria

- No unresolved critical/high software findings.
- Host tests pass normally and under ASan/UBSan.
- Production and Unity images compile under ESP-IDF 6.0.2; size remains within limits.
- Repository is clean after one reviewed root commit containing only intentional files.
- No hardware flashing or RF/on-device validation occurred.
