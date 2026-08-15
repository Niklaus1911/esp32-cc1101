# Harden and Deploy the Modernized Web UI

## Summary

- Keep the current HTML/icon/input-mode modernization, while fixing review findings in `app.css`: the hidden OTA disclosure marker, marginal muted-text contrast, excess rounding, nonzero letter spacing, decorative radial background, and trailing blank lines.
- Preserve all DOM IDs, API routes, JSON contracts, firmware behavior, and existing user work.
- Baseline: all 8 host tests pass; the N16R8 image builds at `0x13a210` bytes with 84% OTA-slot space free. `git diff --check` currently fails on the CSS EOF.

## Implementation Changes

- Set `--text-3` to `#849094`; render stale diagnostics with `--text-2` at the existing opacity so normal text remains above 4.5:1 contrast.
- Set framed-surface radius to 8 px, normalize all letter spacing to `0`, and use a solid page background.
- Restore the firmware `<summary>` to `display: list-item` so its native marker remains visible, keyboard-operable, and styled.
- Remove trailing blank lines and keep both embedded assets below their 20 KB limits.
- Extend `host_tests/test_web_assets.mjs` to require the firmware summary's native list-item behavior. No public API, type, storage, or wire-format changes.

## Validation and Deployment

1. Inspect the final diff and run `git diff --check`, JavaScript syntax validation, the Web asset contract, and the complete clean native host-test workflow.
2. Use Playwright MCP with deterministic mocked APIs across all five views at 1440x1000, 390x844, and the 280 px minimum. Compare full-page before/after screenshots and check focus states, disclosure discoverability, polling failure/recovery, clipping, overflow, console errors, and page errors.
3. Intercept every mock mutation, verify same-origin and form-content headers, and block RF replay/transmit, OTA, hardware switching, deletion, and configuration changes.
4. Build `esp32s3-devkitc-n16r8` through `tools/build-board.sh`.
5. State that embedded HTML/CSS changed, then run `tools/verify-production.sh` once on the final unchanged candidate.
6. Require the approved symlink to exist and resolve to a character device, then flash only with:
   `tools/build-board.sh esp32s3-devkitc-n16r8 flash --port /dev/serial/by-id/usb-EXAMPLE_N16R8-if00`
7. Discover `_rfbridge._tcp` with bounded `avahi-browse`, then use Playwright read-only against the device. Confirm the N16R8 profile, all views, polling, desktop/mobile layouts, overflow, and browser/network errors while blocking all mutations.
8. Finish with `git status --short` and the relevant diff. Do not commit, erase flash, open a monitor, or run on-device Unity tests.

## Assumptions

- The flash authorization applies only to the documented N16R8 profile and exact supplied path.
- `/dev/serial/by-id` is currently absent; flashing and device-browser validation must stop cleanly if it has not reappeared.
- Unity compilation is skipped because the candidate changes only embedded HTML/CSS and a host contract test.
- `docs/code-review-2026-08-14.md` remains untouched as unrelated user-owned work.
- If the device does not advertise Web service after flashing, report device Playwright validation as deferred without changing its persisted service mode.
