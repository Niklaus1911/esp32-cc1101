#!/usr/bin/env node
// Deterministic local mock of the RFBridge Web API for browser and visual validation.
// Serves the real embedded assets, answers /api/* from scripted scenarios, and intercepts
// every mutation: nothing here can reach hardware. Binds to 127.0.0.1 only.
//
//   node tools/web-mock.mjs [--port 8765] [--scenario healthy]
//
// Control endpoints (GET):
//   /mock/scenario?name=<scenario>   switch scenario (see SCENARIOS)
//   /mock/emit?kind=<event>          push an SSE event (rx, catalog, learning, ...)
//   /mock/add-recent                 add a decoded reception and push "rx"
//   /mock/log[?clear=1]              recorded requests and intercepted mutations

import { createServer } from "node:http";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const DEFAULT_ASSETS = join(dirname(fileURLToPath(import.meta.url)), "..", "components", "web_ui", "assets");
const ASSET_TYPES = {
  "index.html": "text/html; charset=utf-8",
  "app.css": "text/css; charset=utf-8",
  "app.js": "text/javascript; charset=utf-8",
};
const REBOOT_WINDOW_MS = 5000;

// healthy: typical CC1101 device. empty: fresh device with no lists. busy: full lists,
// maximum-length names, raw latest frame, generic backend on an S3 with PSRAM.
// degraded: faults and errors everywhere. list-failure: list endpoints return 503.
// disconnected: device unreachable. rebooting: unreachable for a few seconds, then healthy.
export const SCENARIOS = ["healthy", "empty", "busy", "degraded", "list-failure", "disconnected", "rebooting"];

const decoded = (code, extra = {}) => ({
  encoding: "decoded", code: `0x${code.toString(16).toUpperCase()}`, bits: 24, protocol: 1, pulse_us: 350, ...extra,
});

function recentEntries(scenario) {
  if (scenario === "empty") return [];
  const count = scenario === "busy" ? 5 : 2;
  return Array.from({ length: count }, (_, index) => {
    const id = 42 - index;
    const code = 0x5a5a00 + id;
    const named = index === 1;
    return { id: String(id), code: `0x${code.toString(16).toUpperCase()}`, code_decimal: String(code), bits: 24,
             protocol: 1, pulse_us: 350 + index, match: named ? "unique" : "none",
             match_name: named ? "garage_door" : "", match_count: named ? 1 : 0 };
  });
}

function signalList(scenario) {
  if (scenario === "empty") return [];
  const base = [
    { name: "garage_door", ...decoded(0x5a5a29) },
    { name: "porch_light", ...decoded(0x123456, { bits: 32, protocol: 2, pulse_us: 650 }) },
    { name: "gate_raw", encoding: "raw", pulses: 50, start_level: 1 },
  ];
  if (scenario === "degraded") return [...base, { name: "broken_rec", error: "ESP_ERR_INVALID_CRC" }];
  if (scenario !== "busy") return base;
  return Array.from({ length: 32 }, (_, index) => index % 4 === 3
    ? { name: `raw_capture_${String(index).padStart(3, "0")}`, encoding: "raw", pulses: 48 + index * 2, start_level: index % 2 }
    : { name: `living_room_${String(index).padStart(3, "0")}`, ...decoded(0xa00000 + index * 4099) });
}

function ruleList(scenario) {
  if (scenario === "empty") return [];
  const rule = (trigger, target, extra = {}) =>
    ({ trigger, target, repeats: 8, cooldown_ms: 2000, valid: true, error: "ESP_OK", ...extra });
  if (scenario === "busy") {
    return Array.from({ length: 8 }, (_, index) => rule(`living_room_${String(index * 2).padStart(3, "0")}`,
      `living_room_${String(index * 2 + 1).padStart(3, "0")}`,
      index === 5 ? { valid: false, error: "ESP_ERR_NOT_FOUND" } : { repeats: 1 + index }));
  }
  if (scenario === "degraded") return [rule("garage_door", "missing_sig", { valid: false, error: "ESP_ERR_NOT_FOUND" })];
  return [rule("garage_door", "porch_light")];
}

