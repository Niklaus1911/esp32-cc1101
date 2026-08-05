# Selectable Web/MQTT Home Assistant Profiles

## Summary

- Ship one firmware image with mutually exclusive, reboot-selected profiles:
  - **Web:** existing HTTP UI/API, mDNS, and LAN OTA; ESP-MQTT is never initialized.
  - **MQTT:** native Home Assistant MQTT Discovery buttons; HTTP, Web UI, mDNS, and HTTP OTA are never initialized.
- Keep Wi-Fi, RF receive/transmit, learned storage, automation, bridge events, and UART available in both profiles.
- Default to Web for missing, invalid, or corrupt configuration. If MQTT runtime allocation fails, recover to Web for that boot and report the fallback without changing the persisted request.
- Preserve Web-mode memory within 1 KiB of the measured 24,560-byte free-heap and 22,528-byte largest-block baseline by dynamically allocating MQTT-only state.
- Pin `espressif/mqtt ==1.1.0`; update the dependency lock through the component manager without manually editing `managed_components/`.

## Interfaces And Persistence

- Add UART commands:
  ```text
  service status
  service mode web
  service mode mqtt
  mqtt status
  mqtt configure <broker-ipv4> <username> [port]
  mqtt forget
  ```
- Configuration and mode changes persist immediately but never reboot automatically. `service mode mqtt` requires valid broker configuration.
- `mqtt configure` prompts for the password through the existing masked, history-free reader. Accept a unicast numeric IPv4, port 1-65535 (default 1883), printable username 1-63 bytes, and printable password 1-127 bytes. Never print the password and zero temporary buffers.
- Use CRC-protected, explicitly encoded records in `mqtt_cfg`:
  - `service`: version/type, requested/internal state, broker IPv4/port, username/password, generation, CRC.
  - `advertised`: version/type, bound broker IPv4/port, sorted unique signal names, CRC.
- Treat the advertised record as a ledger of entities that may exist on the broker. Refuse broker IP/port changes while it is nonempty; the old broker must be retired first. Same-endpoint credential changes are allowed but require reboot.
- Missing service configuration means Web/unconfigured. Retain corrupt records for diagnosis, boot Web, and repair only the owned key. Never erase the namespace or unrelated NVS.
- `service status` reports requested/effective profile, persisted generation, reboot requirement, fallback reason, and retirement state. `mqtt status` redacts the password and reports connection/subscription/reconciliation state, broker endpoint, counters, outbox bytes, heap, and both MQTT task margins.
- `mqtt forget` immediately clears an empty ledger. With advertised entities, require active MQTT on the persisted generation, a valid endpoint-bound ledger, and a broker connection; otherwise explain the required switch/reboot or configuration repair.

## MQTT And Home Assistant Behavior

- Derive identity from the full lowercase STA MAC:
  ```text
  client:       rfbridge-<12hex>
  device:       rfbridge_<12hex>
  discovery:    homeassistant/button/rfbridge_<12hex>/<signal>/config
  command:      rfbridge/<12hex>/signal/<signal>/press
  availability: rfbridge/<12hex>/availability
  HA birth:     homeassistant/status
  ```
- Publish compact, bounds-checked discovery JSON containing `name`, `unique_id`, command topic, `payload_press:"PRESS"`, command `qos:0`, `retain:false`, availability fields, and shared device metadata including firmware version. Escape JSON strings and keep every packet within 1,024 bytes.
- Use QoS 0 for commands to avoid legal QoS 1 redelivery causing duplicate RF transmissions. Reject retained, duplicate, fragmented, oversized, malformed, wrong-topic, and non-`PRESS` messages.
- Queue only a validated signal name in a fixed depth-8 command queue. The worker calls `bridge_control_replay_named(name, CONFIG_RF_DEFAULT_TX_REPEATS, kMqtt)`; the current default remains eight repeats.
- Subscribe in one request to `rfbridge/<12hex>/signal/+/press` and `homeassistant/status`, both QoS 0. After SUBACK, publish retained QoS 1 `online`, wait for PUBACK, then reconcile discovery.
- Configure retained QoS 1 LWT `offline`, clean sessions, automatic reconnect, 10-second reconnect delay, 5-second network timeout, 1,024-byte input/output buffers, 2,048-byte outbox, a 6,144-byte MQTT task, and a 5,632-byte worker sized to retain at least 1 KiB through the synchronous named-replay path.
- Keep callbacks short: copy validated commands, update atomics/counters, sample stack margin, and notify the worker. Only the worker may publish, access NVS, enumerate signals, or replay RF.
- Permit one retained QoS 1 publish in flight. Match PUBACK and `MQTT_EVENT_DELETED` message IDs; pause on disconnect and retry safely without growing the outbox.
- Reconcile crash-safely:
  1. Read current signals and the endpoint-bound ledger.
  2. Persist `ledger union current` before publishing any new entity.
  3. Tombstone `ledger - current`, waiting for each PUBACK.
  4. Publish every current configuration, waiting for each PUBACK.
  5. Commit the ledger back to exactly `current`.
