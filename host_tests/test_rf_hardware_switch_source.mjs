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
assert(switching.includes("const RfFrameCallback callback = s_frame_callback != nullptr ? s_frame_callback : restart_callback") &&
       switching.includes("const bool should_start = callback != nullptr") &&
       switching.includes("if (was_running || should_start)"),
       "a stopped service must restart through the caller-provided frame callback");
assert((switching.match(/restart_with\(previous\)/g) ?? []).length === 2,
       "activation and persistence failures must both roll back the running backend");
assert(switching.includes("else { s_hardware.store(previous, std::memory_order_release);") &&
       switching.includes("prepare_backend_gpio_ownership(previous)"),
       "a stopped service must restore its previous backend and GPIO ownership on persistence failure");

const activeSwitch = switching.slice(switching.indexOf("const BoardInfo &board"));
assertOrder(activeSwitch, [
  "stop_rf_ook_owned();",
  "const esp_err_t start_error = restart_with(hardware);",
  "rollback_error = restart_with(previous);",
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
  "set_rf_hardware(hardware, rf_signals_on_frame, nullptr)",
  "RfAutomationPauseReason::kHardwareSwitch, false",
  "bridge_events_publish(event)",
], "automation isolation and switch event");
assert(coordination.includes("event.type = BridgeEventType::kHardwareSwitch") &&
       coordination.includes("event.result = switch_error != ESP_OK ? switch_error : resume_error"),
       "the shared switch event must report success, activation failure, or resume failure");
assert(coordination.includes("set_rf_hardware(hardware, rf_signals_on_frame, nullptr)"),
       "bridge hardware switches must provide the normal RF frame callback for stopped recovery");

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
const ownership = sourceSection(
  radio, "esp_err_t prepare_backend_gpio_ownership(", "esp_err_t arm_receiver_owned(",
  "RF backend GPIO ownership");
assert(ownership.includes("gpio_is_cc1101_pin(gpios, gpios.generic_tx)") &&
       ownership.includes("gpio_is_cc1101_pin(gpios, gpios.generic_rx)") &&
       (ownership.match(/gpio_reset_pin/g) ?? []).length === 2 &&
       ownership.includes("hardware == RfHardware::kGeneric || !generic_tx_shared"),
       "shared GPIOs must be released before backend-specific ownership");
const startup = sourceSection(
  radio, "esp_err_t start_rf_ook_owned(", "esp_err_t start_rf_ook(", "owned RF startup");
assertOrder(startup, [
  "prepare_backend_gpio_ownership(selected_hardware)",
  "s_radio.initialize(radio_config)",
  "initialize_rmt()",
], "shared GPIO release before CC1101 and RMT initialization");
const cleanup = sourceSection(
  radio, "esp_err_t cleanup_resources()", "bool capture_input_is_valid(", "RF cleanup");
assert(cleanup.includes("const bool generic_tx_shared = gpio_is_cc1101_pin") &&
       cleanup.includes("if (using_generic_hardware() || !generic_tx_shared) {") &&
       radio.includes("gpio_set_pull_mode") && radio.includes("GPIO_PULLDOWN_ONLY"),
       "shared GPIO-aware TX idle cleanup and RX pull-down contracts missing");
assertOrder(cleanup, [
  "rmt_del_channel(s_rx_channel)",
  "rmt_del_channel(s_tx_channel)",
  "rmt_del_encoder(s_copy_encoder)",
  "s_radio.deinitialize()",
  "const bool generic_tx_shared",
  "gpio_reset_pin",
], "CC1101 teardown before shared GPIO release");
assert(switching.includes("prepare_backend_gpio_ownership(hardware)") &&
       switching.includes("prepare_backend_gpio_ownership(previous)"),
       "stopped backend switching must apply and roll back GPIO ownership");

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
const gpioLoad = sourceSection(
  radio, "esp_err_t load_generic_gpio_config()", "const BoardGpioMap &active_gpio_map(",
  "generic GPIO load");
assert(gpioLoad.includes("load_error = profile_matches ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_INVALID_STATE") &&
       gpioLoad.includes("config.configuration_error = load_error"),
       "profile-mismatch init error must match the cached configuration error");
assert(consoleSource.includes("generic_config_error") &&
       consoleSource.includes("BOARD_GENERIC tx_data=%d rx_data=%d config_error=%s"),
       "console board status must surface the generic GPIO configuration error");

console.log("RF hardware switch source contracts passed");
