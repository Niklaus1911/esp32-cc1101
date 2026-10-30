import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const root = process.argv[2];
assert(root, "project root argument is required");

const read = (...parts) => readFileSync(join(root, ...parts), "utf8");
const bridge = read("components", "bridge_control", "bridge_control.cpp");
const radio = read("components", "rf_ook", "rf_ook.cpp");
const radioHeader = read("components", "rf_ook", "include", "rf_ook.hpp");

function sourceSection(source, start, end, label) {
  const startOffset = source.indexOf(start);
  const endOffset = source.indexOf(end, startOffset + start.length);
  assert(startOffset >= 0 && endOffset > startOffset, `${label} source section missing`);
  return source.slice(startOffset, endOffset).replace(/\s+/g, " ");
}

const replay = sourceSection(bridge, "esp_err_t bridge_control_replay_last(",
                             "esp_err_t bridge_control_replay_named(", "latest-frame replay");
const snapshot = replay.indexOf("get_last_rf_frame(&frame)");
const started = replay.indexOf("BridgeEventType::kTxStarted");
const transmitted = replay.indexOf(
  "frame.encoding == RfEncoding::kDecoded ? transmit_rf_decoded(frame.decoded, repeats) : transmit_rf_raw(frame.raw, repeats)");
const completed = replay.indexOf("BridgeEventType::kTxCompleted");
assert(snapshot >= 0 && started > snapshot && transmitted > started && completed > transmitted,
       "latest-frame replay must transmit the same snapshot its TX events report");

// A separate "replay whatever is latest now" command could transmit a newer frame than
// the one reported, so the radio must not expose it.
for (const obsolete of ["replay_last_rf_frame", "kReplayLast"]) {
  assert(!radio.includes(obsolete) && !radioHeader.includes(obsolete) && !bridge.includes(obsolete),
         `racy latest-frame radio command remains: ${obsolete}`);
}

console.log("Bridge replay source contract tests passed");
