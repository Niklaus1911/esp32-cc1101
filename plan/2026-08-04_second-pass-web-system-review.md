# Second-Pass Web System Review and Hardening

## Summary

Run an independent second review of the uncommitted System-dashboard work, fix the confirmed health-state and compatibility defects, repeat the complete validation matrix, commit the entire intended change set, then rebuild and flash that exact clean commit.

No API fields, routes, persistent data, or Web controls will be added.

## Implementation Changes

- Treat CC1101 status timeouts during active transmission or maintenance as an expected paused state:
  - RF and CC1101 badges become amber, not red.
  - Display chip status as temporarily deferred while retaining the reported timeout.
  - The same timeout while idle remains a real red fault.
- Make the renderer tolerate the previous additive `/api/live` schema:
  - Missing `cc1101`, `system`, or new diagnostic fields render as `-` or "Diagnostics unavailable."
  - Keep the HTTP connection and existing Overview data active.
  - Mark affected System sections amber as "Limited," never falsely disconnected.
- Apply the selected until-reset health policy:
  - Amber RF for truncations, RF queue/command timeouts, recoveries, and CC1101 ready/state timeouts.
  - Amber Wi-Fi for event drops.
  - Amber services for learning queue/catalog errors and automation TX, queue, or log drops.
  - Do not degrade for duplicates, cooldown suppression, log-event counts, stale/ambiguous matches, or the normal startup reset count.
  - Current hard failures remain red.
- Remove the redundant previous-snapshot System render on every successful poll.
- Keep `app.js` strictly below 32 KiB by simplifying existing rendering code rather than raising the limit.
- Update README health semantics and append the second-pass results to the existing implementation record.
- Retain and commit the original implementation plan too.

## Review and Tests

- Use parallel agents for frontend fixes, backend/test-bound review, and a final read-only integration review; the primary agent owns reconciliation, hardware work, and Git.
- Extend host tests for maximum-length quote/backslash SSIDs, exact JSON-escape boundaries, IPv4 limits, and truncation rejection.
- Strengthen Web contracts for expected-busy handling, missing-diagnostics fallbacks, counter classification, single-render polling, API nesting, and the unchanged asset limits.
- Run Playwright MCP without new repository dependencies against:
  - Healthy and genuine CC1101-fault states.
  - Transmitting/maintenance status timeout.
  - Prior-schema and partially missing diagnostics.
  - Each selected cumulative fault-counter category.
  - Normal counters that must remain healthy.
  - Low heap, live timeout, disconnect/reconnect, OTA status, and disclosure persistence.
  - Widths 280, 390, 560, 820, and 1280 with no overflow, clipping, overlap, console errors, or mutation requests.
- Run JavaScript syntax, asset contracts, host CTest, ESP-IDF MCP build, out-of-tree Unity compilation, `git diff --check`, and `tools/verify-production.sh`.

## Commit and Deployment

- Stage only the reviewed dashboard implementation, tests, README, and both plan files.
- Inspect the staged diff, then commit once as `feat(web): expand system diagnostics`; do not push.
- Rebuild from the clean commit so firmware provenance uses the new short commit hash.
- Reconfirm the classic ESP32 CP2102 path is present and unused, then perform the authorized normal wired flash without erasing NVS, changing target, cleaning, or opening a serial monitor.
- Validate `192.0.2.17` returns the clean commit version, healthy CC1101/Wi-Fi/runtime data, stable polling, and responsive mobile/desktop System screenshots.
- Finish with a clean Git worktree and report the commit hash, verification results, live firmware version, and screenshot paths.

## Assumptions

- Cumulative operational fault counters remain amber until the next ESP32 reset.
- No npm/package dependency or durable Playwright suite will be introduced; browser behavior remains validated through Playwright MCP.
- The full embedded serializer will not be broadly refactored solely for host testing; helper boundary tests, stronger source contracts, production builds, live JSON parsing, and browser fixtures provide the scoped coverage.
- No RF transmission, reset-button interaction, serial monitoring, NVS erasure, or Git push is authorized or required.

## Implementation Results

- Expected CC1101 status deferrals during RF transmit or maintenance now remain amber while retaining the ESP-IDF error. The same errors while idle remain red.
- Previous-schema detection now uses diagnostic field sentinels rather than object property counts. Missing data renders as `-` or "Diagnostics unavailable," and a hard Wi-Fi error always labels the badge `Faulted` even if the retained state string is `online`.
- Cumulative health classification covers the selected RF, CC1101, Wi-Fi, learning, and automation counters while excluding normal duplicates, reset counts, stale/ambiguous matches, cooldown suppression, and successful log-event counts.
- Host coverage now exercises a worst-case 32-byte quote/backslash SSID, its exact 65-byte output capacity, atomic one-byte-short rejection, maximum/minimum IPv4 values, and invalid output capacities. Web source contracts cover API nesting, compatibility sentinels, health counter selection, busy-state deferral, Wi-Fi fault labelling, and one System render per successful poll.
- JavaScript syntax, responsive asset contracts, `git diff --check`, host CTest (3/3), the ESP-IDF MCP build, the out-of-tree Unity image, and `tools/verify-production.sh` passed. The production image is 1,049,770 bytes with 47% free in each OTA slot; `app.js` is 32,390 bytes, 378 bytes below its strict 32 KiB limit.
- Deterministic Playwright fixtures covered 21 healthy, hard-fault, expected-busy, previous-schema, selected/excluded counter, low-memory, disconnect, and reconnect cases. Widths 280, 390, 560, 820, and 1280 had no horizontal overflow, clipped status/control text, or offscreen System content. Disclosure state survived polling and reconnect, no mutation was sent, and the only console error was the deliberately injected `ERR_TIMED_OUT` transport failure.
- Final local screenshots are preserved at `/tmp/esp32-cc1101-playwright/system-after-local-{mobile,desktop}.png`. Exact-commit flashing and live device validation follow the commit and are reported in the implementation handoff.
