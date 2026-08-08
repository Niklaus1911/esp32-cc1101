import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const root = process.argv[2];
assert(root, "project root argument is required");

const read = (...parts) => readFileSync(join(root, ...parts), "utf8");
const network = read("components", "network_wifi", "network_wifi.cpp");
const networkHeader = read("components", "network_wifi", "include", "network_wifi.hpp");
const networkCmake = read("components", "network_wifi", "CMakeLists.txt");
const bridge = read("components", "bridge_control", "bridge_control.cpp");
const bridgeCmake = read("components", "bridge_control", "CMakeLists.txt");
const consoleSource = read("components", "rf_console", "rf_console.cpp");
const web = read("components", "web_ui", "web_api.cpp");

function sourceSection(source, start, end, label) {
  const startOffset = source.indexOf(start);
  const endOffset = source.indexOf(end, startOffset + start.length);
  assert(startOffset >= 0 && endOffset > startOffset, `${label} source section missing`);
  return source.slice(startOffset, endOffset).replace(/\s+/g, " ");
}

for (const obsolete of ["kTrueRandom", "TrueRandomReply", "s_true_random_reply_queue",
                        "network_wifi_get_true_random_word", "esp_random.h"]) {
  assert(!network.includes(obsolete), `obsolete Wi-Fi entropy plumbing remains: ${obsolete}`);
}
assert(!networkHeader.includes("network_wifi_get_true_random_word") &&
       !networkCmake.includes("esp_hw_support"),
       "Wi-Fi public API and dependencies must not own random generation");

const generation = sourceSection(
  bridge, "esp_err_t bridge_control_generate_and_save_random_decoded(",
  "esp_err_t bridge_control_transmit_raw(", "shared random generation");
for (const requirement of [
  "attempt < kRandomSignalMaximumAttempts",
  "esp_random()",
  "make_random_signal_candidate",
  "rf_signals_match_frame",
  "LearnedMatchKind::kUnavailable",
  "rf_signals_save_decoded",
  "rf_storage_exists",
  "ESP_ERR_NOT_FINISHED",
]) assert(generation.includes(requirement), `random generation contract missing: ${requirement}`);
assert(bridgeCmake.includes("esp_hw_support"),
       "bridge control must declare the hardware RNG dependency");
assert(!generation.includes("transmit_rf") && !generation.includes("bridge_control_transmit"),
       "random generation must never transmit RF");

const uart = sourceSection(
  consoleSource, "int random_signal_command(", "int send_value_command(",
  "UART random command");
assert(uart.includes("bridge_control_generate_and_save_random_decoded") &&
       uart.includes("RANDOM name=%s code=%llu hex=0x%llX bits=%u protocol=%u pulse_us=%u"),
       "UART random command output missing");
assert(!uart.includes("Wi-Fi") && !uart.includes("true entropy unavailable"),
       "UART random generation must not depend on Wi-Fi");
assert(consoleSource.includes('{"random", "Generate and save a random decoded value"'),
       "UART random command registration missing");

const webHandler = sourceSection(
  web, "esp_err_t random_signal_handler(", "esp_err_t delete_signal_handler(",
  "Web random handler");
assert(webHandler.includes("validate_mutation_headers(request, false)") &&
       webHandler.includes("request->content_len != 0") &&
       webHandler.includes("bridge_control_generate_and_save_random_decoded") &&
       webHandler.includes("random_generation_exhausted") &&
       webHandler.includes('send_json(request, "201 Created", response)'),
       "Web random route validation, errors, or response missing");
assert(!webHandler.includes("true_random_unavailable") &&
       !webHandler.includes("ESP_ERR_NOT_SUPPORTED"),
       "Web random generation must not expose a Wi-Fi entropy error");
assert(!webHandler.includes("transmit"), "Web random handler must never transmit RF");
assert(web.includes('{"/api/signals/random", HTTP_POST, random_signal_handler'),
       "Web random route registration missing");

console.log("Random signal source contracts passed");
