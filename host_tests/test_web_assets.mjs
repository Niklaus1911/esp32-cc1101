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
const otaSource = readFileSync(join(root, "components", "ota_update", "ota_update.cpp"), "utf8");
const source = `${api}\n${forms}\n${lifecycle}\n${cmake}`;

function sourceSection(content, start, end, label) {
  const startOffset = content.indexOf(start);
  const endOffset = content.indexOf(end, startOffset + start.length);
  assert(startOffset >= 0 && endOffset > startOffset, `${label} source section missing`);
  return content.slice(startOffset, endOffset);
}

function assertFields(content, fields, label) {
  for (const field of fields) {
    assert(content.includes(`\\"${field}\\"`), `${label} field missing: ${field}`);
  }
}

const compactSource = (content) => content.replace(/\s+/g, " ");
const occurrenceCount = (content, value) => content.split(value).length - 1;

assert.match(lifecycle, /constexpr uint16_t kHttpPort = CONFIG_OTA_HTTP_PORT;/);
assert.match(lifecycle, /constexpr uint32_t kHttpTaskStackSize = CONFIG_OTA_HTTP_TASK_STACK_SIZE;/);
assert.match(lifecycle, /config\.max_uri_handlers = 22;/);
assert.match(lifecycle, /config\.max_open_sockets = 2;/);
assert.match(api, /constexpr uint16_t kHttpPort = CONFIG_OTA_HTTP_PORT;/);
assert.match(api, /Referrer-Policy", "same-origin/);
assert.match(api, /web_origin_matches_device_host/);
assert.match(api, /parse_web_device_host/);
assert.match(forms, /return port == 80;/);
assert.match(cmake, /EMBED_TXTFILES "assets\/index\.html" "assets\/app\.css" "assets\/app\.js"/);

for (const route of [
  '"/api/live", HTTP_GET, live_handler',
  '"/api/recent", HTTP_GET, recent_handler',
  '"/api/signals", HTTP_GET, signals_handler',
  '"/api/rules", HTTP_GET, rules_handler',
  '"/api/learn", HTTP_POST, learn_handler',
  '"/api/learn", HTTP_DELETE, cancel_learn_handler',
  '"/api/replay", HTTP_POST, replay_handler',
  '"/api/recent", HTTP_POST, recent_action_handler',
  '"/api/signals", HTTP_POST, create_signal_handler',
  '"/api/signals/random", HTTP_POST, random_signal_handler',
  '"/api/signals", HTTP_DELETE, delete_signal_handler',
  '"/api/transmit/decoded", HTTP_POST, decoded_transmit_handler',
  '"/api/transmit/raw", HTTP_POST, raw_transmit_handler',
  '"/api/rules", HTTP_POST, add_rule_handler',
  '"/api/rules", HTTP_DELETE, remove_rule_handler',
  '"/api/rules", HTTP_PATCH, patch_rule_handler',
  '"/api/radio/hardware", HTTP_POST, hardware_handler',
]) assert(api.includes(route), `missing Web route: ${route}`);

assert(source.includes("register_ota_http_handlers"), "OTA must share the Web server");
assert(source.includes("authorize_web_ota_request"), "OTA must use Host/origin authorization");
for (const field of [
  "board_profile", "running_elf_sha256", "candidate_elf_sha256", "running_image_state",
  "running_image_state_error", "confirmation_error", "initialization_error",
]) assert(otaSource.includes(`\\"${field}\\"`), `OTA status identity field missing: ${field}`);
assert(otaSource.includes('\\"target_partition\\"') &&
       otaSource.includes('\\"candidate_version\\"') &&
       otaSource.includes('\\"candidate_elf_sha256\\"'),
       "OTA acceptance response identity fields missing");
assert(api.includes("bridge_control_replay_named"), "named replay route missing");
assert(api.includes("bridge_control_save_decoded") &&
       api.includes('"201 Created"') &&
       forms.includes("parse_web_signal_save_form"),
       "manual decoded signal save route missing");
assert(api.includes("bridge_control_generate_and_save_random_decoded") &&
       api.includes("random_generation_exhausted") &&
       api.includes('send_json(request, "201 Created", response)'),
       "random decoded signal save route missing");
assert(!api.includes("true_random_unavailable"),
       "random signal save must not require Wi-Fi entropy");
