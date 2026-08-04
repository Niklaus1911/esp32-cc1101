# More RF automation logging

## Goal

Add useful serial diagnostics for automation activity, especially a clear message when a rule matches and when its transmission completes, without blocking the radio callback, automation task, or learning path.

Default behavior after upgrade is action logging enabled. Operators can persistently choose the amount of output:

```text
rule log
rule log off
rule log actions
rule log verbose
```

Representative output:

```text
RULE TRIGGER trigger=B target=A repeats=8 encoding=decoded
RULE ACTION trigger=B target=A result=OK elapsed_ms=742
RULE SUPPRESS trigger=B reason=cooldown remaining_ms=418
RULE SKIP reason=ambiguous matches=2
```

`actions` prints trigger and completion/error lines. `verbose` additionally prints cooldown suppressions, ambiguous matches, stale-generation skips, and deferred queue-drop summaries. Unmatched frames are never logged.

## Approach

### 1. Add a bounded automation event interface

Update `components/rf_automation/` with a typed event structure and a registered event sink.

- Emit `triggered` immediately before bounded TX and `action_completed` immediately after it, including trigger, target, repeats, decoded/raw encoding, result, and elapsed time.
- In verbose mode, emit cooldown, ambiguity, stale-frame, and queue-drop events with the relevant counters/reason fields.
- Keep `rf_automation_on_frame()` zero-wait and free of logging, formatting, NVS, or mutex waits.
- The sink callback performs only a zero-wait queue send. Failed sends increment a separate `log_drops` status counter and never affect RF execution.
- Register/unregister the sink through a mutex-protected API. Do not invoke a sink that can call back into automation APIs while the automation mutex is held.
- Add `log_mode`, emitted-event count, and dropped-log count to `RfAutomationStatus`.

Files:

- `components/rf_automation/include/rf_automation.hpp`
- `components/rf_automation/rf_automation.cpp`
- `components/rf_automation/test/test_rf_automation.cpp`

### 2. Persist the logging mode safely

Extend the existing `rf_rule_meta` namespace with a validated `log_mode` `u8` value:

- `0=off`, `1=actions`, `2=verbose`.
- A missing key defaults to `actions`, so rule-trigger logging is enabled after firmware upgrade.
- `rule log <mode>` commits NVS before changing runtime state; changing logging does not alter rule generations or queued-frame validity.
- A malformed value fails clearly and can be repaired only by an explicit `rule log <mode>` command, using the same single-key recovery pattern as the enabled flag.
- Preserve the existing no-automatic-erase policy and independent learned-code/rule namespace degradation.

Files:

- `components/rf_storage/private_include/rf_storage_rule_backend.hpp`
- `components/rf_storage/rf_storage.cpp`
- `components/rf_storage/test/test_rf_storage.cpp`

### 3. Route logs through the console owner

Update `components/rf_console/` so automation never writes directly to UART:

- Add a dedicated bounded automation-log queue, separate from the critical frame/learn queue.
- Have the existing console event worker service frame/learn events first and log events second, using task notifications for bounded wakeups and learn deadlines while directly prioritizing the frame/learn queue.
- Register the automation sink only after both queues and the worker are ready; unregister it during failed-start cleanup before deleting queues.
- A full log queue drops only that log event. It must not increment frame-drop counters, cancel learning, delay TX, or suppress later rule actions.
- Format stable one-line `RULE ...` records and flush after each line.
- Add strict parsing for `rule log [off|actions|verbose]`, show the current persisted/runtime mode, and extend `status` with log mode/event/drop fields.
- Preserve normal linenoise history, arrows, editing, and tab completion. As with existing asynchronous `RX` output, a log can visually interrupt an active prompt; do not reintroduce dumb mode or escape-sequence workarounds.

Files:

- `components/rf_console/rf_console.cpp`
- `components/rf_console/include/rf_console_parse.hpp`
- `components/rf_console/rf_console_parse.cpp`
- `components/rf_console/test/test_rf_console_parse.cpp`
- `README.md`

## Tests

Add host and Unity coverage for:

- strict `rule log` parsing and all invalid arities/modes;
- metadata default `actions`, round-trip persistence, invalid values/types, and explicit repair without namespace erase;
- event formatting for decoded/raw triggers, TX success, and named ESP errors;
- action mode versus verbose filtering;
- trigger-before-completion ordering and elapsed-time bounds through testable event helpers;
- log-queue overflow incrementing only `log_drops` while frame/learn delivery remains unaffected;
- sink registration/cleanup and unavailable-automation command behavior;
- unchanged cooldown, generation, ambiguity, learning, replay, and callback tests.

## Verification

- Run normal and ASan/UBSan host tests.
- Run `git diff --check` and audit for destructive NVS calls, direct automation-task `printf`/`ESP_LOG`, merge markers, and generated files.
- Build clean production and Unity images under ESP-IDF 6.0.2; inspect warnings, image size, 4 MB header, static DRAM, queue memory, and task stack usage.
- Use a final independent critical/high blocker review.
- Do not flash automatically.
- With explicit hardware approval, verify `actions`, `verbose`, `off`, reboot persistence, successful and failed TX lines, cooldown suppression, active linenoise editing, and learning while automation logs are being produced.

## Commit

After all gates pass, create a focused commit:

```text
feat: add RF automation activity logging
```
