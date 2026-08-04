"use strict";

const state = {
  busy: false,
  pollTimer: null,
  pollController: null,
  pollGeneration: 0,
  noticeTimer: null,
  otaRebooting: false,
  backoff: 1000,
  accepted: null,
  ruleExecutions: null,
  rulePulseTimer: null,
  rulePulseItem: null,
  learningRevision: null,
  live: null,
  signals: [],
  rules: [],
  activity: [],
};

const activityStorageKey = "rfbridge.observed-activity.v1";
const activityStorageVersion = 1;
const maximumActivityEntries = 50;
const maximumActivityStorageLength = 64 * 1024;
const maximumDateMilliseconds = 8640000000000000;

const byId = (id) => document.getElementById(id);
const text = (value) => document.createTextNode(String(value));

function element(tag, className, content) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (content !== undefined) node.append(text(content));
  return node;
}

function formBody(values) {
  return Object.entries(values).map(([key, value]) => `${key}=${String(value)}`).join("&");
}

function showNotice(message, kind = "") {
  const notice = byId("notice");
  notice.textContent = message;
  notice.className = `notice ${kind}`.trim();
  notice.hidden = false;
  clearTimeout(state.noticeTimer);
  state.noticeTimer = setTimeout(() => { notice.hidden = true; }, 6000);
}

function cancelPolling() {
  clearTimeout(state.pollTimer);
  state.pollTimer = null;
  state.pollGeneration += 1;
  if (state.pollController) {
    state.pollController.abort();
    state.pollController = null;
  }
}

function setBusy(busy) {
  state.busy = busy;
  document.querySelectorAll("main button, main input, main select, main textarea").forEach((control) => {
    if (control.id !== "ota-progress") control.disabled = busy || state.otaRebooting;
  });
  if (!busy && !state.otaRebooting) {
    renderLive(state.live);
    updateRuleSelectors();
  }
}

function setConnection(connected, label) {
  const badge = byId("connection");
  badge.textContent = label;
  badge.className = `state ${connected ? "state-ok" : "state-bad"}`;
  byId("device-state").textContent = connected ? "Live receiver" : "HTTP unavailable";
}

async function readJson(path, timeoutMs = 3500) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(path, { cache: "no-store", signal: controller.signal });
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.error || `HTTP ${response.status}`);
    return payload;
  } finally {
    clearTimeout(timeout);
  }
}

async function requestAction(path, method, body = null) {
  if (state.busy || state.otaRebooting) return null;
  cancelPolling();
  setBusy(true);
  try {
    const options = { method, cache: "no-store", headers: {} };
    if (body !== null) {
      options.headers["Content-Type"] = "application/x-www-form-urlencoded";
      options.body = body;
    }
    const response = await fetch(path, options);
    const payload = await response.json();
    if (!response.ok || !payload.ok) throw new Error(payload.error || `HTTP ${response.status}`);
    showNotice("Action completed", "success");
    return payload;
  } catch (error) {
    showNotice(error.message || "Request failed", "error");
    return null;
  } finally {
    setBusy(false);
    schedulePoll(0);
  }
}

function schedulePoll(delay = state.backoff) {
  clearTimeout(state.pollTimer);
  if (!document.hidden && !state.busy && !state.pollController) {
    state.pollTimer = setTimeout(pollLive, delay);
  }
}

async function pollLive() {
  if (document.hidden || state.busy || state.pollController) return;
  const generation = state.pollGeneration;
  const controller = new AbortController();
  state.pollController = controller;
  const timeout = setTimeout(() => controller.abort(), 3500);
  try {
    const response = await fetch("/api/live", { cache: "no-store", signal: controller.signal });
    const live = await response.json();
    if (!response.ok) throw new Error(live.error || `HTTP ${response.status}`);
    if (generation !== state.pollGeneration) return;
    state.backoff = 1000;
    setConnection(true, "Connected");
    if (state.otaRebooting) {
      state.otaRebooting = false;
      setBusy(false);
      byId("ota-progress").hidden = true;
      refreshOta();
      showNotice("Firmware reboot complete", "success");
    }
    renderLive(live);
  } catch (error) {
    if (generation === state.pollGeneration && error.name !== "AbortError") {
      state.backoff = Math.min(state.backoff * 2, 5000);
      setConnection(false, "Disconnected");
    }
  } finally {
    clearTimeout(timeout);
    if (state.pollController === controller) state.pollController = null;
    if (generation === state.pollGeneration) schedulePoll();
  }
}