- Reconcile at connection, after coalesced Home Assistant births with 0-2 second jitter, after catalog-change events, and through a 60-second audit so dropped bridge events cannot strand entities.
- Implement `mqtt forget` as a persisted retirement state machine: record retirement intent, tombstone the entire ledger and retained availability with PUBACKs, request a clean MQTT disconnect and wait for its disconnect event, mark retirement complete, erase the ledger, clear credentials/select Web, then stop MQTT. Reboots and reconnects resume an incomplete retirement; a completed retirement boots Web and finishes local cleanup without re-advertising.
- Add `BridgeEventSource::kMqtt` and a catalog-changed event after committed learn/delete operations. Update all source/type formatting and switches while retaining the existing two-sink bound.

## Startup, Memory, And Documentation

- Initialize NVS, RF storage, Wi-Fi ownership without starting Wi-Fi, automation, and profile configuration first. When MQTT is selected, initialize the lwIP core before starting its socket task; keep Web mode's TCP/IP initialization lazy.
- Preallocate/start the MQTT client and worker before creating the Wi-Fi interface and driver when MQTT is requested, proving their stacks and queues are available. Fully roll back partial MQTT ownership before selecting current-boot Web recovery.
- Initialize bridge events and learned signals before activating MQTT command/catalog delivery. In Web mode initialize mDNS, OTA, Web UI, and the Wi-Fi online sink exactly as today; provide an idempotent bridge-source rebinding path for late MQTT-to-Web fallback.
- Start UART and RF, confirm a pending OTA image unconditionally, then start saved Wi-Fi. Broker unreachability or bad credentials remains MQTT mode with automatic reconnect; it does not consume Web memory.
- Update README documentation for profiles, commands, native HA discovery, plaintext/NVS credential exposure, DHCP reservation, default `homeassistant` prefix, Web-only OTA workflow, reboot behavior, and a dedicated Mosquitto ACL:
  ```text
  read  homeassistant/status
  read  rfbridge/<mac>/signal/+/press
  write rfbridge/<mac>/availability
  write homeassistant/button/rfbridge_<mac>/#
  ```

## Verification And Acceptance

- Add host tests for both record codecs, CRC/version/type/length/name/duplicate failures, profile fallback, validation, exact topics/JSON, command rejection, ledger precommit, PUBACK/deletion handling, disconnect/restart recovery, birth/catalog coalescing, broker-change protection, and every retirement crash point.
- Add Unity coverage for missing/corrupt/wrong-type NVS, owned-key repair, full-NVS failures, endpoint binding, generation checks, and retirement ordering; compile but do not execute the Unity image without separate authorization.
- Run host CTest, `tools/verify-production.sh`, Unity compilation, dependency/Kconfig assertions, image/partition inspection, and `git diff --check`.
- Hardware acceptance:
  - Web mode remains within 1 KiB of its measured settled heap baseline.
  - MQTT mode after 32-button discovery retains at least 20 KiB free internal heap, a 16 KiB largest block, and 1 KiB stack margin on both new tasks.
  - Ten Wi-Fi/broker reconnect cycles degrade settled heap by no more than 1 KiB, with no allocation failures or monotonic outbox growth.
  - Validate 1 and 32 buttons, HA restart/birth replay, offline deletion, broker restart, bad credentials, Wi-Fi loss, command bursts, retained-command rejection, LWT, retirement, crash recovery, and both profile switches.
- RF replay acceptance requires explicit transmit authorization. Use the established no-erase flash procedure, rediscover the CP2102 port, flash the verified image, and monitor boot/profile/heap diagnostics. Commit the reviewed implementation and exclude the planning-only `.playwright-mcp/` artifact.
