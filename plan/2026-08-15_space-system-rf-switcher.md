# Separate the System RF Controls from Diagnostics

## Summary

Playwright measured a `0px` gap between the RF switcher row and RF service cards at both 390px and 1440px. Add a scoped 16px separation using the existing `--s-4` spacing token. The proposed spacing was previewed successfully at both widths without overflow.

## Implementation Changes

- In `app.css`, add `#system-status > .system-status-band > .settings-row + .system-grid { margin-top: var(--s-4); }`.
- Keep the selector limited to the System RF controls so the Rules settings row and other grids remain unchanged.
- Extend the Web asset contract test to require this selector and spacing token.
- Make no HTML, JavaScript, API, storage, or firmware behavior changes.

## Validation and Deployment

- Run `git diff --check`, JavaScript syntax validation, the Web asset contract, and all eight native host tests.
- Validate a deterministic mock with Playwright at 1440x1000, 390x844, and 280px minimum width. Confirm a computed 16px gap, no overflow or clipping, clean console/page state, and visually inspect before/after System screenshots.
- Block all RF, OTA, deletion, hardware-switching, and configuration mutations during browser validation.
- Build `esp32s3-devkitc-n16r8`, then run `tools/verify-production.sh` once because the embedded CSS changes the production image.
- Confirm the approved by-id symlink resolves to a character device, then flash only through `tools/build-board.sh esp32s3-devkitc-n16r8 flash --port /dev/serial/by-id/usb-EXAMPLE_N16R8-if00`.
- Rediscover `_rfbridge._tcp` and repeat read-only desktop/mobile Playwright validation against the device.

## Assumptions

- The existing flash authorization applies to the N16R8 profile and exact approved path above.
- Unity compilation is unnecessary because this is an embedded CSS and host-contract-only change.
- The unrelated untracked `docs/code-review-2026-08-14.md` remains untouched.
