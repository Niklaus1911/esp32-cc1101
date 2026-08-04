# Fix System Mobile Layout and Visual Review

## Summary

Replace the mechanically responsive but excessively tall System layout with a compact mobile dashboard. The current 390 px page is 3,639 px tall with a horizontally scrolled tab bar; the target is at most 1,800 px for the deterministic healthy fixture, with all navigation visible.

Do not use subagents.

## Implementation Changes

- At widths up to 560 px, fit all five navigation tabs within the viewport with equal-width controls and no horizontal scrolling.
- Keep the four health metrics in a compact 2x2 summary with reduced fixed spacing and readable wrapping.
- Keep primary RF, CC1101, Wi-Fi, network, runtime, and service information visible, but render label/value pairs as dense two-column rows instead of stacking every value below its label.
- Keep section badges beside headings, reduce excessive band/group spacing, and retain existing diagnostic disclosures.
- Convert Firmware update into a native disclosure:
  - Its summary always shows firmware state and version.
  - It starts closed on mobile.
  - A small startup check opens it on initial desktop loads above 560 px.
  - Polling and viewport resizing must not override the user's subsequent open/closed choice.
  - Preserve all existing OTA element IDs, upload behavior, progress, warnings, and reboot recovery.
- Keep `app.js` below the existing strict 32 KiB limit; simplify nearby code if necessary rather than raising the cap.
- Update `AGENTS.md` Browser Validation rules for all frontend work:
  - Automated overflow and error checks are insufficient by themselves.
  - Full-page mobile and desktop screenshots must be visually inspected for hierarchy, density, scanability, navigation discoverability, first-viewport usefulness, awkward whitespace, and excessive scrolling.
  - Reject technically non-overflowing layouts that are unnecessarily tall, sparse, clipped, or cumbersome.
  - Compare before/after screenshots and revise before handoff when the visual result is poor.

No REST API, firmware backend, NVS, RF, health classification, or polling interface changes are required.

## Verification

- Extend Web asset contracts for the mobile tab layout, compact System rows, firmware disclosure structure, desktop initialization behavior, preserved OTA IDs, and unchanged asset limits.
- Run JavaScript syntax checks, Web asset contracts, host CTest, `git diff --check`, ESP-IDF MCP build, out-of-tree Unity compilation, and `tools/verify-production.sh`.
- Use deterministic Playwright fixtures at 280, 390, 560, 561, 820, and 1280 px:
  - All tabs must be fully visible with no navigation scrolling.
  - Healthy 390 px content must be no taller than 1,800 px with firmware collapsed; 280 px must remain below 2,100 px.
  - The first viewport must contain the complete health summary and meaningful RF details.
  - Long SSIDs, errors, warning states, prior schemas, and disconnected retained data must wrap cleanly.
  - Firmware starts closed on mobile and open on desktop; user and diagnostic disclosure state survives polling.
  - No clipping, overlap, horizontal overflow, mutation requests, console errors, or page errors.
- Capture and visually inspect final local 390 px and 1280 px screenshots against the current mobile screenshot and compact prototype.

## Deployment

- Do not commit or push.
- Reconfirm the established CP2102 port is present and unused, then perform a normal MCP flash of the verified uncommitted build. Preserve NVS and do not clean, erase, change target, or open a serial monitor.
- Expect the deployed version to carry the current commit plus a `-dirty` suffix.
- Validate normal live polling, System/OTA disclosure behavior, mobile height and navigation, and desktop layout at `192.0.2.17`.
- Capture final live mobile and desktop screenshots without submitting OTA, transmitting RF, or requiring user action.

## Implementation Results

- Flashed the verified uncommitted build normally through the established CP2102 by-id port, preserving NVS. The device returned at `192.0.2.17` and reported `ota_0` as the running partition.
- The live OTA status endpoint reports `running_version: 5308a34`; this firmware's `esp_app_desc` does not include the expected `-dirty` suffix even though the build was uncommitted. The live markup and behavior match the current changes.
- Live Playwright width checks passed at 280, 390, 560, 561, 820, and 1280 px. Mobile body heights were 1,614 px at 280 px and 1,594 px at 390/560 px with firmware closed; desktop firmware opened on initial loads.
- Live navigation fit exactly at all tested mobile widths (`scrollWidth === clientWidth`) with no horizontal body overflow. System badges and health metrics rendered from the device APIs.
- Live polling and disclosure persistence passed: 10 `/api/live` and 4 OTA status polls completed; user-opened and user-closed firmware states survived polling and viewport changes.
- The live probe observed no mutation requests, failed requests, HTTP error responses, console errors, or page errors.
- Captured and visually inspected live screenshots:
  - `/tmp/esp32-cc1101-playwright/system-mobile-layout-after-device-mobile.png`
  - `/tmp/esp32-cc1101-playwright/system-mobile-layout-after-device-desktop.png`
- The Playwright MCP transport closed during the long persistence probe. The same Playwright engine was run locally against the device for that probe and the final screenshots; the earlier MCP width matrix completed before the transport failure.

## Post-commit Deployment

- Committed the implementation as `fd6a004` (`Improve System page mobile layout`).
- Performed an in-tree clean, rebuilt the committed tree, and flashed the clean image through the CP2102 by-id port with NVS preserved.
- Confirmed `/api/v1/ota/status` reports `running_version: fd6a004`, `state: idle`, and no rollback or maintenance error.
- Confirmed the device serves the committed firmware disclosure markup and compact mobile CSS.
- The branch has not been pushed.