function stateClass(value) {
  return value === "active" || value === "completed" || value === "idle"
    ? "value-ok"
    : value === "paused" || value === "recovering" || value === "armed" || value === "cancelled"
      ? "value-warn"
      : "value-bad";
}

function renderDetails(target, entries) {
  const fragment = document.createDocumentFragment();
  entries.forEach(([label, value]) => {
    fragment.append(element("dt", "", label), element("dd", "", value));
  });
  target.replaceChildren(fragment);
}

function frameSummary(frame) {
  if (!frame) return "No frame";
  if (frame.error) return frame.error;
  return frame.encoding === "decoded"
    ? `${frame.code} / ${frame.bits} bit / protocol ${frame.protocol}`
    : `${frame.pulses} pulses / ${frame.fingerprint}`;
}

function frameMatch(frame) {
  if (!frame) return "None";
  if (frame.match === "unique") return frame.match_name;
  if (frame.match === "ambiguous") return `Ambiguous (${frame.match_count})`;
  return frame.match === "unavailable" ? "Catalog unavailable" : "None";
}

function clearRulePulse() {
  clearTimeout(state.rulePulseTimer);
  if (state.rulePulseItem) state.rulePulseItem.classList.remove("rule-triggered");
  state.rulePulseTimer = null;
  state.rulePulseItem = null;
}

function pulseTriggeredRule(trigger) {
  const rulesView = document.querySelector('[data-panel="rules"]');
  if (!trigger || rulesView.hidden) return;
  const item = [...byId("rule-list").querySelectorAll("[data-trigger]")]
    .find((candidate) => candidate.dataset.trigger === trigger);
  if (!item) return;
  clearRulePulse();
  void item.offsetWidth;
  item.classList.add("rule-triggered");
  state.rulePulseItem = item;
  state.rulePulseTimer = setTimeout(() => {
    if (state.rulePulseItem === item) clearRulePulse();
  }, 1000);
}

function removeStoredActivity() {
  try { sessionStorage.removeItem(activityStorageKey); } catch (_) { /* Storage is optional. */ }
}

function validStoredActivityEntry(entry) {
  return entry && Number.isSafeInteger(entry.at_ms) && entry.at_ms >= 0 &&
    entry.at_ms <= maximumDateMilliseconds && Number.isSafeInteger(entry.delta) &&
    entry.delta > 0 && entry.frame && typeof entry.frame === "object" &&
    !Array.isArray(entry.frame);
}

function restoreActivity() {
  try {
    const serialized = sessionStorage.getItem(activityStorageKey);
    if (serialized === null) return;
    if (serialized.length > maximumActivityStorageLength) throw new Error("Activity data is too large");
    const stored = JSON.parse(serialized);
    if (!stored || stored.version !== activityStorageVersion ||
        !Number.isSafeInteger(stored.accepted) || stored.accepted < 0 ||
        !Array.isArray(stored.entries) || stored.entries.length > maximumActivityEntries ||
        !stored.entries.every(validStoredActivityEntry)) {
      throw new Error("Activity data is invalid");
    }
    state.accepted = stored.accepted;
    state.activity = stored.entries;
  } catch (_) {
    removeStoredActivity();
  }
}

function persistActivity() {
  try {
    const serialized = JSON.stringify({
      version: activityStorageVersion,
      accepted: state.accepted,
      entries: state.activity,
    });
    if (serialized.length > maximumActivityStorageLength) {
      removeStoredActivity();
      return;
    }
    sessionStorage.setItem(activityStorageKey, serialized);
  } catch (_) {
    // Activity remains available in memory when browser storage is unavailable.
  }
}

function addActivity(live, delta) {
  if (live.last) {
    state.activity.unshift({ at_ms: Date.now(), frame: live.last, delta });
    if (state.activity.length > maximumActivityEntries) {
      state.activity.length = maximumActivityEntries;
    }
    renderActivity();
  }
  persistActivity();
}

function renderActivity() {
  const target = byId("activity-list");
  if (state.activity.length === 0) {
    const row = element("tr");
    const cell = element("td", "empty", "No new frames this session.");
    cell.colSpan = 3;
    row.append(cell);
    target.replaceChildren(row);
    return;
  }
  const fragment = document.createDocumentFragment();
  state.activity.forEach((entry) => {
    const row = element("tr");
    const received = entry.delta > 1
      ? `${new Date(entry.at_ms).toLocaleTimeString()} (+${entry.delta}, latest shown)`
      : new Date(entry.at_ms).toLocaleTimeString();
    row.append(element("td", "", received), element("td", "", frameSummary(entry.frame)),
               element("td", "", frameMatch(entry.frame)));
    fragment.append(row);
  });
  target.replaceChildren(fragment);
}

