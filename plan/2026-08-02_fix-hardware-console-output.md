# Hardware Console Output Fixes

## Context

The first hardware OTA/status run is functionally healthy, but it exposed three presentation defects:

- `status` logs `Could not format dashboard row labels=Last frame,Console drops` and omits that row because `Console drops` exceeds the two-column label width.
- Every UART OTA progress event is separated by a blank line. The first progress line needs a leading separator to avoid overwriting the active `rf> ` prompt, but subsequent progress lines can be contiguous.
- The pretty Wi-Fi disconnect record reports `Disconnected ... | ONLINE` before the following `RETRY WAIT`/`OFF` transitions, which reads as a stale current state.

Framework ESP-IDF logs may still interleave with the prompt and application events. This plan does not delay the console for optional Wi-Fi startup, manipulate linenoise's edit buffer, or suppress native framework logs.

## Implementation

1. **Repair dashboard geometry**
   - Adjust the bounded two-column formatter to fit the longest existing label, `Console drops`, while preserving the 78-visible-column dashboard width.
   - Rebalance the two-column label/value widths and full-width value width without truncating identifiers.
   - Add formatter coverage for the new exact label/value limits and the `Last frame`/`Console drops` row.

2. **Compact OTA progress output**
   - Track whether the console worker is inside one OTA progress series.
   - Keep the leading separator before the first progress line for prompt safety.
   - Emit later progress lines contiguously with one newline, avoiding blank lines between 0%, 6%, 13%, and later updates.
   - Reset the series on completion, failure, and server lifecycle boundaries. Keep plain `OTA PROGRESS bytes=... total=...` records unchanged.

3. **Clarify Wi-Fi disconnect presentation**
   - Preserve the existing plain `WIFI DISCONNECTED ... state=...` record for machine-readable compatibility.
   - Remove the stale state suffix from the pretty disconnect sentence; let the subsequent state event communicate `RETRY WAIT`, `OFF`, or `FAULT`.
   - Add focused coverage for the pretty event wording if the current test boundary permits it without exposing private runtime state.

4. **Document observed behavior**
   - Keep the README focused on supported `tio` behavior and note that asynchronous ESP-IDF logs can appear after the prompt or between application lines.
   - Do not change RF, Wi-Fi retry, OTA streaming, maintenance ownership, or reboot sequencing.

## Verification

- Run focused host and Unity formatter tests, including dashboard width and exact plain records.
- Run normal and ASan/UBSan host suites plus mocked uploader tests.
- Run `tools/verify-production.sh`, a clean Unity build, `bash -n`, and `git diff --check`.
- Recheck console/event-worker stack usage and production image margin.
- With explicit hardware approval, use `tio` to verify contiguous UART OTA progress, a clean `status` dashboard with no formatter error, and a disconnect sequence without the misleading `ONLINE` suffix. Confirm plain mode remains unchanged.
