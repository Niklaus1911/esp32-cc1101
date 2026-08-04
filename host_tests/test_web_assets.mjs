import assert from "node:assert/strict";
import { readFileSync, statSync } from "node:fs";
import { join } from "node:path";

const root = process.argv[2];
assert(root, "project root argument is required");
const component = join(root, "components", "web_ui");
const api = readFileSync(join(component, "web_api.cpp"), "utf8");
const forms = readFileSync(join(component, "web_form.cpp"), "utf8");
const lifecycle = readFileSync(join(component, "web_ui.cpp"), "utf8");
const cmake = readFileSync(join(component, "CMakeLists.txt"), "utf8");
const index = readFileSync(join(component, "assets", "index.html"), "utf8");
const css = readFileSync(join(component, "assets", "app.css"), "utf8");
const js = readFileSync(join(component, "assets", "app.js"), "utf8");
const main = readFileSync(join(root, "main", "main.cpp"), "utf8");
const radioHeader = readFileSync(join(root, "components", "rf_ook", "include", "rf_ook.hpp"), "utf8");
const consoleSource = readFileSync(join(root, "components", "rf_console", "rf_console.cpp"), "utf8");
const source = `${api}\n${forms}\n${lifecycle}\n${cmake}`;

assert.match(lifecycle, /constexpr uint16_t kHttpPort = 80;/);
assert.match(lifecycle, /config\.max_uri_handlers = 20;/);
assert.match(lifecycle, /config\.max_open_sockets = 2;/);
assert.match(api, /constexpr uint16_t kHttpPort = 80;/);
assert.match(api, /Referrer-Policy", "same-origin/);
assert.match(api, /web_origin_matches_ipv4/);
assert.match(forms, /return port == 80;/);
assert.match(cmake, /EMBED_TXTFILES "assets\/index\.html" "assets\/app\.css" "assets\/app\.js"/);

for (const route of [
  '"/api/live", HTTP_GET, live_handler',
  '"/api/signals", HTTP_GET, signals_handler',
  '"/api/rules", HTTP_GET, rules_handler',
  '"/api/learn", HTTP_POST, learn_handler',
  '"/api/learn", HTTP_DELETE, cancel_learn_handler',
  '"/api/replay", HTTP_POST, replay_handler',
  '"/api/signals", HTTP_DELETE, delete_signal_handler',
  '"/api/transmit/decoded", HTTP_POST, decoded_transmit_handler',
  '"/api/transmit/raw", HTTP_POST, raw_transmit_handler',
  '"/api/rules", HTTP_POST, add_rule_handler',
  '"/api/rules", HTTP_DELETE, remove_rule_handler',
  '"/api/rules", HTTP_PATCH, patch_rule_handler',
]) assert(api.includes(route), `missing Web route: ${route}`);

assert(source.includes("register_ota_http_handlers"), "OTA must share the Web server");
assert(source.includes("authorize_web_ota_request"), "OTA must use Host/origin authorization");
assert(api.includes("bridge_control_replay_named"), "named replay route missing");
assert(api.includes("bridge_control_transmit_decoded"), "decoded transmit route missing");
assert(api.includes("bridge_control_transmit_raw"), "raw transmit route missing");
assert(api.includes("rf_automation_add_rule"), "rule add route missing");
assert(api.includes("rf_automation_set_enabled"), "rule setting route missing");
assert(main.indexOf("initialize_ota_update()") < main.indexOf("initialize_bridge_events()"),
       "OTA must initialize before the broker installs its event sink");
assert(!radioHeader.includes("set_rf_receive_enabled"), "receiver disable API must remain absent");
assert(!consoleSource.includes('{"rx",'), "receiver disable command must remain absent");
assert(api.includes("kMaximumActionBodySize = 2048"), "action body limit changed");
assert(api.includes('httpd_resp_set_hdr(request, "Connection", "close")'),
       "rejected requests must close their connection");

assert(index.includes("/app.css") && index.includes("/app.js"), "static assets missing");
assert(index.includes('rel="icon" href="data:,"'), "embedded favicon contract missing");
assert(api.includes("img-src 'self' data:"), "favicon CSP contract missing");
assert(index.includes("Replay"), "Replay control missing");
assert(index.includes("Install and reboot"), "OTA control missing");
assert(js.includes("/api/live") && js.includes("schedulePoll"), "live polling missing");
assert(js.includes("document.hidden"), "visibility-aware polling missing");
assert(js.includes("pollController") && js.includes("cancelPolling"), "single-flight poll cancellation missing");
assert(js.includes("otaRebooting"), "OTA reconnect state missing");
assert(js.includes("XMLHttpRequest"), "OTA progress upload missing");
assert(js.includes('"rfbridge.observed-activity.v1"'), "versioned activity storage key missing");
assert(js.includes("sessionStorage.getItem(activityStorageKey)"), "activity restoration missing");
assert(js.includes("sessionStorage.setItem(activityStorageKey"), "activity persistence missing");
assert(js.includes("sessionStorage.removeItem(activityStorageKey)"), "activity Clear persistence missing");
assert.match(js, /maximumActivityEntries = 50;/, "activity entry bound changed");
assert.match(js, /maximumActivityStorageLength = 64 \* 1024;/, "activity storage bound changed");
assert(js.lastIndexOf("restoreActivity();") < js.lastIndexOf("renderActivity();"),
       "activity must restore before its initial render");
assert(js.includes("live.automation.actions + live.automation.tx_errors"),
       "rule execution watermark missing");
assert(js.includes('rulesView.hidden') && js.includes("candidate.dataset.trigger === trigger"),
       "visible exact-trigger rule lookup missing");
assert(js.includes('classList.add("rule-triggered")') &&
       js.includes('classList.remove("rule-triggered")'), "rule pulse lifecycle missing");
assert.match(css, /\.item\.rule-triggered \{ animation: rule-trigger-pulse 900ms ease-out; \}/,
             "rule trigger animation missing");
assert(css.includes("@keyframes rule-trigger-pulse"), "rule trigger keyframes missing");
assert(css.includes("@media (prefers-reduced-motion: reduce)"),
       "reduced-motion rule trigger treatment missing");
assert(css.includes("@media"), "responsive CSS missing");
for (const forbidden of [
  '"/probe"', "text/event-stream", "WebSocket", "setInterval", "Authorization",
  "httpd_uri_match_wildcard", "body:{command:", "no-referrer", "8032",
]) {
  assert(!source.includes(forbidden) && !index.includes(forbidden) && !js.includes(forbidden),
         `forbidden or stale Web contract remains: ${forbidden}`);
}
for (const [name, content] of [["index.html", index], ["app.css", css], ["app.js", js]]) {
  const maximumSize = name === "app.js" ? 24 * 1024 : 20000;
  assert(statSync(join(component, "assets", name)).size < maximumSize, `${name} is too large`);
  assert(content.length > 100, `${name} is unexpectedly empty`);
}
console.log("Responsive Web contracts passed");
