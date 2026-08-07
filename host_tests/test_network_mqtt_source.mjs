import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const root = process.argv[2];
assert(root, "project root argument is required");
const source = readFileSync(
  join(root, "components", "network_mqtt", "network_mqtt.cpp"), "utf8");

function functionSource(name) {
  const signature = source.indexOf(` ${name}(`);
  assert(signature >= 0, `${name} function missing`);
  const open = source.indexOf("{", signature);
  assert(open >= 0, `${name} function body missing`);
  let depth = 1;
  for (let offset = open + 1; offset < source.length; ++offset) {
    if (source[offset] === "{") ++depth;
    if (source[offset] === "}" && --depth === 0) return source.slice(signature, offset + 1);
  }
  assert.fail(`${name} function body is unterminated`);
}

const compact = (value) => value.replace(/\s+/g, " ");
const fixedStart = source.indexOf("constexpr MqttDiscoveryEntityKind kFixedEntityKinds[]");
const fixedEnd = source.indexOf("};", fixedStart);
assert(fixedStart >= 0 && fixedEnd > fixedStart, "fixed discovery entity catalog missing");
const fixedEntities = source.slice(fixedStart, fixedEnd);
for (const entity of [
  "kInternalFreeSensor", "kInternalMinimumSensor", "kInternalLargestSensor", "kPsramFreeSensor",
  "kHardwareSelect",
]) {
  assert(fixedEntities.includes(`MqttDiscoveryEntityKind::${entity}`),
         `fixed system discovery entity missing: ${entity}`);
}

const fixedPublisher = compact(functionSource("publish_fixed_discovery"));
assert(fixedPublisher.includes("for (const MqttDiscoveryEntityKind kind : kFixedEntityKinds)"),
       "fixed discovery publisher must reconcile the complete catalog");
assert(/kind\s*==\s*MqttDiscoveryEntityKind::kPsramFreeSensor\s*&&\s*current_board_info\(\)\.psram_mib\s*==\s*0/.test(
         fixedPublisher) && fixedPublisher.includes("continue;"),
       "PSRAM discovery must be skipped on boards without PSRAM");

const reconciliation = compact(functionSource("reconcile_discovery"));
assert(reconciliation.includes("publish_fixed_discovery(context)"),
       "discovery reconciliation must publish fixed diagnostic entities");
assert((reconciliation.match(/publish_system_state\(context\)/g) ?? []).length === 2,
       "fast and full reconciliation paths must publish system state");

const retirement = compact(functionSource("retire_discovery"));
assert(retirement.includes("for (const MqttDiscoveryEntityKind kind : kFixedEntityKinds)") &&
       retirement.includes("publish_entity_discovery_tombstone(context, kind, nullptr)"),
       "retirement must tombstone every fixed diagnostic entity");
assert(retirement.includes("MqttStateTopicKind::kSystem") &&
       retirement.includes('wait_for_publish(context, topic, "", 0)'),
       "retirement must tombstone retained system state");

const subscriptions = compact(functionSource("subscribe_topics"));
assert(subscriptions.includes("context->hardware_command_topic") &&
       subscriptions.includes("esp_mqtt_client_subscribe_multiple"),
       "RF hardware commands must be part of the acknowledged subscription set");
const commands = compact(functionSource("service_commands"));
assert(commands.includes("MqttCommandType::kSetHardware") &&
       commands.includes("bridge_control_set_rf_hardware(command.hardware, BridgeEventSource::kMqtt)"),
       "RF hardware MQTT commands must use the shared switching transaction");

console.log("MQTT runtime source contracts passed");