function live(state) {
  const { scenario } = state;
  const busy = scenario === "busy";
  const bad = scenario === "degraded";
  const recent = recentEntries(scenario);
  const lists = { recent: recent.length, rules: ruleList(scenario).length, signals: signalList(scenario).length };
  const cc1101 = bad ? { available: false, error: "ESP_ERR_TIMEOUT" } : {
    available: true, error: "ESP_OK", part: 0, version: 20, marc_state: 13, rssi_dbm_x2: -183,
    carrier_sense: false, clear_channel: true, resets: 1, recoveries: 0, ready_timeouts: 0, state_timeouts: 0,
  };
  const match = (name) => ({ match: name ? "unique" : "none", match_name: name, match_count: name ? 1 : 0 });
  const last = scenario === "empty" ? null
    : bad ? { error: "ESP_ERR_INVALID_STATE" }
    : busy ? { encoding: "raw", pulses: 96, start_level: 1, fingerprint: "0x9C41E2A7", repeats: 3, captured_us: 812345678, ...match("") }
    : { ...decoded(0x5a5a29), repeats: 4, captured_us: 812345678, ...match("garage_door") };
  const memory = (total, free) => ({ total, free, minimum: Math.round(free * 0.8), largest: Math.round(free * 0.5) });
  const internalFree = bad ? 10240 : 118000;
  const psram = busy ? memory(8388608, 8100000) : memory(0, 0);
  const generic = state.generic;
  return {
    radio: {
      available: true, running: !bad, hardware: busy ? "generic" : "cc1101",
      hardware_switch_error: bad ? "ESP_ERR_TIMEOUT" : "ESP_OK", hardware_switches: busy ? 3 : 0,
      rx: bad ? "faulted" : "active", transmitting: false, maintenance: false, accepted: state.accepted,
      duplicates: 12, queue_drops: bad ? 7 : 0, timeouts: bad ? 2 : 0, truncated: bad ? 4 : 0,
      frequency_hz: 433920000, tx_power_dbm: 10, cc1101,
    },
    learning: {
      available: !bad, state: bad ? "failed" : "idle", revision: 3, name: bad ? "front_gate" : "", result: bad ? "ESP_ERR_TIMEOUT" : "ESP_OK",
      count: lists.signals, catalog_available: !bad, queue_drops: 0, catalog_errors: bad ? 1 : 0,
      initialization_error: "ESP_OK",
    },
    recent: { available: true, count: recent.length, revision: state.recentRevision, errors: bad ? 2 : 0,
              last_error: bad ? "ESP_ERR_NVS_NOT_ENOUGH_SPACE" : "ESP_OK" },
    last,
    automation: {
      available: true, enabled: !bad, runtime_paused: busy, log_mode: busy ? "verbose" : "actions",
      configuration_revision: 1, rules: lists.rules, frames: 314, matches: 21, stale: 0, ambiguous: bad ? 3 : 0,
      actions: 19, last_action_id: 19, suppressed: 4, tx_errors: bad ? 2 : 0, queue_drops: 0, log_events: 40,
      log_drops: 0, initialization_error: "ESP_OK", last_error: bad ? "ESP_ERR_TIMEOUT" : "ESP_OK",
      last_trigger: lists.rules ? "garage_door" : "", last_target: lists.rules ? "porch_light" : "",
    },
    network: {
      available: true, online: !bad, rssi: bad ? -88 : -57, state: bad ? "retry_wait" : "online",
      driver_initialized: true, driver_started: true, scan_running: false, ota_locked: false,
      ssid: bad ? "" : "home-iot", saved_ssid: "home-iot", failed_ssid: bad ? "guest-network" : "",
      saved_known: true, saved: true, active_saved: !bad,
      ip: bad ? "0.0.0.0" : "127.0.0.1", netmask: "255.255.255.0", gateway: "127.0.0.1", dns: "127.0.0.1",
      retries: bad ? 14 : 0, event_drops: 0, disconnect_reason: bad ? 201 : 0, initialization_error: "ESP_OK",
      persistence_error: "ESP_OK", last_error: bad ? "ESP_ERR_WIFI_NOT_CONNECT" : "ESP_OK",
    },
    mdns: {
      available: true, state: bad ? "stalled" : "ready", configured_hostname: "rfbridge", hostname_custom: busy,
      effective_hostname: busy ? "rfbridge-2" : "rfbridge", effective_known: true, conflict_renamed: busy,
      http_registered: !bad, rfbridge_registered: !bad, configured_generation: 2, applied_generation: bad ? 1 : 2,
      heartbeat_age_ms: bad ? 45000 : 120, initialization_error: "ESP_OK", last_error: bad ? "ESP_ERR_TIMEOUT" : "ESP_OK",
    },
    board: {
      profile: busy ? "esp32s3-devkitc-n16r8" : "esp32-devkit", model: busy ? "ESP32-S3 DevKitC N16R8" : "ESP32 DevKit",
      target: busy ? "esp32s3" : "esp32", flash_mib: busy ? 16 : 4, psram_mib: busy ? 8 : 0, console: "uart0",
      combined_services: busy, activity_led_enabled: !busy, activity_led_gpio: busy ? -1 : 2, activity_led_active_high: true,
      cc1101: busy ? { sclk: 12, miso: 13, mosi: 11, cs: 10, gdo0_tx: 4, gdo2_rx: 5 }
                   : { sclk: 18, miso: 19, mosi: 23, cs: 27, gdo0_tx: 26, gdo2_rx: 25 },
      generic: {
        tx: generic.tx, rx: generic.rx, default_tx: 32, default_rx: 33, saved_tx: generic.tx, saved_rx: generic.rx,
        saved: generic.tx !== 32 || generic.rx !== 33, pending: false, overlap_cc1101: busy,
        configuration_error: "ESP_OK", tx_options: [4, 5, 13, 14, 16, 17, 21, 22, 32, 33], rx_options: [4, 5, 13, 14, 16, 17, 21, 22, 32, 33, 34, 35],
      },
    },
    services: {
      requested: busy ? "both" : "web", boot: busy ? "both" : "web", effective: busy ? "both" : "web", configured: true,
      reboot_required: false, fallback: false, retirement: "active", maintenance: false,
      config_error: "ESP_OK", web_error: "ESP_OK", mqtt_error: bad ? "ESP_ERR_TIMEOUT" : "ESP_OK", status_error: "ESP_OK",
    },
    system: {
      uptime_ms: 273645000, reset_reason: bad ? "task_watchdog" : "power_on", heap_free: internalFree,
      heap_minimum: Math.round(internalFree * 0.8), heap_largest: Math.round(internalFree * 0.5),
      internal: memory(300000, internalFree), psram,
    },
    errors: {
      radio: "ESP_OK", signals: bad ? "ESP_ERR_INVALID_STATE" : "ESP_OK", automation: "ESP_OK",
      wifi: "ESP_OK", mdns: "ESP_OK", services: "ESP_OK",
    },
  };
}

