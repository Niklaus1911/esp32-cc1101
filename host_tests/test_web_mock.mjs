import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const root = process.argv[2];
assert(root, "project root argument is required");
const { createMockServer, SCENARIOS } = await import(pathToFileURL(join(root, "tools", "web-mock.mjs")).href);

// Every JSON key a firmware handler can emit, taken from its escaped format strings.
function firmwareKeys(file, start, end) {
  const source = readFileSync(join(root, file), "utf8");
  const from = source.indexOf(start);
  const to = source.indexOf(end, from + start.length);
  assert(from >= 0 && to > from, `${start} source section missing`);
  return new Set([...source.slice(from, to).matchAll(/\\"([a-z_0-9]+)\\":/g)].map((match) => match[1]));
}

function collectKeys(value, keys) {
  if (Array.isArray(value)) value.forEach((item) => collectKeys(item, keys));
  else if (value && typeof value === "object") {
    for (const [key, child] of Object.entries(value)) {
      keys.add(key);
      collectKeys(child, keys);
    }
  }
  return keys;
}

const endpoints = {
  "/api/live": firmwareKeys("components/web_ui/web_api.cpp", "esp_err_t live_handler(", "esp_err_t signals_handler("),
  "/api/signals": firmwareKeys("components/web_ui/web_api.cpp", "esp_err_t signals_handler(", "esp_err_t recent_handler("),
  "/api/recent": firmwareKeys("components/web_ui/web_api.cpp", "esp_err_t recent_handler(", "esp_err_t rules_handler("),
  "/api/rules": firmwareKeys("components/web_ui/web_api.cpp", "esp_err_t rules_handler(", "esp_err_t learn_handler("),
  "/api/v1/ota/status": firmwareKeys("components/ota_update/ota_update.cpp", "bool make_status_json(",
                                     "esp_err_t status_handler("),
};

const { server, setScenario, state } = createMockServer();
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const base = `http://127.0.0.1:${server.address().port}`;
try {
  const seen = Object.fromEntries(Object.keys(endpoints).map((path) => [path, new Set()]));
  for (const scenario of SCENARIOS.filter((name) => !["disconnected", "rebooting", "list-failure"].includes(name))) {
    assert(setScenario(scenario));
    for (const path of Object.keys(endpoints)) {
      const response = await fetch(base + path);
      assert.equal(response.status, 200, `${scenario} ${path} must answer`);
      collectKeys(await response.json(), seen[path]);
    }
  }
  for (const [path, expected] of Object.entries(endpoints)) {
    const invented = [...seen[path]].filter((key) => !expected.has(key));
    const missing = [...expected].filter((key) => !seen[path].has(key));
    assert.deepEqual(invented, [], `${path} mock invents fields the firmware never sends`);
    assert.deepEqual(missing, [], `${path} mock scenarios never exercise firmware fields`);
  }

  const random = firmwareKeys("components/web_ui/web_api.cpp", "esp_err_t random_signal_handler(",
                              "esp_err_t delete_signal_handler(");
  assert(setScenario("healthy"));
  const created = await fetch(`${base}/api/signals/random`, { method: "POST" });
  assert.equal(created.status, 201);
  assert.deepEqual([...collectKeys(await created.json(), new Set())].sort(), [...random].sort(),
                   "random signal response shape drifted");

  state.mutations.length = 0;
  const transmit = await fetch(`${base}/api/transmit/raw`, {
    method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: "start_level=1&durations=100,200,300,400,500,600,700,800&repeats=2",
  });
  assert.equal(transmit.status, 200);
  assert.deepEqual(state.mutations.map((entry) => `${entry.method} ${entry.path}`), ["POST /api/transmit/raw"],
                   "mutations must be recorded and intercepted");
  const ota = await fetch(`${base}/api/v1/ota`, { method: "POST", body: new Uint8Array(512) });
  assert.equal(ota.status, 409, "firmware uploads must never be accepted by the mock");

  assert(setScenario("list-failure"));
  assert.equal((await fetch(`${base}/api/rules`)).status, 503);
  assert.equal((await fetch(`${base}/api/live`)).status, 200, "list failures must keep live status reachable");
  assert(setScenario("disconnected"));
  await assert.rejects(fetch(`${base}/api/live`), "disconnected scenario must drop API connections");
  assert.equal((await fetch(`${base}/app.js`)).status, 200, "assets stay reachable for page reloads");
} finally {
  server.closeAllConnections();
  server.close();
}
console.log("Web mock API contract tests passed");
