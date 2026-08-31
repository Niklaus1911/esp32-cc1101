# Recover RF Backend Switches From a Stopped Service

## Diagnosis

When the initial CC1101 startup fails (for example after clearing NVS with no CC1101 attached), the RF service is left stopped. Selecting Generic from Web, MQTT, or UART currently persists the backend and prepares its GPIOs but does not start the service, so the change only becomes operational after a reboot.

## Fix

- Let the shared hardware-switch transaction accept the application frame callback used for recovery.
- When the service is stopped, start the selected backend after GPIO ownership is prepared and before persisting the selection.
- Roll back both backend state and GPIO ownership if stopped-service activation or persistence fails; keep the existing live-service rollback behavior.
- Have the bridge control path pass `rf_signals_on_frame`, covering Web, MQTT, and UART callers consistently.
- Add a source contract that requires stopped-service activation and rollback ordering.

## Verification

- Run the focused native/source tests and firmware build checks.
- Use Playwright against a deterministic localhost mock for the unchanged Web control contract.
- If the approved N16R8 path is available, flash through `tools/build-board.sh` without erasing NVS and validate `cc1101 -> generic` recovery and Generic health.
