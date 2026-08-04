# Restore interactive console editing

## Goal

Remove the failed linenoise dumb-mode workaround and restore the original ESP-IDF REPL behavior:

- command history;
- up/down navigation;
- left/right cursor editing;
- tab completion;
- normal escape-sequence handling instead of displaying `[A` or related input artifacts.

Keep named NVS learning, `learn list`, `forget`, named replay, the 30-second timeout, create-only names, and the 4 MB flash configuration unchanged.

## Changes

1. **Restore stock linenoise mode** — `components/rf_console/rf_console.cpp`
   - Remove the direct linenoise header dependency and `linenoiseSetDumbMode(1)` call.
   - Restore the original newline formatting used before the workaround.
   - Restore the normal REPL startup/ready-message ordering.
   - Remove only workaround-specific behavior; retain the reviewed marker-first learning window, worker-side RF/RX revalidation, queue-overflow cancellation, and NVS logic.

2. **Correct documentation** — `README.md`
   - Remove the statement that history, arrows, and tab completion are intentionally disabled.
   - Continue documenting UART0 at 115200 baud and the existing command syntax.
   - Keep `replay <name> <repeats>` explicit: `replay motore_on` remains invalid, while `replay motore_on 8` is valid.

## Subagent orchestration

Use subagents as parallel read-only reviewers so the small restoration is not slowed by overlapping edits:

1. **Console restoration reviewer**
   - Inspect the focused `rf_console.cpp` diff immediately after the primary edit.
   - Confirm normal linenoise mode/history/completion are restored.
   - Confirm learning order, timeout, RF/RX revalidation, queue-overflow cancellation, NVS behavior, and replay compatibility were not accidentally reverted.
   - Report only confirmed critical/high blockers and exact corrections.

2. **Documentation and command reviewer**
   - Check README text against actual parser behavior, especially `replay <name> <repeats>`.
   - Confirm the restoration does not claim asynchronous prompt-safe output that stock ESP-IDF linenoise cannot guarantee.
   - Audit the intended two-file staging allowlist and generated-file hygiene.

3. **Final independent reviewer**
   - After tests/builds pass, recheck the complete diff and verification results.
   - Return `PASS` only if no critical/high blocker remains.

The primary agent owns all edits, reconciles reviewer findings, and reruns focused checks after any correction. Independent shell gates run in parallel where safe: host tests, sanitizer tests, production build, and Unity build.

## Verification

- Run normal and ASan/UBSan host tests.
- Build clean production and Unity images under ESP-IDF 6.0.2.
- Confirm no compiler warnings, `git diff --check` is clean, and image metadata remains 4 MB.
- Do not flash automatically.
- With separate hardware approval, verify:
  - up/down history no longer prints `[A`/`[B`;
  - left/right editing and tab completion work;
  - `learn list` still returns stored names;
  - `replay motore_on 8` loads and transmits the named record;
  - no NVS data is erased or rewritten by the restoration.

## Commit

After verification, create one focused commit:

```text
fix: restore interactive console editing
```

Stage only `components/rf_console/rf_console.cpp` and `README.md`, then report the commit hash and clean working-tree status.