assert(api.includes("bridge_control_replay_recent") &&
       api.includes("bridge_control_save_recent") &&
       api.includes("bridge_control_clear_recent"),
       "recent signal action routes missing");
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
assert(index.includes('id="recent-list"') && index.includes('id="refresh-recent"') &&
       index.includes('id="clear-recent"'), "recent decoded signal controls missing");
assert(index.includes('id="signal-save-form"') &&
       index.includes('id="signal-save-name"') &&
       index.includes('id="signal-save-code"'),
       "manual decoded signal form missing");
assert(index.includes('id="generate-random-signal"') &&
       index.includes("Generate &amp; save"),
       "random learned-signal action missing");
assert(index.includes("Install and reboot"), "OTA control missing");
assert(index.includes('id="radio-hardware"') && index.includes('id="apply-radio-hardware"') &&
       js.includes('requestAction("/api/radio/hardware","POST"'),
       "explicit Web RF hardware selector contract missing");
const hardwareRenderer = sourceSection(js, "function renderRadioHardware(",
                                       "function formatState(", "RF hardware renderer");
assert(hardwareRenderer.includes('["cc1101","generic"].includes') &&
       hardwareRenderer.includes("select.disabled=state.busy||!supported") &&
       hardwareRenderer.includes("apply.disabled=state.busy||!supported||!state.radioDirty") &&
       hardwareRenderer.includes("Backend selection unavailable on this firmware") &&
       hardwareRenderer.includes("hardware_switch_error") &&
       hardwareRenderer.includes("hardware_switches"),
       "RF hardware compatibility, feedback, or diagnostics renderer missing");
assert(js.includes("/api/live") && js.includes("schedulePoll"), "live polling missing");
const liveApi = sourceSection(api, "esp_err_t live_handler(", "esp_err_t signals_handler(",
                              "live API");
const radioApi = sourceSection(liveApi, '"{\\"radio\\":{', '"\\"learning\\":{',
                               "live radio");
const learningApi = sourceSection(liveApi, '"\\"learning\\":{',
                                  '"\\"last\\":null,', "live learning");
const recentApi = sourceSection(liveApi, '"\\"recent\\":{',
                                '"\\"last\\":null,', "live recent history");
const automationApi = sourceSection(liveApi, '"\\"automation\\":{',
                                    '"\\"network\\":{', "live automation");
const networkApi = sourceSection(liveApi, '"\\"network\\":{',
                                 '"\\"system\\":{', "live network");
const systemApi = sourceSection(liveApi, '"\\"system\\":{',
                                '"\\"errors\\":{', "live system");
const errorsApi = sourceSection(liveApi, '"\\"errors\\":{',
                                "if (error != ESP_OK)", "live errors");
assertFields(radioApi, [
  "available", "running", "hardware", "hardware_switch_error", "hardware_switches", "rx",
  "transmitting", "maintenance", "accepted", "duplicates",
  "queue_drops", "timeouts", "truncated", "frequency_hz", "tx_power_dbm", "cc1101", "error",
  "part", "version", "marc_state", "rssi_dbm_x2", "carrier_sense", "clear_channel", "resets",
  "recoveries", "ready_timeouts", "state_timeouts",
], "live radio");
assertFields(learningApi, [
  "available", "state", "revision", "name", "result", "count", "catalog_available",
  "queue_drops", "catalog_errors", "initialization_error",
], "live learning");
assertFields(recentApi, ["available", "count", "revision", "errors", "last_error"],
             "live recent history");
assertFields(automationApi, [
  "available", "enabled", "runtime_paused", "log_mode", "rules", "frames", "matches", "stale",
  "ambiguous", "actions", "suppressed", "tx_errors", "queue_drops", "log_events", "log_drops",
  "initialization_error", "last_error", "last_trigger", "last_target",
], "live automation");
assertFields(networkApi, [
  "available", "online", "rssi", "state", "driver_initialized", "driver_started", "scan_running",
  "ota_locked", "ssid", "saved_ssid", "saved_known", "saved", "active_saved", "ip", "netmask",
  "gateway", "dns", "retries", "event_drops", "disconnect_reason", "initialization_error",
  "persistence_error", "last_error",
], "live network");
const mdnsApi = sourceSection(liveApi, '"\\"mdns\\":{',
                              '"\\"system\\":{', "live mdns");
