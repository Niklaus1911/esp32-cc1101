import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import vm from "node:vm";

const root = process.argv[2];
assert(root, "project root argument is required");
const source = readFileSync(process.argv[3] || join(root, "components/web_ui/assets/app.js"), "utf8");
// Exercise the production refresh functions without running page startup or rendering a DOM.
const startup = source.lastIndexOf('if (!matchMedia(');
assert(startup > 0, "page startup boundary missing");

function harness(view = "rules", streaming = true) {
  let now = 100000;
  let nextTimer = 0;
  const timers = new Map();
  const requests = [];
  const renders = { signals: 0, rules: 0, recent: 0 };
  const notices = [];
  let reloads = 0;
  const panels = { signals: { hidden: view !== "signals" }, rules: { hidden: view !== "rules" } };
  const data = {
    live: { learning: { revision: 1, state: "idle" }, recent: { revision: 1 },
            automation: { configuration_revision: 1 } },
    signals: { signals: [{ name: "gate" }, { name: "lamp" }] },
    rules: { revision: 1, enabled: false, log_mode: "off", rules: [] },
    recent: { revision: 1, entries: [] },
  };
  const failures = new Set();
  class Clock extends Date { static now() { return now; } }
  class Events {
    readyState = 1;
    listeners = new Map();
    addEventListener(kind, callback) { this.listeners.set(kind, callback); }
    close() { this.readyState = 2; }
  }
  const context = vm.createContext({
    console, AbortController, Date: Clock, EventSource: Events,
    setTimeout(callback, delay) { const id = ++nextTimer; timers.set(id, { callback, delay }); return id; },
    clearTimeout(id) { timers.delete(id); },
    document: {
      hidden: false,
      querySelectorAll() { return []; },
      querySelector(selector) { return selector.includes('"signals"') ? panels.signals : panels.rules; },
      getElementById() { return {}; },
    },
    sessionStorage: { setItem() {} },
    location: { reload() { reloads++; } },
    async fetch(path) {
      requests.push(path);
      if (failures.has(path)) throw new Error("mock unavailable");
      return { ok: true, async json() { return structuredClone(data.live); } };
    },
  });
  const api = vm.runInContext(source.slice(0, startup) +
    "\n({state, pollLive, schedulePoll, refreshViews, refreshSignalsAndRules, refreshRecent, openEvents, beginOtaConfirmation})", context);
  context.readJson = async (path) => {
    requests.push(path);
    if (failures.has(path)) throw new Error("mock unavailable");
    return structuredClone(data[path === "/api/v1/ota/status" ? "ota" : path.slice(5)]);
  };
  context.renderLive = () => {};
  context.setConnection = () => {};
  context.showNotice = (message) => notices.push(message);
  context.renderSignals = () => { renders.signals++; };
  context.renderRules = (payload) => { api.state.rules = payload.rules; renders.rules++; };
  context.renderRecent = () => { renders.recent++; };
  context.renderOtaStatus = () => {};
  if (streaming) api.state.sse = new Events();
  return {
    ...api, data, requests, renders, failures, notices, panels, context,
    reloads() { return reloads; },
    advance(milliseconds) { now += milliseconds; },
    async seed() {
      await api.refreshSignalsAndRules();
      await api.refreshRecent();
      api.state.learningRevision = data.live.learning.revision;
      requests.length = 0;
    },
    async event(kind) {
      api.state.kinds.add(kind);
      await api.refreshViews();
    },
    async flush(delay) {
      const entry = [...timers].find(([, timer]) => timer.delay === delay);
      assert(entry, `no ${delay} ms timer scheduled`);
      timers.delete(entry[0]);
      await entry[1].callback();
    },
  };
}

for (const kind of ["hello", "resync", "watchdog"]) {
  const h = harness();
  await h.seed();
  h.data.signals.signals.push({ name: "new_signal" });
  h.data.rules.enabled = true;
  h.data.rules.rules.push({ trigger: "gate", target: "new_signal" });
  h.data.rules.revision = 2;
  h.data.live.automation.configuration_revision = 2;
  await h.event(kind);
  assert(h.requests.includes("/api/signals"), `${kind} must reconcile the catalog and rule selectors`);
  assert(h.requests.includes("/api/rules"), `${kind} must reconcile rules and configuration controls`);
  assert.equal(h.state.signals.length, 3);
  assert.equal(h.state.rules.length, 1);
}

