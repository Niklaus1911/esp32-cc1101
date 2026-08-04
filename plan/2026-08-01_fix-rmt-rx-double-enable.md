# Fix RMT RX double-enable error

## Context

Boot succeeds, but startup logs:

```text
E (...) rmt: rmt_rx_enable(...): channel not in init state
```

`initialize_rmt()` enables the RX channel, then the radio-owner task calls `restore_receive_owned()`, which enables it again. ESP-IDF rejects that second transition, although the firmware currently masks `ESP_ERR_INVALID_STATE` and continues.

## Changes

1. **Correct RX ownership and startup ordering** — `components/rf_ook/rf_ook.cpp`
   - Leave the newly created RX channel in ESP-IDF's initial state inside `initialize_rmt()`.
   - Continue enabling the TX channel during initialization.
   - Let `radio_task()` exclusively perform the first RX enable through `restore_receive_owned()`, immediately before arming `rmt_receive()`.
   - Preserve the existing disable/re-enable behavior used by receive toggling, transmission, queue-drop recovery, shutdown, and failed-start cleanup.

2. **Review lifecycle edge cases**
   - Confirm cleanup can delete an RX channel that was never enabled if task creation fails.
   - Confirm normal startup performs exactly one `INIT → ENABLE` RX transition.
   - Confirm later recovery paths perform `ENABLE → INIT → ENABLE` transitions and do not hide unexpected state errors.

## Verification

- Run host tests normally and with ASan/UBSan.
- Perform clean ESP-IDF 6.0.2 production and Unity-image builds.
- Run `git diff --check` and inspect the focused diff.
- Do not flash automatically. With explicit hardware approval, reboot the board and verify:
  - no `rmt_rx_enable: channel not in init state` log;
  - `status` reports `running=1 rx_enabled=1 rx_active=1`;
  - remote reception and `last` still work.

The unrelated 4 MB flash versus 2 MB image-header warning is outside this focused fix.
