# Persistent RF automation rules

## Goal

Add terminal-configured rules that match an incoming learned signal and replay one other learned signal:

```text
rule add B A 8
```

When learned signal `B` is received, transmit learned signal `A` eight times.

Confirmed behavior:

- Rules persist in NVS across reboot.
- Each trigger name has at most one action; adding the same trigger again fails until removed.
- Any number of rules may target the same learned code.
- Rules support decoded and raw learned signals.
- Each rule stores a 1000 ms cooldown; runtime timestamps reset at boot.
- Learned codes referenced as either trigger or target cannot be forgotten until all referencing rules are removed.
- NVS is never erased automatically.

## Serial interface

Add one command family:

```text
rule add <received_name> <transmit_name> [repeats]
rule list
rule remove <received_name>
rule enable
rule disable
```

- `repeats` defaults to `CONFIG_RF_DEFAULT_TX_REPEATS` and remains bounded to 1–20.
- `rule list` prints the persisted enabled state and each `trigger -> target`, repeats, and cooldown.
- A missing/corrupt learned code, duplicate trigger, self-rule, equivalent trigger signal, full rule table, or invalid repeat count fails without changing NVS or runtime state.
- Global enable/disable state persists; first-time default is enabled.
- Automation tasks do not print unsolicited UART messages. Existing RX output remains unchanged; counters and last action/error are exposed through `status`/`rule list` to avoid adding more asynchronous REPL output.

## Architecture

### 1. Extend versioned NVS storage

Update `components/rf_storage/` with a second namespace for automation rules and a small metadata namespace/key for global enabled state.

- Use the trigger name as the rule key, naturally enforcing one rule per trigger.
- Store an explicit versioned, checksummed rule payload containing target name, repeats, and 1000 ms cooldown; never persist C++ struct layout.
- Add create-only rule creation, load/list/remove, enabled-state get/set, and reference-query APIs under the existing storage mutex.
- During rule creation, atomically verify both learned records exist and decode correctly before committing.
- Allow unlimited rules to share a target name.
- Make `rf_storage_forget()` scan rule references under the same mutex and return `ESP_ERR_INVALID_STATE` when the learned name is used by any trigger or target.
- Reject malformed versions, lengths, names, CRCs, repeats, cooldowns, and dangling references without transmitting or erasing anything.

Files:

- `components/rf_storage/include/rf_storage.hpp`
- `components/rf_storage/include/rf_storage_rule_format.hpp`
- `components/rf_storage/rf_storage.cpp`
- `components/rf_storage/rf_storage_rule_format.cpp`
- `components/rf_storage/CMakeLists.txt`
- `components/rf_storage/test/test_rf_storage.cpp`

### 2. Add an automation owner component

Create `components/rf_automation/` with one task owning the runtime rule table and all rule state transitions.

- Load and validate persisted rules at startup into a bounded table (`CONFIG_RF_MAX_AUTOMATION_RULES`, default 32).
- Queue accepted RF frames from `rf_console_on_frame()` with zero wait; the radio callback performs no NVS, matching, logging, or transmission waits.
- Match incoming frames against stored trigger signals with the existing canonical decoded/raw equivalence rules, including decoded/raw cross-identification.
- Serialize frame handling, add/remove, enable/disable, and cache updates through the automation task command queue with bounded replies.
- On a match, check global enable state and the rule’s 1000 ms monotonic cooldown, record the attempt before TX, then call the existing bounded decoded/raw transmit API.
- Preserve the desired RX state through the existing radio-owner TX path. Local TX cannot trigger itself because RX is disabled during transmission.
- Reject self-rules, equivalent duplicate trigger signals, and directed cycles when adding rules. Acyclic chains are allowed, but one received frame fires at most one rule.
- Preflight table capacity and runtime validation before committing NVS so persistent and RAM state cannot diverge.
- Track matches, successful actions, cooldown suppressions, queue drops, TX failures, and the last trigger/target/error for diagnostics.
- If startup encounters corrupt/dangling/over-capacity rules, fail automation closed and report it unavailable while ordinary receive, manual replay, console, and learned-code storage continue.

Files:

- `components/rf_automation/CMakeLists.txt`
- `components/rf_automation/Kconfig`
- `components/rf_automation/include/rf_automation.hpp`
- `components/rf_automation/rf_automation.cpp`
- portable engine/helpers and focused tests as needed

### 3. Integrate console and startup

Update `components/rf_console/` and `main/`:

- Register the `rule` command family and parse all forms strictly.
- Route every accepted frame to both the existing console event queue and the automation queue without blocking.
- Keep `radio start` using the same fan-out callback so automation remains active after radio recovery.
- Extend `status` with automation enabled/available state and counters; do not add asynchronous automation prints.
- Initialize storage, then automation, then console/RF. Automation initialization failure must not prevent normal RF startup.
- Make `forget <name>` report that the name is referenced when storage refuses deletion.

Files:

- `components/rf_console/CMakeLists.txt`
- `components/rf_console/rf_console.cpp`
- `components/rf_console/include/rf_console_parse.hpp`
- `components/rf_console/rf_console_parse.cpp`
- `components/rf_console/test/test_rf_console_parse.cpp`
- `main/CMakeLists.txt`
- `main/main.cpp`
- `README.md`

## Subagent orchestration

Use non-overlapping parallel tracks:

1. **Storage agent:** review rule format, NVS atomicity, reference integrity, and corruption/error paths.
2. **Automation agent:** review owner-task serialization, cooldown boundaries, matching, cycle rejection, and TX/queue failure behavior.
3. **Console/integration agent:** review parser compatibility, callback nonblocking behavior, diagnostics, startup degradation, and documentation.
4. **Final independent agent:** after fixes and tests, return `PASS` only if no critical/high production blocker remains.

The primary agent owns edits and reconciles all findings. Read-only reviews and independent test/build commands run in parallel where safe.

## Tests

Add host and Unity coverage for:

- golden rule record bytes, round trips, CRC/version/length/name/repeat/cooldown corruption;
- one rule per trigger and unlimited shared targets;
- equivalent trigger rejection, self-rule rejection, directed-cycle rejection, and valid acyclic chains;
- decoded, raw, and decoded/raw-equivalent matching;
- exact 1000 ms cooldown boundary, disabled state, one action per frame, and timestamp reset behavior;
- create/list/remove and persisted enable/disable;
- referenced trigger/target `forget` rejection and successful deletion after rule removal;
- duplicate create preserving the original rule;
- queue-full fail-closed counters, TX errors, malformed/dangling rules, and over-capacity startup;
- unchanged RAM replay, named replay, learning, radio recovery, and parser behavior.

## Verification

- Run normal and ASan/UBSan host tests.
- Build clean production and Unity images under ESP-IDF 6.0.2; run `idf.py size` and inspect task/queue memory.
- Confirm no compiler warnings, destructive NVS calls, merge markers, local paths, or staged generated files.
- Do not flash automatically.
- With explicit hardware approval:
  1. Learn `A` and `B`, then run `rule add B A 8`.
  2. Reboot and confirm the rule and enabled state persist.
  3. Press `B` and verify `A` is transmitted once per cooldown window.
  4. Confirm `A` alone does not trigger the rule.
  5. Confirm `rule disable` survives reboot and prevents actions.
  6. Confirm `forget A` and `forget B` fail while referenced, then succeed after `rule remove B`.
  7. Exercise raw learned signals and multiple different triggers targeting `A`.

## Commit

After all gates pass, create a focused commit:

```text
feat: add persistent RF automation rules
```

Audit the explicit staged-file inventory and report the commit hash, author, verification results, hardware limitations, and final working-tree status.
