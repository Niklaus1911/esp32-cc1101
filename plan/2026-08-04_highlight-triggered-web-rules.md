# Highlight Triggered Rules in the Web UI

## Summary

When the Rules view is active and an automation rule executes, softly pulse the corresponding rule row in yellow. Reuse the existing one-second `/api/live` polling data; no firmware event stream, heap allocation, or API extension is needed.

Live Playwright testing confirmed that sending `moto_off_2` changed `actions` from `0` to `1` and set `last_trigger` to `moto_off_2`, matching the existing row's `data-trigger`.

## Implementation Changes

- Track the previous completed execution count as `automation.actions + automation.tx_errors`. Initialize it without highlighting on the first poll and rebaseline without highlighting after counter rollback or device reset.
- On a strict increase, highlight `automation.last_trigger` only when the Rules panel is currently visible. Match the row by exact `data-trigger`; missing or removed rows are a no-op.
- Count successful and failed transmissions as triggered rules. Do not highlight cooldown-suppressed matches because they do not increment either execution counter.
- Restart the pulse when the same rule triggers again, cancel any previous row's pulse when another rule triggers, and remove the class after one second. Events detected while another view is active are not queued for later.
- Add a 900 ms single yellow pulse using the existing `#efc06f` warning color, a low-opacity background, and an inset accent that does not change layout. Under `prefers-reduced-motion`, show a steady low-contrast yellow highlight for the same brief duration.
- Keep `app.js` below its existing 24 KiB limit and add focused asset contracts for the counter source, active-view guard, trigger-row lookup, animation, and reduced-motion rule.

## Interfaces

- No changes to REST endpoints, firmware types, NVS, automation behavior, or polling frequency.
- The pulse is browser-local and non-persistent. If several rules execute between polls, only the latest `last_trigger` row pulses because `/api/live` intentionally exposes a bounded snapshot.
- The pulse appears on the first poll after the action finishes, normally within one polling interval plus RF transmission time.

## Test Plan

- Run JavaScript syntax checks, Web asset contracts, clean host CTest, the Unity image build, `git diff --check`, and `tools/verify-production.sh`.
- Use a deterministic local Playwright mock to verify: first-poll baseline, successful action, failed action, cooldown suppression, counter rollback, same-row retrigger, different-row replacement, missing rows, hidden Rules view, and no delayed pulse after returning to Rules.
- Validate desktop and 390 px mobile layouts, no horizontal overflow or layout shift, animation cleanup, reduced-motion behavior, and no console/page errors or mutation requests.
- Build and flash the approved classic ESP32 through the confirmed CP2102 port without erasing NVS. Keep Playwright on the live Rules page, ask the user to send `moto_off_2` or another configured trigger, and verify the exact row pulses once while the action counters and `last_trigger` advance.

## Assumptions

- "Rule trigger" means a non-cooldown execution attempt, including a failed RF transmission.
- The existing one-second snapshot polling latency is acceptable; an immediate event-stream implementation is outside scope.
- Existing serial-console worktree changes remain untouched.
