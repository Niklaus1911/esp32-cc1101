# MQTT Console Parity and Persistent RF Activity

## Summary

- Preserve one shared RF, learned-signal, automation, storage, and physical UART implementation across Web and MQTT profiles.
- Delay the MQTT connection until Wi-Fi has an IP, replacing the misleading pre-DHCP `Host is unreachable` errors with a normal waiting state.
- Keep learned signals, rules, enabled state, repeats, cooldowns, and logging mode in the existing NVS storage.
- Store activity history in Home Assistant Recorder and retained MQTT snapshots, not ESP flash, avoiding RAM pressure and NVS wear.

## Implementation Changes

- Refactor MQTT startup so its client, worker, queues, and internal-heap reservation are prepared before Wi-Fi, but the MQTT task starts only after the shared Wi-Fi event reports a valid IP.
- Preserve early allocation failure fallback to Web mode. Report post-DHCP connection failures as real MQTT faults without affecting RF, automation, or UART.
- Add a profile-independent UART startup summary showing learned-signal count, persisted-rule count, automation enabled state, log mode, and storage errors.
- Verify `rule list/add/remove/enable/disable/log`, RX lines, and rule-action lines use exactly the same console path and formatting in both profiles. Only Web/mDNS versus MQTT network-service lines may differ.
- Extend MQTT's existing bridge-event sink to consume compact RX and automation events without blocking or interfering with the UART sink.
- Use a small fixed-depth telemetry queue. Publish decoded-frame metadata or bounded raw summaries, never complete raw pulse arrays. Coalesce retained updates and report queue/publish drops.
- Add an automation configuration revision so UART rule changes trigger MQTT state/discovery reconciliation promptly.

## MQTT and Persistence Interfaces

Use the existing `rfbridge/<12hex>` identity and add:

```text
rfbridge/<12hex>/event/rx
rfbridge/<12hex>/event/automation
rfbridge/<12hex>/state/automation
rfbridge/<12hex>/state/last_rx
rfbridge/<12hex>/state/last_automation
rfbridge/<12hex>/automation/enabled/set
rfbridge/<12hex>/automation/log_mode/set
```

- Publish non-retained QoS 0 Home Assistant MQTT Event messages for RF reception and automation events. Home Assistant Recorder owns the durable timeline.
- Publish rate-limited retained QoS 1 snapshots for automation configuration, rule counters, last RX, and last rule result. Retained last-event topics survive ESP and Home Assistant restarts when broker persistence is enabled.
- Add MQTT Discovery entities for RF activity, automation activity, automation enabled switch, automation log-mode select, rule count, event-drop diagnostics, and one diagnostic entity per persisted rule.
- Apply automation commands only in the MQTT worker. Validate exact non-retained QoS 0 payloads and publish state only after the NVS commit succeeds.
- Extend the retained discovery ledger to track fixed entities and rule entities. Migrate the current ledger format and tombstone all related discovery/state topics during rule removal or `mqtt forget`.
- Keep rule creation/removal available through the identical UART commands in MQTT mode. Home Assistant automations may also trigger learned-signal buttons from RF event entities; no stateful MQTT rule-builder UI will be introduced.

## Verification

- Add host tests for event serialization, maximum payload bounds, command validation, topic formatting, discovery entities, ledger migration, tombstones, coalescing, and start-after-IP behavior.
- Add Unity coverage proving MQTT configuration/profile writes cannot overwrite learned signals or rules, persisted automation settings reload after reboot, and corrupt NVS records fail closed without namespace erasure.
- Run host tests, Unity image compilation, `tools/verify-production.sh`, size inspection, and `git diff --check`.
- Hardware-test Web and MQTT boots with the same stored signals and rules. Compare `rule list`, `status`, RX output, and rule-event output before and after one profile-switch reboot.
- Reboot the ESP, broker, and Home Assistant independently and verify discovery, controls, retained snapshots, and Recorder history recover correctly.
- Require at least 20 KiB free internal heap, a 16 KiB largest block, and 1 KiB margin on MQTT and worker stacks under event bursts and maximum discovery load.
- Commit the reviewed changes, perform a no-erase wired flash on the confirmed classic ESP32 port, and monitor boot, MQTT connection, persistence, rule execution, and memory telemetry. RF-triggered transmission testing remains subject to explicit RF transmit authorization.

## Assumptions

- “No persistence” includes activity/history visibility; existing signal and rule configuration must remain device-owned and NVS-persistent.
- Home Assistant Recorder and broker persistence are the durable event owners. The ESP will not maintain a flash event journal.
- MQTT and Web network-service logs will naturally differ, but physical UART commands, RF output, storage behavior, and automation behavior must be identical.
