# Persist Web Observed Activity Across Refreshes

## Summary

Persist the existing 50-row Observed activity list in tab-scoped `sessionStorage`. Playwright reproduced the defect: an accepted frame appeared before refresh and the table returned to "No new frames this session" afterward.

The ESP32 API, heap usage, and NVS remain unchanged.

## Implementation Changes

- Add a versioned storage key such as `rfbridge.observed-activity.v1` in `components/web_ui/assets/app.js`.
- Store a versioned payload containing the accepted-frame watermark and entries with observation time, counter delta, and frame snapshot.
- Restore valid data before the first activity render and `/api/live` poll so rows are visible immediately after refresh.
- Persist after accepted-frame increments and retain the accepted counter as a watermark, preventing duplicate rows after reload and capturing the latest frame when the counter advanced during refresh.
- Keep the existing 50-entry limit on both restoration and writes. Reject the entire stored payload if it is malformed, oversized beyond 64 KiB, has the wrong version, or contains invalid timestamps, deltas, counters, or frame objects.
- Catch all Web Storage access and quota errors; polling and the in-memory activity list must continue normally when storage is unavailable.
- If the device counter decreases because of reset or wraparound, retain existing rows but rebaseline and persist the new counter without fabricating an event.
- Make Clear empty the in-memory list and remove its `sessionStorage` entry. Subsequent accepted frames begin a new persisted list.
- Update `README.md` to document that the last 50 browser-observed rows survive refresh within the same tab session, are removed by Clear/tab closure, and are not device-wide or guaranteed RF history.
- Extend `host_tests/test_web_assets.mjs` with the storage, version, bound, restoration, and Clear contracts. Raise only the `app.js` size ceiling from 20,000 bytes to 24 KiB; retain the existing HTML/CSS limits and production image-size gate.

## Interfaces

- No REST API, C++ type, NVS schema, partition, or firmware-side history changes.
- The `sessionStorage` payload is an internal, origin-scoped browser schema. A device IP change starts a separate history.
- Rows remain browser-observation snapshots. Frames missed while the page was not polling cannot be reconstructed from the current latest-frame API.

## Test Plan

- Run `node --check`, Web asset contract tests, clean host CMake/CTest, the Unity image build, `git diff --check`, and `tools/verify-production.sh`.
- Serve the real assets locally and use Playwright MCP with deterministic API interception. Verify baseline, decoded/raw additions, counter jumps, refresh persistence without duplication, subsequent additions, the 50-row cap, Clear plus refresh, corrupt/oversized storage, storage exceptions, and counter rollback.
- At desktop and 390 px mobile viewports, verify no horizontal page overflow, coherent table layout, polling continuity, and no application console/page errors. Intercept mutation routes and confirm restoration and Clear make no device requests.
- Confirm an independent new tab starts empty while refresh in the original tab retains its rows.
- With the approved classic ESP32 and confirmed CP2102 port, build and flash without erasing NVS. Open `http://192.0.2.17/` with Playwright, ask the user to send an accepted RF signal, confirm the row appears, refresh and confirm it remains exactly once, then verify another signal and Clear behavior.

## Assumptions

- Persistence is limited to the selected same-tab browser session.
- Existing activity remains visible across an ESP reset until Clear or tab-session termination.
- The two RF attempts during planning were not accepted by the device, so live RF persistence validation must be repeated after implementation; the defect itself was reproduced against the real page using a deterministic mocked `/api/live` sequence.