const boardApi = sourceSection(liveApi, '"\\"board\\":{',
                               '"\\"services\\":{', "live board");
assertFields(mdnsApi, [
  "available", "state", "configured_hostname", "hostname_custom", "effective_hostname",
  "effective_known", "conflict_renamed", "http_registered", "rfbridge_registered",
  "configured_generation", "applied_generation", "heartbeat_age_ms", "initialization_error",
  "last_error",
], "live mdns");
assertFields(boardApi, [
  "profile", "model", "target", "flash_mib", "psram_mib", "console", "combined_services",
  "activity_led_enabled", "activity_led_gpio", "activity_led_active_high", "cc1101", "sclk",
  "miso", "mosi", "cs", "gdo0_tx", "gdo2_rx", "generic", "tx", "rx",
], "live board");
assertFields(systemApi, [
  "uptime_ms", "reset_reason", "heap_free", "heap_minimum", "heap_largest",
  "internal", "psram", "total", "free", "minimum", "largest",
], "live system");
assertFields(errorsApi, ["radio", "signals", "automation", "wifi", "mdns"], "live errors");
assert(compactSource(radioApi).includes(
  'radio_error == ESP_OK ? "true" : "false", radio_error == ESP_OK && radio.running ? "true" : "false"'),
  "live radio availability must retain the outer status result");
assert(compactSource(learningApi).includes(
  'signals_error == ESP_OK && signals.available ? "true" : "false"'),
  "live learning availability must use the successful copied snapshot");
assert(compactSource(automationApi).includes(
  'automation_error == ESP_OK && automation.available ? "true" : "false"'),
  "live automation availability must use the successful copied snapshot");
assert(radioApi.includes('radio.cc1101_info_valid ? "true" : "false", esp_err_to_name(radio.cc1101_error)') &&
       radioApi.includes("error == ESP_OK && radio.cc1101_info_valid"),
       "CC1101 availability and optional details must use the chip-info snapshot");
const compactNetworkApi = compactSource(networkApi);
assert(compactNetworkApi.includes(
  'wifi_error == ESP_OK && wifi.available ? "true" : "false", wifi_error == ESP_OK && wifi.available && wifi.state == NetworkWifiState::kOnline && wifi.ip != 0 ? "true" : "false"'),
  "live network availability and online state must use one successful copied snapshot");
assert(!api.includes("network_wifi_is_online()"),
       "live API online state must come from the copied Wi-Fi snapshot");
assert(liveApi.includes("escape_web_json_string(wifi.active_ssid") &&
       liveApi.includes("escape_web_json_string(wifi.saved_ssid") &&
       occurrenceCount(systemApi, "memory.internal.free") === 2 &&
       occurrenceCount(systemApi, "memory.internal.minimum_free") === 2 &&
       occurrenceCount(systemApi, "memory.internal.largest_free_block") === 2,
       "live SSID escaping or internal-heap telemetry contract missing");
for (const id of [
  "system-status", "system-radio-summary", "system-cc1101-summary", "system-wifi-summary",
  "system-memory-summary", "system-radio-badge", "system-wifi-badge", "system-runtime-badge",
  "system-radio-details", "system-cc1101-details", "system-wifi-details", "system-network-details",
  "system-hardware-details", "system-runtime-details", "system-services-details",
  "system-rf-diagnostics", "system-wifi-diagnostics", "system-hardware-diagnostics",
  "system-services-diagnostics",
]) assert(index.includes(`id="${id}"`), `system status DOM target missing: ${id}`);
const firmwareDisclosure = sourceSection(index, '<details id="firmware-disclosure"',
                                         "</details>", "firmware disclosure");
for (const id of ["ota-state", "refresh-ota", "ota-form", "ota-file", "ota-progress", "ota-details"]) {
  assert(firmwareDisclosure.includes(`id="${id}"`), `firmware disclosure target missing: ${id}`);
}
assert(index.includes('id="firmware-disclosure" class="firmware-disclosure"') &&
       !/<details id="firmware-disclosure"[^>]*\sopen(?:\s|>)/.test(index) &&
       js.includes('if (!matchMedia("(max-width: 560px)").matches) byId("firmware-disclosure").open = true;'),
       "firmware disclosure must start closed and open only on initial desktop load");