{
  const h = harness("signals");
  await h.seed();
  await h.event("resync");
  assert(h.requests.includes("/api/recent"), "resync must refresh recent history even if its revision reset");
  assert(h.requests.includes("/api/signals"));
  assert(!h.requests.includes("/api/rules"), "hidden rules need no recovery request");
}

{
  const h = harness("rules", false);
  await h.seed();
  h.data.rules.revision = 2;
  h.data.rules.enabled = true;
  h.data.live.automation.configuration_revision = 2;
  await h.pollLive();
  assert(h.requests.includes("/api/rules"), "polling must follow configuration revisions immediately");
  assert.equal(h.state.rulesRevision, 2);
  h.requests.length = 0;
  h.advance(1000);
  await h.pollLive();
  assert.deepEqual(h.requests, ["/api/live"], "unchanged polling must not refetch lists each second");
  const rendered = { ...h.renders };
  h.requests.length = 0;
  h.advance(30000);
  await h.pollLive();
  assert(h.requests.includes("/api/signals") && h.requests.includes("/api/rules"),
         "polling must periodically reconcile catalogs even without a catalog revision");
  assert.deepEqual(h.renders, rendered, "unchanged snapshots must preserve existing list controls and focus");
  h.data.signals.signals[0].name = "replacement";
  h.data.rules.rules.push({ trigger: "replacement", target: "lamp" });
  h.advance(30000);
  await h.pollLive();
  assert.equal(h.state.signals[0].name, "replacement", "same-count catalog changes must be detected");
  assert.equal(h.state.rules.length, 1, "periodic snapshots must recover a reset or unchanged revision");
}

{
  const h = harness("rules", false);
  await h.seed();
  h.data.live.automation.configuration_revision = 2;
  h.data.rules.revision = 2;
  h.failures.add("/api/rules");
  await h.pollLive();
  assert.equal(h.state.rulesRevision, 1, "failed refresh must not acknowledge the new revision");
  h.failures.clear();
  h.requests.length = 0;
  await h.pollLive();
  assert(h.requests.includes("/api/rules"), "failed rule refresh must retry on the next poll");
  assert.equal(h.state.rulesRevision, 2);
}

{
  const h = harness("signals", false);
  await h.seed();
  h.data.live.learning.revision++;
  await h.pollLive();
  assert(h.requests.includes("/api/signals"), "polling must preserve learning-driven refreshes");
  h.requests.length = 0;
  h.panels.signals.hidden = true;
  h.advance(30000);
  await h.pollLive();
  assert.deepEqual(h.requests, ["/api/live"], "background panels need no list polling");
  h.context.document.hidden = true;
  h.requests.length = 0;
  await h.pollLive();
  assert.equal(h.requests.length, 0, "hidden pages must not poll");
}

{
  const h = harness();
  await h.seed();
  h.state.sse = null;
  h.openEvents();
  const source = h.state.sse;
  source.onopen();
  source.listeners.get("hello")({ data: '{"snapshot_required":true}' });
  await h.flush(100);
  assert.equal(h.requests.filter((path) => path === "/api/signals").length, 1,
               "open and hello must coalesce into one catalog snapshot");
  source.onerror();
  h.requests.length = 0;
  h.data.rules.revision++;
  h.data.live.automation.configuration_revision++;
  await h.flush(0);
  assert(h.requests.includes("/api/rules"), "stream failure must recover configuration through polling");
}

{
  const h = harness("rules", false);
  await h.seed();
  h.state.otaAttempt = { originalPartition: "ota_0", targetPartition: "ota_1", expectedDigest: "ab" };
  h.data.ota = { state: "pending_reboot", running_partition: "ota_0" };
  h.beginOtaConfirmation("mock accepted");
  await h.flush(3000);
  await h.flush(0);
  assert(h.requests.includes("/api/v1/ota/status"), "OTA reboot must poll its confirmation status");
  assert(!h.requests.includes("/api/signals") && !h.requests.includes("/api/rules"),
         "OTA confirmation must keep list recovery paused");
  assert.equal(h.reloads(), 0, "pending reboot must not claim confirmation");
  h.data.ota = { state: "idle", running_partition: "ota_1", running_elf_sha256: "ab",
                 running_image_state: "valid", running_image_state_error: "ESP_OK",
                 confirmation_error: "ESP_OK" };
  h.advance(1000);
  await h.flush(1000);
  assert.equal(h.reloads(), 1, "confirmed exact image must reload and resume page startup");
}

console.log("Web refresh behavior tests passed");
