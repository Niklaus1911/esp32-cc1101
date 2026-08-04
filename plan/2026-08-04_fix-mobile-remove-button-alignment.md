# Fix Mobile Rule Remove Button Alignment

## Summary

Correct the Rules page "Remove" button on mobile with a CSS-only change. Playwright confirmed the shared mobile action grid constrains the button to 76 px, shifting its text about 9 px right; a rule-specific override produced exact centering from 280-560 px without changing desktop layout.

## Implementation Changes

- Inside the existing `max-width: 560px` media query, override `#rule-list .item-actions` to use a single `1fr` column.
- Keep learned-signal controls on their existing `76px 1fr 1fr` grid and preserve the intrinsic, right-aligned desktop rule button.
- Add a focused Web asset contract asserting that the rule-specific mobile override remains present.
- Make no JavaScript, REST API, storage, firmware-control, or documentation changes.

## Subagent Orchestration

- Assign one subagent exclusive ownership of the CSS change.
- Assign a second subagent exclusive ownership of the Web asset contract test.
- Have the primary agent review both focused diffs, resolve integration issues, and own Playwright, build, OTA deployment, and final verification.
- Keep agents on separate files and synchronize before shared test execution to avoid conflicting edits.

## Test Plan

- Run `git diff --check`, the Web asset contract, and the complete host CTest suite.
- Run the ESP-IDF MCP build and `tools/verify-production.sh`; Unity is unnecessary for an embedded-asset-only change.
- Use a deterministic Playwright MCP mock without mutation requests at 280, 390, and 560 px. Require the rule button to fill its action row, text and button centers to differ by at most 0.5 px, and horizontal overflow to remain zero.
- Verify at 561 and 1280 px that the button remains intrinsic-width, right-aligned, and centered internally; confirm learned-signal actions retain their three-column mobile layout.
- Capture mobile before/after and desktop-after screenshots. Check browser console errors, page errors, polling, overlaps, and overflow.
- Deploy the verified image through OTA to the authorized device at `192.0.2.17`, wait for reconnect, then repeat the mobile and desktop screenshots against the live Web UI without clicking Remove or invoking RF/destructive mutations.

## Interfaces And Assumptions

- No public interface or persistent-data change.
- Mobile Remove becomes full-row at widths up to 560 px; desktop behavior remains unchanged.
- Existing hardware and OTA authorization, device address, and saved RF/Wi-Fi state remain valid; no user interaction or RF signal is required.

## Implementation Results

- Added the mobile-only `#rule-list .item-actions` single-column override and a Web asset contract scoped to the exact 560 px media block.
- The Web asset test, complete host CTest suite, ESP-IDF MCP build, `git diff --check`, and clean production verifier passed. The production image is 1,033,024 bytes with 47% of each OTA slot free.
- Deterministic Playwright validation at 280, 390, 560, 561, and 1280 px confirmed exact label centering, zero horizontal overflow, unchanged learned-signal controls, and unchanged desktop alignment. No console errors or mutation requests occurred.
- Deployed the verified image through OTA to `192.0.2.17`; the device rebooted into `ota_0` running `6f5b578-dirty` with healthy OTA status.
- Live Playwright screenshots confirmed the full-width centered mobile buttons and compact right-aligned desktop buttons across all three persisted rules. Screenshots are preserved under `/tmp/esp32-cc1101-playwright/`.