for (const formatter of [
  "formatUptime", "formatBytes", "formatHalfDbm", "formatWifiQuality", "formatMarcState",
  "formatBoolean",
]) assert(js.includes(`function ${formatter}(`), `system formatter missing: ${formatter}`);
assert(js.includes("function renderSystem(live)") && js.includes("renderSystem(live);"),
       "live system render path missing");
const connectionSource = sourceSection(js, "function setConnection(",
                                       "async function readJson(", "connection renderer");
const pollSource = sourceSection(js, "async function pollLive(",
                                 "function stateClass(", "live poll");
const systemRenderer = sourceSection(js, "function renderSystem(",
                                     "function frameSummary(", "system renderer");
const liveRenderer = sourceSection(js, "function renderLive(",
                                   "function signalMeta(", "live renderer");
assert(connectionSource.includes('byId("system-status").classList.toggle("is-stale", !connected)'),
       "disconnected system snapshot handling missing");
assert(connectionSource.includes("if (connected) return;") &&
       !connectionSource.includes("renderSystem("),
       "connected state must not render the previous System snapshot");
assert(occurrenceCount(pollSource, "renderLive(live);") === 1 &&
       occurrenceCount(liveRenderer, "renderSystem(live);") === 1,
       "successful live polling must render the System snapshot exactly once");
assert(js.includes('classList.contains("is-stale")'),
       "cached system snapshots must not overwrite disconnected status");
assert(js.includes("timedOut = true") && js.includes('error.name !== "AbortError" || timedOut'),
       "live poll timeout must mark system status disconnected");
assert(connectionSource.includes(
  'document.querySelectorAll("#system-status .metric strong, #system-status .section-heading > .state")') &&
       connectionSource.includes('setStatus(target.id, "Disconnected", "bad"'),
       "disconnected system status treatment missing");
assert.match(js, /heapCriticalBytes = 12 \* 1024;/, "critical heap threshold changed");
assert.match(js, /heapWarningBytes = 24 \* 1024;/, "warning heap threshold changed");
assert(systemRenderer.includes('"system-memory-summary"') &&
       systemRenderer.includes("i.free<heapCriticalBytes") &&
       systemRenderer.includes("i.free<heapWarningBytes") &&
       systemRenderer.includes("b?.combined_services?32:16") &&
       systemRenderer.includes('frag?"Fragmented":"Low memory"'),
       "heap health tone missing from system summary");
const compactSystemRenderer = compactSource(systemRenderer);
assert(/const runTone\s*=\s*svcTone\s*===\s*"bad"\s*\|\|\s*hTone\s*===\s*"bad"\s*\?\s*"bad"\s*:\s*svcTone\s*===\s*"warn"\s*\|\|\s*hTone\s*===\s*"warn"\s*\?\s*"warn"\s*:\s*"ok";/.test(
         compactSystemRenderer),
       "runtime badge must include heap and service health");
const systemBindings =
  /const \{\s*radio:\s*(\w+),\s*learning:\s*(\w+),\s*automation:\s*(\w+),\s*network:\s*(\w+),\s*mdns:\s*(\w+),\s*system:\s*(\w+)\s*=\s*null,\s*errors:\s*(\w+)\s*=\s*\{\}\s*\}\s*=\s*live;/.exec(
    compactSystemRenderer);
assert(systemBindings, "previous live-schema object defaults missing");
const [, radioBinding, learningBinding, automationBinding, networkBinding, mdnsBinding, systemBinding] =
  systemBindings;