function renderLast(frame) {
  const replay = byId("replay-last");
  replay.disabled = state.busy || !frame || Boolean(frame.error);
  if (!frame || frame.error) {
    byId("last-age").textContent = frame?.error || "No frame captured";
    renderDetails(byId("last-frame"), [["State", frame?.error || "No frame captured"]]);
    return;
  }
  byId("last-age").textContent = `Captured at ${Math.max(0, Math.floor(frame.captured_us / 1000))} ms uptime`;
  const details = frame.encoding === "decoded"
    ? [["Encoding", "Decoded"], ["Code", frame.code], ["Bits", frame.bits],
       ["Protocol", frame.protocol], ["Pulse", `${frame.pulse_us} us`],
       ["Observed repeats", frame.repeats], ["Learned match", frameMatch(frame)]]
    : [["Encoding", "Raw"], ["Pulses", frame.pulses], ["Start level", frame.start_level],
       ["Fingerprint", frame.fingerprint], ["Observed repeats", frame.repeats],
       ["Learned match", frameMatch(frame)]];
  renderDetails(byId("last-frame"), details);
}

function renderLearning(learning) {
  const label = byId("learning-state");
  const name = learning.name ? `: ${learning.name}` : "";
  label.textContent = `${learning.state.replace("_", " ")}${name}`;
  label.className = `state ${stateClass(learning.state)}`;
  const armed = learning.state === "armed";
  byId("cancel-learning").hidden = !armed;
  byId("learn-name").disabled = state.busy || armed;
  document.querySelector("#learn-form button[type=submit]").disabled = state.busy || armed;
}

function renderLive(live) {
  if (!live) return;
  const previousAccepted = state.accepted;
  const previousRuleExecutions = state.ruleExecutions;
  const ruleExecutions = live.automation.actions + live.automation.tx_errors;
  state.live = live;
  state.accepted = live.radio.accepted;
  state.ruleExecutions = ruleExecutions;
  if (previousAccepted !== null && live.radio.accepted > previousAccepted) {
    addActivity(live, live.radio.accepted - previousAccepted);
  } else if (live.radio.accepted !== previousAccepted) {
    persistActivity();
  }
  if (previousRuleExecutions !== null && ruleExecutions > previousRuleExecutions) {
    pulseTriggeredRule(live.automation.last_trigger);
  }
  if (state.learningRevision !== null && live.learning.revision !== state.learningRevision &&
      live.learning.state !== "armed") refreshSignals();
  state.learningRevision = live.learning.revision;

  byId("radio-state").textContent = live.radio.running ? "Running" : "Faulted";
  byId("radio-state").className = live.radio.running ? "value-ok" : "value-bad";
  byId("rx-state").textContent = live.radio.rx[0].toUpperCase() + live.radio.rx.slice(1);
  byId("rx-state").className = stateClass(live.radio.rx);
  byId("network-state").textContent = live.network.online ? `${live.network.rssi} dBm` : "Offline";
  byId("network-state").className = live.network.online ? "value-ok" : "value-bad";
  byId("accepted-count").textContent = live.radio.accepted;
  renderLast(live.last);
  renderLearning(live.learning);
  byId("automation-summary").textContent = `${live.automation.rules} rules / ${live.automation.actions} actions`;
  renderDetails(byId("runtime-details"), [
    ["Radio", live.radio.running ? "Running" : live.errors.radio],
    ["Receiver", live.radio.rx],
    ["RF queue drops", live.radio.queue_drops],
    ["Command timeouts", live.radio.timeouts],
    ["Network", live.network.online ? `${live.network.rssi} dBm` : live.errors.wifi],
  ]);
}

function signalMeta(signal) {
  if (signal.error) return signal.error;
  return signal.encoding === "decoded"
    ? `${signal.code} / ${signal.bits} bit / protocol ${signal.protocol} / ${signal.pulse_us} us`
    : `${signal.pulses} pulses / start ${signal.start_level}`;
}

