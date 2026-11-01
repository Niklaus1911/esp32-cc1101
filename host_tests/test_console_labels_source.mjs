import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const root = process.argv[2];
assert(root, "project root argument is required");

const read = (...parts) => readFileSync(join(root, ...parts), "utf8");
const consoleSource = read("components", "rf_console", "rf_console.cpp");
const formatSource = read("components", "rf_console", "rf_console_format.cpp");

const widthMatch = /constexpr std::size_t kDashboardLabelWidth = (\d+);/.exec(formatSource);
assert(widthMatch, "dashboard label width constant missing");
const labelWidth = Number(widthMatch[1]);

// Split a call's argument list on top-level commas.
function splitArguments(text) {
  const parts = [];
  let depth = 0;
  let current = "";
  let quoted = false;
  for (let index = 0; index < text.length; ++index) {
    const character = text[index];
    if (character === "\"" && text[index - 1] !== "\\") quoted = !quoted;
    if (!quoted && "([{".includes(character)) ++depth;
    if (!quoted && ")]}".includes(character)) --depth;
    if (!quoted && character === "," && depth === 0) {
      parts.push(current.trim());
      current = "";
    } else {
      current += character;
    }
  }
  parts.push(current.trim());
  return parts;
}

// The formatter rejects longer labels at run time: the row is dropped and an error is
// logged instead (found on hardware with "CC1101 diagnostics" in generic mode).
let checked = 0;
for (const match of consoleSource.matchAll(/print_dashboard_(value|row)\(([\s\S]*?)\);/g)) {
  const parts = splitArguments(match[2]);
  for (const position of match[1] === "value" ? [0] : [0, 3]) {
    const literal = /^"((?:[^"\\]|\\.)*)"$/.exec(parts[position] ?? "");
    if (!literal) continue;
    ++checked;
    assert(literal[1].length <= labelWidth,
           `dashboard label "${literal[1]}" exceeds ${labelWidth} characters and would be dropped`);
  }
}
assert(checked > 40, `too few dashboard labels found (${checked}); the call pattern may have changed`);

console.log(`Console dashboard label tests passed (${checked} labels)`);
