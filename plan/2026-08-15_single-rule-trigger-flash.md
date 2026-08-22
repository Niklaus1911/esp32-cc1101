# Make Rule Highlight Flash Once

## Summary

Change the existing yellow rule-row animation to run once when an automation event has `kind: "triggered"`. The subsequent `completed` event will still refresh live counters but will not restart the animation.

## Implementation

- In the browser SSE handler, call `pulseRule()` only when the event is `automation`, its payload kind is `triggered`, and it has a trigger name.
- Expose the latest automation action ID in `/api/live` and use it to deduplicate the SSE trigger against a polling snapshot observed while the stream is connecting or disconnecting.
- Continue processing every automation event through the coalesced `/api/live` refresh so action counts, errors, and status remain current.
- Leave the SSE event schema unchanged; `triggered` and `completed` remain useful to MQTT and other consumers.
- Do not alter Signals-page rows, animation duration, color, polling fallback, or reduced-motion behavior.

## Validation

- Extend the Web asset contract to require the `kind === "triggered"` guard.
- With Playwright, send `triggered` followed by `completed` for the same rule and assert the matching rule starts exactly one animation, other rules do not flash, completion does not restart the highlight, counters still reconcile, and no browser errors or layout overflow occur.
- Run JavaScript syntax checks, the clean host suite, and an affected N16R8 build.
- Verify `app.js` remains below 48 KiB and run `tools/verify-production.sh` once on the unchanged final candidate.
- Flash the approved ESP32-S3 N16R8 without erasing NVS, then perform read-only Web UI checks without deliberately triggering an RF rule.

## Assumptions

- The reported yellow effect is the existing `.rule-triggered` rule-row animation.
- A rule flashes immediately on its `triggered` event, regardless of the later replay result.
- If Playwright MCP remains transport-closed, leave its installation untouched, use the installed Playwright runner for equivalent validation, and report the fallback.