function renderSignals() {
  byId("signal-count").textContent = `${state.signals.length} learned`;
  const target = byId("signal-list");
  if (state.signals.length === 0) {
    target.replaceChildren(element("p", "empty", "No learned signals."));
    updateRuleSelectors();
    return;
  }
  const fragment = document.createDocumentFragment();
  state.signals.forEach((signal) => {
    const item = element("div", "item");
    item.dataset.name = signal.name;
    item.append(element("div", "item-title", signal.name), element("div", "item-meta", signalMeta(signal)));
    const actions = element("div", "item-actions");
    const repeats = document.createElement("input");
    repeats.type = "number";
    repeats.min = "1";
    repeats.max = "20";
    repeats.value = "8";
    repeats.setAttribute("aria-label", `Repeats for ${signal.name}`);
    const replay = element("button", "primary", "Replay");
    replay.type = "button";
    replay.dataset.action = "replay";
    const remove = element("button", "danger", "Delete");
    remove.type = "button";
    remove.dataset.action = "delete";
    actions.append(repeats, replay, remove);
    item.append(actions);
    fragment.append(item);
  });
  target.replaceChildren(fragment);
  updateRuleSelectors();
}

function updateRuleSelectors() {
  document.querySelectorAll("#rule-form select").forEach((select) => {
    const selected = select.value;
    select.replaceChildren();
    state.signals.forEach((signal) => {
      const option = element("option", "", signal.name);
      option.value = signal.name;
      select.append(option);
    });
    if (state.signals.some((signal) => signal.name === selected)) select.value = selected;
  });
  document.querySelector("#rule-form button").disabled = state.busy || state.signals.length < 2;
}

async function refreshSignals() {
  try {
    const payload = await readJson("/api/signals");
    state.signals = payload.signals || [];
    renderSignals();
  } catch (error) {
    showNotice(error.message || "Could not load signals", "error");
  }
}

function renderRules(payload) {
  state.rules = payload.rules || [];
  byId("automation-enabled").checked = Boolean(payload.enabled);
  if (["off", "actions", "verbose"].includes(payload.log_mode)) byId("automation-log").value = payload.log_mode;
  const target = byId("rule-list");
  if (state.rules.length === 0) {
    target.replaceChildren(element("p", "empty", "No automation rules."));
    return;
  }
  const fragment = document.createDocumentFragment();
  state.rules.forEach((rule) => {
    const item = element("div", "item");
    item.dataset.trigger = rule.trigger;
    item.append(element("div", "item-title", `${rule.trigger} -> ${rule.target}`),
                element("div", "item-meta", `Replay ${rule.repeats} / cooldown ${rule.cooldown_ms} ms${rule.valid ? "" : ` / ${rule.error}`}`));
    const actions = element("div", "item-actions");
    const remove = element("button", "danger", "Remove");
    remove.type = "button";
    remove.dataset.action = "remove-rule";
    actions.append(remove);
    item.append(actions);
    fragment.append(item);
  });
  target.replaceChildren(fragment);
}

async function refreshRules() {
  try {
    renderRules(await readJson("/api/rules"));
  } catch (error) {
    showNotice(error.message || "Could not load rules", "error");
  }
}

async function refreshOta() {
  try {
    const ota = await readJson("/api/v1/ota/status");
    byId("ota-state").textContent = `${ota.state} / ${ota.running_version || "unknown"}`;
    renderDetails(byId("ota-details"), [
      ["State", ota.state], ["Running partition", ota.running_partition],
      ["Update partition", ota.update_partition], ["Running version", ota.running_version],
      ["Candidate", ota.candidate_version || "None"],
      ["Rollback", ota.rollback_possible ? "Available" : "Unavailable"],
      ["Last error", ota.error],
    ]);
  } catch (error) {
    byId("ota-state").textContent = error.message || "Unavailable";
  }
}

function activateView(name) {
  document.querySelectorAll(".tab").forEach((tab) => tab.classList.toggle("active", tab.dataset.view === name));
  document.querySelectorAll(".view").forEach((panel) => {
    const active = panel.dataset.panel === name;
    panel.hidden = !active;
    panel.classList.toggle("active", active);
  });
  if (name === "signals") refreshSignals();
  if (name === "rules") refreshRules();
  if (name === "system") refreshOta();
}