function otaStatus(state) {
  const bad = state.scenario === "degraded";
  return {
    state: bad ? "failed" : "idle", server: true, upload: false, port: 80,
    board_profile: state.scenario === "busy" ? "esp32s3-devkitc-n16r8" : "esp32-devkit",
    running_partition: "ota_0", update_partition: "ota_1", running_version: "v1.0.1", candidate_version: "",
    running_elf_sha256: "3f1c9a0d5e7b2c4f8a6d1e0b9c7a5f3e2d4c6b8a0f1e3d5c7b9a2f4e6d8c0b1a", candidate_elf_sha256: "",
    running_image_state: "valid", running_image_state_error: "ESP_OK", confirmation_error: "ESP_OK",
    rollback_possible: true, pending_verification: false, bytes: 0, total: 0,
    error: bad ? "ESP_ERR_OTA_VALIDATE_FAILED" : "ESP_OK", maintenance_error: "ESP_OK", initialization_error: "ESP_OK",
  };
}

export function createMockServer({ assets = DEFAULT_ASSETS, scenario = "healthy" } = {}) {
  if (!SCENARIOS.includes(scenario)) throw new Error(`Unknown scenario ${scenario}`);
  const state = {
    scenario, accepted: 128, recentRevision: 1, nextRecentId: 43, extraRecent: [], extraSignals: [],
    generic: { tx: 32, rx: 33 }, unreachableUntil: 0, requests: [], mutations: [],
  };
  const streams = new Set();
  const emit = (kind, data = {}) => {
    for (const stream of streams) stream.write(`event: ${kind}\ndata:${JSON.stringify({ sequence: 1, ...data })}\n\n`);
  };
  const setScenario = (name) => {
    if (!SCENARIOS.includes(name)) return false;
    Object.assign(state, { scenario: name, extraRecent: [], extraSignals: [], generic: { tx: 32, rx: 33 } });
    state.unreachableUntil = name === "rebooting" ? Date.now() + REBOOT_WINDOW_MS : 0;
    if (name === "rebooting") state.scenario = "healthy";
    for (const stream of streams) stream.destroy();
    return true;
  };
  setScenario(scenario);
  const unreachable = () => state.scenario === "disconnected" || Date.now() < state.unreachableUntil;
  const recent = () => [...state.extraRecent, ...recentEntries(state.scenario)].slice(0, 5);

  const server = createServer((request, response) => {
    const url = new URL(request.url, "http://127.0.0.1");
    const path = url.pathname;
    state.requests.push(`${request.method} ${path}`);
    const json = (status, body) => {
      response.writeHead(status, { "Content-Type": "application/json", "Cache-Control": "no-store" });
      response.end(JSON.stringify(body));
    };
    const asset = path === "/" ? "index.html" : path.slice(1);
    if (request.method === "GET" && ASSET_TYPES[asset]) {
      response.writeHead(200, { "Content-Type": ASSET_TYPES[asset], "Cache-Control": "no-store" });
      return response.end(readFileSync(join(assets, asset)));
    }
    if (path.startsWith("/mock/")) {
      if (path === "/mock/scenario") {
        return setScenario(url.searchParams.get("name")) ? json(200, { ok: true, scenario: state.scenario })
                                                         : json(400, { ok: false, scenarios: SCENARIOS });
      }
      if (path === "/mock/emit") { emit(url.searchParams.get("kind") || "rx"); return json(200, { ok: true }); }
      if (path === "/mock/add-recent") {
        const id = state.nextRecentId++;
        const code = 0xabc000 + id;
        state.extraRecent.unshift({ id: String(id), code: `0x${code.toString(16).toUpperCase()}`, code_decimal: String(code),
                                    bits: 24, protocol: 1, pulse_us: 350, match: "none", match_name: "", match_count: 0 });
        state.recentRevision++;
        state.accepted++;
        emit("rx");
        return json(200, { ok: true, id: String(id) });
      }
      if (path === "/mock/log") {
        const body = { scenario: state.scenario, requests: state.requests, mutations: state.mutations };
        if (url.searchParams.get("clear") === "1") { state.requests = []; state.mutations = []; }
        return json(200, body);
      }
      return json(404, { ok: false });
    }
    if (unreachable()) return request.socket.destroy();
    const listFailure = state.scenario === "list-failure";
    if (request.method === "GET") {
      switch (path) {
        case "/api/live": return json(200, live({ ...state, scenario: listFailure ? "healthy" : state.scenario }));
        case "/api/recent":
          return listFailure ? json(503, { ok: false, error: "ESP_ERR_TIMEOUT", code: 263 })
                             : json(200, { revision: state.recentRevision, entries: recent() });
        case "/api/signals":
          return listFailure ? json(503, { ok: false, error: "ESP_ERR_TIMEOUT", code: 263 })
                             : json(200, { signals: [...signalList(state.scenario), ...state.extraSignals] });
        case "/api/rules": {
          if (listFailure) return json(503, { ok: false, error: "ESP_ERR_TIMEOUT", code: 263 });
          const bad = state.scenario === "degraded";
          return json(200, { enabled: !bad, available: true, log_mode: state.scenario === "busy" ? "verbose" : "actions",
                             revision: 1, rules: ruleList(state.scenario) });
        }
        case "/api/v1/ota/status": return json(200, otaStatus(state));
        case "/api/events":
          response.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-cache, no-store" });
          response.write(": connected\n\nid: 0\nevent: hello\ndata:{\"sequence\":0,\"snapshot_required\":true}\n\n");
          streams.add(response);
          request.on("close", () => streams.delete(response));
          return undefined;
        default: return json(404, { ok: false, error: "not_found" });
      }
    }
    // Intercept every mutation: record it and answer without any device effect.
    const chunks = [];
    request.on("data", (chunk) => { if (chunks.length < 64) chunks.push(chunk); });
    request.on("end", () => {
      let body = path === "/api/v1/ota" ? "<firmware upload intercepted>" : Buffer.concat(chunks).toString("utf8").slice(0, 2048);
      const wifi = path === "/api/wifi" ? new URLSearchParams(body) : null;
      if (wifi) body = `ssid=${encodeURIComponent(wifi.get("ssid") ?? "")}&password=<redacted>`;
      // Headers the firmware's same-origin and form checks depend on.
      state.mutations.push({ method: request.method, path, body, origin: request.headers.origin ?? null,
                             contentType: request.headers["content-type"] ?? null });
      if (path === "/api/v1/ota") return json(409, { error: "mock_ota_intercepted" });
      if (path === "/api/signals/random") {
        const signal = { name: `random_${(state.extraSignals.length + 0xd30a91).toString(16).toUpperCase()}`, ...decoded(0xd30a91 + state.extraSignals.length) };
        state.extraSignals.push(signal);
        emit("catalog");
        return json(201, { ok: true, signal: { ...signal, code_decimal: String(Number.parseInt(signal.code, 16)) } });
      }
      if (path === "/api/radio/generic-gpio") {
        const form = new URLSearchParams(body);
        const tx = Number(form.get("tx_gpio"));
        const rx = Number(form.get("rx_gpio"));
        if (tx === state.generic.tx && rx === state.generic.rx) return json(200, { ok: true, rebooting: false });
        state.generic = { tx, rx };
        json(202, { ok: true, rebooting: true, tx_gpio: tx, rx_gpio: rx });
        state.unreachableUntil = Date.now() + REBOOT_WINDOW_MS;
        for (const stream of streams) stream.destroy();
        return undefined;
      }
      if (wifi) return json(202, { ok: true, ssid: wifi.get("ssid") ?? "" });
      if (path === "/api/recent" && new URLSearchParams(body).get("action") === "clear") {
        state.extraRecent = [];
        state.recentRevision++;
      }
      const status = path === "/api/learn" ? 202 : path === "/api/signals" && request.method === "POST" ? 201 : 200;
      return json(status, { ok: true });
    });
    return undefined;
  });
  return { server, state, setScenario, emit };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const argument = (name, fallback) => {
    const index = process.argv.indexOf(name);
    return index > 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
  };
  const port = Number(argument("--port", "8765"));
  const { server, state } = createMockServer({ scenario: argument("--scenario", "healthy") });
  server.listen(port, "127.0.0.1", () => {
    console.log(`RFBridge Web mock on http://127.0.0.1:${port}/ (scenario ${state.scenario}; mutations are intercepted)`);
  });
}