assert(compactSystemRenderer.includes(`const chip=${radioBinding}.cc1101;`) &&
       compactSystemRenderer.includes(`${mdnsBinding}.effective_hostname`) &&
       systemRenderer.includes('"Diagnostics unavailable"') &&
       systemRenderer.includes('Q+"."') &&
       js.includes('value ?? "-"') &&
       /function formatError\(\w+\) \{ return \w+ (?:=== undefined|== null) \? "-"/.test(js) &&
       /function formatBoolean\(\w+,[^}]+typeof \w+ === "boolean"/.test(js) &&
       compactSystemRenderer.includes(`free:${systemBinding}.heap_free`) &&
       compactSystemRenderer.includes("hLim=!i||i.free==null") &&
       compactSystemRenderer.includes(`const rLim=!chip||${radioBinding}.truncated == null;`) &&
       compactSystemRenderer.includes(
         `const wLim=${networkBinding}.state == null||${networkBinding}.event_drops == null;`) &&
       compactSystemRenderer.includes(
         `const sLim=${learningBinding}.catalog_available == null||${automationBinding}.log_drops == null||!${mdnsBinding};`),
       "previous live-schema diagnostics must use bounded defaults and visible fallbacks");
assert(compactSystemRenderer.includes('const wBadge=wTone === "bad"?"Faulted" :'),
       "hard Wi-Fi failures must not retain a stale online badge label");
assert(compactSystemRenderer.includes('const mqttExpected=v&&["mqtt","both"].includes(v.boot);') &&
       occurrenceCount(compactSystemRenderer, "mqttExpected&&H(v.mqtt_error)") === 2,
       "Web-only health must ignore the intentionally absent MQTT runtime");
assert(compactSystemRenderer.includes(
  `Boolean(${radioBinding}.transmitting||${radioBinding}.maintenance)`) &&
       compactSystemRenderer.includes("chip&&!chip.available") &&
       compactSystemRenderer.includes('["ESP_ERR_TIMEOUT","ESP_ERR_INVALID_STATE"].includes(chip.error)') &&
       /deferred\?`Deferred \/ \$\{chip\.error\}`/.test(systemRenderer) &&
       /!chip\|\|\w+\?"warn"/.test(systemRenderer),
       "busy CC1101 status deferral must remain warning-classified and retain its error");
assert(compactSystemRenderer.includes(`H(${radioBinding}.hardware_switch_error)`) &&
       compactSystemRenderer.includes("Hardware switch error:hardware_switch_error") &&
       compactSystemRenderer.includes("Hardware switches:hardware_switches"),
       "RF health and diagnostics must expose hardware switch failures");
const bindingNames = new Map([
  [radioBinding, "radio"], [learningBinding, "learn"], [automationBinding, "auto"],
  [networkBinding, "wifi"], [mdnsBinding, "mdns"],
]);
const healthCounterInputs = [...systemRenderer.matchAll(/hasCount\(([^)]*)\)/gs)]
  .map((match) => {
    let inputs = match[1].replaceAll("?.", ".");
    for (const [binding, name] of bindingNames) inputs = inputs.replaceAll(`${binding}.`, `${name}.`);
    return inputs;
  })
  .join(",");
for (const counter of [
  "radio.truncated", "radio.queue_drops", "radio.timeouts", "chip.recoveries",
  "chip.ready_timeouts", "chip.state_timeouts", "wifi.event_drops", "learn.queue_drops",
  "learn.catalog_errors", "auto.tx_errors", "auto.queue_drops", "auto.log_drops",
]) assert(healthCounterInputs.includes(counter), `health counter input missing: ${counter}`);
for (const counter of [
  "radio.duplicates", "chip.resets", "auto.stale", "auto.ambiguous", "auto.suppressed",
  "auto.log_events",
]) assert(!healthCounterInputs.includes(counter), `normal counter must not degrade health: ${counter}`);
for (const id of [
  "system-rf-diagnostics", "system-wifi-diagnostics", "system-hardware-diagnostics",
  "system-services-diagnostics",
]) {
  assert(js.includes(`D("${id}"`) || js.includes(`renderDetails(byId("${id}")`),
         `system diagnostics renderer missing: ${id}`);
}
assert(css.includes(".system-runtime-grid { grid-template-columns: repeat(3, minmax(0, 1fr)); }") &&
       /@media\s*\(max-width\s*:\s*820px\)[\s\S]*?\.system-grid\s*\{[^}]*grid-template-columns:\s*minmax\(0, 1fr\)/.test(css),
       "system hardware/runtime/services responsive grid missing");
assert(!js.includes('byId("runtime-details")'), "stale runtime details renderer remains");
assert(js.includes("document.hidden"), "visibility-aware polling missing");
assert(js.includes('readJson("/api/recent")') &&
       js.includes('requestAction("/api/recent", "POST"') &&
       js.includes("dataset.recentId"),
       "recent history load or ID-keyed actions missing");
