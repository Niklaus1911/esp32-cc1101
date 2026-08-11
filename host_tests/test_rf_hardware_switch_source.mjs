import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const root = process.argv[2];
assert(root, "project root argument is required");

const radio = readFileSync(
  join(root, "components", "rf_ook", "rf_ook.cpp"), "utf8");
const bridge = readFileSync(
  join(root, "components", "bridge_control", "bridge_control.cpp"), "utf8");
const storage = readFileSync(
  join(root, "components", "rf_storage", "rf_storage.cpp"), "utf8");
const consoleSource = readFileSync(
  join(root, "components", "rf_console", "rf_console.cpp"), "utf8");

function sourceSection(source, start, end, label) {
  const startOffset = source.indexOf(start);
  const endOffset = source.indexOf(end, startOffset + start.length);
  assert(startOffset >= 0 && endOffset > startOffset, `${label} source section missing`);
  return source.slice(startOffset, endOffset).replace(/\s+/g, " ");
}

function assertOrder(source, fragments, label) {
  let offset = -1;
  for (const fragment of fragments) {
    const next = source.indexOf(fragment, offset + 1);
    assert(next > offset, `${label} ordering missing: ${fragment}`);
    offset = next;
  }
}

const switching = sourceSection(
  radio, "esp_err_t set_rf_hardware(", "esp_err_t reset_rf_radio(",
  "RF hardware switch transaction");

assert(switching.includes("hardware != RfHardware::kCc1101 && hardware != RfHardware::kGeneric"),
       "switch transaction must reject unsupported backends");
assert(switching.includes("!switch_guard.acquired() || !lifecycle.acquired()") &&
       switching.includes("state != ServiceState::kStopped && state != ServiceState::kRunning") &&
       switching.includes("s_maintenance_requested.load"),
       "switch transaction must reject overlap, transitions, and maintenance");
assert(switching.includes("if (previous == hardware)") &&
       switching.includes("rf_storage_hardware_set(hardware)"),
       "selecting the active backend must still repair persistence");
assert(switching.includes("s_start_override_valid.store(true") &&
       switching.includes("start_rf_ook_owned(callback, context)"),
       "a target restart must not be replaced by the previous NVS selection");
assert((switching.match(/restart_with\(previous\)/g) ?? []).length === 2,
       "activation and persistence failures must both roll back the running backend");
assert(switching.includes("else { s_hardware.store(previous, std::memory_order_release); }"),
       "a stopped service must restore its previous in-memory backend on persistence failure");

assertOrder(switching, [
  "const RfHardware previous",
  "const bool was_running",
  "stop_rf_ook_owned();",
  "const esp_err_t start_error = restart_with(hardware);",
  "const esp_err_t rollback_error = restart_with(previous);",
  "const esp_err_t persist_error = rf_storage_hardware_set(hardware);",
], "activation before persistence");
assertOrder(switching, [
  "const esp_err_t persist_error = rf_storage_hardware_set(hardware);",
  "if (persist_error != ESP_OK)",
  "s_hardware_switch_error.store(ESP_OK",
  "s_hardware_switches.fetch_add(1",
], "successful switch accounting");

const coordination = sourceSection(
  bridge, "esp_err_t bridge_control_set_rf_hardware(",
  "esp_err_t bridge_control_reset_radio(", "shared switch coordination");
assertOrder(coordination, [
  "RfAutomationPauseReason::kHardwareSwitch, true",
  "set_rf_hardware(hardware)",
  "RfAutomationPauseReason::kHardwareSwitch, false",
  "bridge_events_publish(event)",
], "automation isolation and switch event");
assert(coordination.includes("event.type = BridgeEventType::kHardwareSwitch") &&
       coordination.includes("event.result = switch_error != ESP_OK ? switch_error : resume_error"),
       "the shared switch event must report success, activation failure, or resume failure");

const start = sourceSection(radio, "esp_err_t start_rf_ook(", "esp_err_t stop_rf_ook_owned(",
                            "public RF start guard");
assert(start.includes("LifecycleGuard lifecycle") &&
       start.includes("s_hardware_switch_busy.load"),
       "normal RF start must serialize with hardware switching");
const stop = sourceSection(radio, "void stop_rf_ook(", "esp_err_t transmit_rf_decoded(",
                           "public RF stop guard");
assert(stop.includes("LifecycleGuard lifecycle") &&
       stop.includes("s_hardware_switch_busy.load"),
       "normal RF stop must serialize with hardware switching");
const statusGetter = sourceSection(radio, "esp_err_t get_rf_radio_status(", "esp_err_t get_rf_hardware(",
                                  "status getter");
assert(radio.includes("status->hardware = stored_hardware") &&
       statusGetter.indexOf("fill_software_status(status)") <
           statusGetter.indexOf("status->hardware = stored_hardware") &&
       !statusGetter.includes("s_hardware.store(stored_hardware"),
       "stopped status reads must not mutate the selected backend");
assert(radio.includes("gpio_set_pull_mode") && radio.includes("GPIO_PULLDOWN_ONLY") &&
       radio.includes("force_generic_tx_idle();"),
       "generic GPIO idle and pull-down safety contracts missing");

const persistence = sourceSection(
  storage, "esp_err_t rf_storage_hardware_set(",
  "esp_err_t rf_storage_rule_create_validated(", "RF hardware persistence");
assertOrder(persistence, [
  "decode_rf_hardware_record(",
  "if (matches)",
  "nvs_set_blob(",
  "nvs_commit(",
], "hardware persistence no-op and repair");
assert(storage.includes("s_hardware_initialization_error.store(ESP_ERR_NO_MEM"),
       "hardware storage must report mutex allocation failure");

const radioCommand = sourceSection(
  consoleSource, "int radio_command(", "int last_command(", "radio command");
const hardwareRead = sourceSection(
  radioCommand, 'if (argc == 2 && std::strcmp(argv[1], "hardware") == 0)',
  'if (argc == 3 && std::strcmp(argv[1], "hardware") == 0)', "radio hardware read");
assert(hardwareRead.includes("OutputGuard guard") && hardwareRead.includes("guard.locked()"),
       "radio hardware output must hold the console output lock");
assert(consoleSource.includes('print_dashboard_row("Switch error"') &&
       consoleSource.includes('"Switches", right'),
       "pretty radio status must show switch diagnostics");

console.log("RF hardware switch source contracts passed");