function bindActions() {
  document.querySelector(".tabs-inner").addEventListener("click", (event) => {
    const tab = event.target.closest("[data-view]");
    if (tab) activateView(tab.dataset.view);
  });

  byId("learn-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const name = byId("learn-name").value.trim();
    if (await requestAction("/api/learn", "POST", formBody({ name }))) byId("learn-name").value = "";
  });
  byId("cancel-learning").addEventListener("click", () => requestAction("/api/learn", "DELETE"));
  byId("replay-last").addEventListener("click", () => requestAction("/api/replay", "POST", formBody({ name: "", repeats: byId("last-repeats").value })));
  byId("clear-activity").addEventListener("click", () => {
    state.activity = [];
    removeStoredActivity();
    renderActivity();
  });
  byId("refresh-signals").addEventListener("click", refreshSignals);
  byId("refresh-rules").addEventListener("click", refreshRules);
  byId("refresh-ota").addEventListener("click", refreshOta);

  byId("signal-list").addEventListener("click", async (event) => {
    const button = event.target.closest("[data-action]");
    const item = event.target.closest("[data-name]");
    if (!button || !item) return;
    const name = item.dataset.name;
    if (button.dataset.action === "replay") {
      const repeats = item.querySelector("input").value;
      await requestAction("/api/replay", "POST", formBody({ name, repeats }));
    } else if (button.dataset.action === "delete" && confirm(`Delete learned signal ${name}?`)) {
      if (await requestAction("/api/signals", "DELETE", formBody({ name }))) {
        await Promise.all([refreshSignals(), refreshRules()]);
      }
    }
  });

  byId("decoded-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const data = new FormData(event.currentTarget);
    await requestAction("/api/transmit/decoded", "POST", formBody(Object.fromEntries(data)));
  });
  byId("raw-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const data = Object.fromEntries(new FormData(event.currentTarget));
    data.durations = data.durations.trim().split(/[\s,]+/).filter(Boolean).join(",");
    await requestAction("/api/transmit/raw", "POST", formBody(data));
  });
  byId("rule-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const data = Object.fromEntries(new FormData(event.currentTarget));
    if (await requestAction("/api/rules", "POST", formBody(data))) await refreshRules();
  });
  byId("rule-list").addEventListener("click", async (event) => {
    const button = event.target.closest("[data-action=remove-rule]");
    const item = event.target.closest("[data-trigger]");
    if (button && item && confirm(`Remove rule triggered by ${item.dataset.trigger}?`)) {
      if (await requestAction("/api/rules", "DELETE", formBody({ name: item.dataset.trigger }))) await refreshRules();
    }
  });
  byId("automation-enabled").addEventListener("change", async (event) => {
    await requestAction("/api/rules", "PATCH", formBody({ enabled: event.target.checked ? 1 : 0 }));
    await refreshRules();
  });
  byId("automation-log").addEventListener("change", async (event) => {
    await requestAction("/api/rules", "PATCH", formBody({ log_mode: event.target.value }));
    await refreshRules();
  });

  byId("ota-form").addEventListener("submit", uploadOta);
  document.addEventListener("visibilitychange", () => {
    cancelPolling();
    if (!document.hidden) schedulePoll(0);
  });
}

function uploadOta(event) {
  event.preventDefault();
  const file = byId("ota-file").files[0];
  if (!file || state.busy || !confirm(`Install ${file.name} and reboot RFBridge?`)) return;
  cancelPolling();
  setBusy(true);
  const progress = byId("ota-progress");
  progress.hidden = false;
  progress.value = 0;
  const request = new XMLHttpRequest();
  request.open("POST", "/api/v1/ota");
  request.setRequestHeader("Content-Type", "application/octet-stream");
  request.upload.addEventListener("progress", (upload) => {
    if (upload.lengthComputable) progress.value = Math.round((upload.loaded / upload.total) * 100);
  });
  request.addEventListener("load", () => {
    let payload = {};
    try { payload = JSON.parse(request.responseText); } catch (_) { payload = {}; }
    if (request.status === 200 && payload.ok) {
      progress.value = 100;
      showNotice("Firmware accepted; reconnecting after reboot", "success");
      setConnection(false, "Rebooting");
      state.otaRebooting = true;
      state.busy = false;
      setTimeout(() => schedulePoll(0), 3000);
    } else {
      showNotice(payload.error || `OTA failed: HTTP ${request.status}`, "error");
      progress.hidden = true;
      setBusy(false);
      schedulePoll(0);
    }
  });
  request.addEventListener("error", () => {
    showNotice("OTA transport failed", "error");
    progress.hidden = true;
    setBusy(false);
    schedulePoll(0);
  });
  request.send(file);
}

restoreActivity();
bindActions();
renderActivity();
Promise.all([refreshSignals(), refreshRules(), refreshOta()]).finally(() => schedulePoll(0));