assert(js.includes('action: "replay"') && js.includes('action: "save"') &&
       js.includes('action: "clear"') && js.includes("recentRevision"),
       "recent action forms or revision refresh missing");
assert(js.includes('bindPostForm("signal-save-form", "/api/signals"') &&
       js.includes('byId("signal-save-name").value = ""') &&
       js.includes('byId("signal-save-code").value = ""') &&
       js.includes("await refreshSignalsAndRules();"),
       "manual decoded signal save refresh flow missing");
const requestActionStart = js.indexOf(
  "async function requestAction(path, method, body = null, afterSuccess = null)");
const requestActionEnd = js.indexOf("function schedulePoll", requestActionStart);
const requestAction = js.slice(requestActionStart, requestActionEnd);
const afterSuccessOffset = requestAction.indexOf(
  "if (afterSuccess) await afterSuccess(payload);");
const finallyOffset = requestAction.indexOf("} finally {");
const pollingOffset = requestAction.indexOf("schedulePoll(0);");
assert(requestActionStart >= 0 && requestActionEnd > requestActionStart &&
       afterSuccessOffset >= 0 && finallyOffset > afterSuccessOffset &&
       pollingOffset > finallyOffset &&
       compactSource(js).includes(
         'await requestAction(path, "POST", formBody(data), complete);'),
       "post-action refresh must finish before live polling resumes");
assert(compactSource(js).includes(
         'requestAction("/api/signals/random", "POST", null, async (result) => { await refreshSignalsAndRules(); showNotice(`Saved ${result.signal.name} / ${result.signal.code}`, "success");') &&
       "random signal save feedback or refresh flow missing");
assert(compactSource(js).includes(
         "async function refreshSignalsAndRules() { await refreshSignals(); await refreshRules(); }") &&
       compactSource(js).includes(
         "async function refreshSignalsView() { cancelPolling(); try { await refreshRecent(); await refreshSignals(); } finally { schedulePoll(0); } }") &&
       compactSource(js).includes(
         "refreshRecent() .then(refreshSignals) .then(refreshRules) .then(refreshOta) .finally(()=>schedulePoll(0))") &&
       !js.includes("Promise.all([refresh"),
       "Web API refreshes must stay within the two-socket server limit");
assert(js.includes("pollController") && js.includes("cancelPolling"), "single-flight poll cancellation missing");
assert(liveRenderer.includes("const incomingRadio=live.radio||{}") &&
       liveRenderer.includes("renderRadioHardware(incomingRadio)"),
       "old live snapshots must route missing hardware through the compatibility renderer");
assert(js.includes("state.radioDirty=false") &&
       js.includes("renderRadioHardware(state.live?.radio)") &&
       js.includes("RF hardware switch failed; active backend restored"),
       "failed hardware switches must resynchronize the selector and report rollback");
assert(js.includes("otaRebooting") && js.includes("XMLHttpRequest"),
       "OTA reconnect or progress state missing");
assert(js.includes("function inspectOtaImage(") &&
       js.includes('project !== "esp32-cc1101"') &&
       js.includes("otaBoardProfiles.get(descriptor)") &&
       js.includes("otaElfSha256Offset"),
       "browser OTA image identity inspection missing");
assert(js.includes('live.services?.requested === "mqtt"') &&
       js.includes("image.profile !== deviceProfile") &&
       js.includes('["receiving", "validating", "pending_reboot"].includes(ota.state)'),
       "browser OTA preflight safety checks missing");
assert(js.includes('ota.running_image_state === "valid"') &&
       js.includes('ota.running_image_state_error === "ESP_OK"') &&
       js.includes('ota.confirmation_error === "ESP_OK"') &&
       js.includes("digest === attempt.expectedDigest") &&
       js.includes("location.reload()"),
       "browser OTA must require exact validated boot identity before success");
assert(js.includes("function expireOtaConfirmation(") &&
       js.includes("background checks will continue") &&
       js.includes("otaSuccessStorageKey"),
       "browser OTA timeout, late reconciliation, or one-time success feedback missing");
for (const outcome of [
  "returned to ${attempt.originalPartition}",
  "target partition booted with a different firmware digest",
  "cannot prove its image identity and validation state",
  "upload connection closed after transfer",
]) assert(js.includes(outcome), `browser OTA outcome handling missing: ${outcome}`);
assert(!js.includes('showNotice("Firmware reboot complete"'),
       "a successful live poll alone must not claim OTA success");
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
const mobileMediaMatch = /@media\s*\(\s*max-width\s*:\s*560px\s*\)\s*\{/.exec(css);
assert(mobileMediaMatch, "560px responsive CSS missing");
const mobileMediaOpen = mobileMediaMatch.index + mobileMediaMatch[0].lastIndexOf("{");
let mobileMediaDepth = 1;
let mobileMediaClose = -1;
for (let offset = mobileMediaOpen + 1; offset < css.length; ++offset) {
  if (css[offset] === "{") ++mobileMediaDepth;
  if (css[offset] === "}" && --mobileMediaDepth === 0) {
    mobileMediaClose = offset;
    break;
  }
}
assert.notEqual(mobileMediaClose, -1, "560px responsive CSS is unterminated");
const mobileCss = css.slice(mobileMediaOpen + 1, mobileMediaClose);
assert.match(mobileCss,
             /#rule-list\s+\.item-actions\s*\{[^}]*\bgrid-template-columns\s*:\s*1fr\s*;/,
             "mobile rule actions must use a single grid column");
assert.match(mobileCss,
             /\.recent-actions\s*\{[^}]*\bgrid-template-columns\s*:\s*76px\s+minmax\(0,\s*1fr\)\s+72px\s*;/,
             "mobile recent actions must use stable repeat name and save columns");
assert.match(mobileCss,
             /\.recent-actions\s+\.recent-name\s*\{[^}]*\bgrid-column\s*:\s*1\s*\/\s*3\s*;/,
             "mobile recent name input must span the repeat and flexible columns");
assert.match(mobileCss,
             /\.tabs-inner\s*\{[^}]*\bmin-width\s*:\s*100%\s*;[^}]*\bpadding\s*:\s*0\s*;/,
             "mobile navigation must fit the viewport without a scrolling inner width");
assert.match(mobileCss,
             /\.tab\s*\{[^}]*\bflex\s*:\s*1\s+1\s+20%\s*;[^}]*\bmin-width\s*:\s*0\s*;/,
             "mobile navigation tabs must share the viewport evenly");
assert.match(css, /@media\s*\(max-width\s*:\s*320px\s*\)\s*\{[^}]*\.tab\s*\{[^}]*font-size\s*:\s*\.7rem\s*;/s,
             "narrow mobile navigation must fit its longest label");
assert.match(mobileCss,
             /\.system-status-band\s+\.section-heading\s*\{[^}]*\bflex-direction\s*:\s*row\s*;[^}]*\balign-items\s*:\s*center\s*;/,
             "mobile System badges must stay beside their section headings");
assert.match(mobileCss,
             /#system-status\s+\.details\s*\{[^}]*\bgrid-template-columns\s*:\s*minmax\(104px,\s*132px\)\s+minmax\(0,\s*1fr\)\s*;/,
             "mobile System details must retain compact label/value columns");
assert.match(css, /\.firmware-disclosure\s*>\s*summary\s*\{[^}]*\bcursor\s*:\s*pointer\s*;/,
             "firmware disclosure must retain an interactive native summary");
assert.match(css, /\.firmware-disclosure\s*>\s*summary\s*\{[^}]*\bdisplay\s*:\s*list-item\s*;/,
             "firmware disclosure must retain its native expansion marker");
for (const forbidden of [
  '"/probe"', "text/event-stream", "WebSocket", "setInterval", "Authorization",
  "httpd_uri_match_wildcard", "body:{command:", "no-referrer", "8032",
]) {
  assert(!source.includes(forbidden) && !index.includes(forbidden) && !js.includes(forbidden),
         `forbidden or stale Web contract remains: ${forbidden}`);
}
for (const [name, content] of [["index.html", index], ["app.css", css], ["app.js", js]]) {
  const maximumSize = name === "app.js" ? 48 * 1024 : 20000;
  assert(statSync(join(component, "assets", name)).size < maximumSize, `${name} is too large`);
  assert(content.length > 100, `${name} is unexpectedly empty`);
}
console.log("Responsive Web contracts passed");
