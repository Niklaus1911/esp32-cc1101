#!/usr/bin/env node
// Full-page screenshots of every Web UI view against tools/web-mock.mjs, for before/after
// visual review. Drives a local headless Chromium over the DevTools protocol; no npm
// dependencies. Screenshots are review artifacts: keep them out of Git (for example under
// the ignored .local/ directory).
//
//   node tools/web-screenshots.mjs <output-dir> [--scenario healthy] [--scheme dark|light]
//                                  [--widths 1280,768,375] [--chromium /usr/bin/chromium]

import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const VIEWS = ["overview", "signals", "transmit", "rules", "system"];
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function argument(name, fallback) {
  const index = process.argv.indexOf(name);
  return index > 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
}

async function waitForFile(path, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    try {
      return readFileSync(path, "utf8");
    } catch (error) {
      if (Date.now() > deadline) throw error;
      await sleep(100);
    }
  }
}

class Page {
  constructor(socket) {
    this.socket = socket;
    this.nextId = 0;
    this.pending = new Map();
    this.problems = [];
    socket.addEventListener("message", (event) => {
      const message = JSON.parse(event.data);
      if (message.id && this.pending.has(message.id)) {
        const { resolve, reject } = this.pending.get(message.id);
        this.pending.delete(message.id);
        if (message.error) reject(new Error(message.error.message));
        else resolve(message.result);
      } else if (message.method === "Runtime.exceptionThrown") {
        this.problems.push(`exception: ${message.params.exceptionDetails.exception?.description || message.params.exceptionDetails.text}`);
      } else if (message.method === "Runtime.consoleAPICalled" && message.params.type === "error") {
        this.problems.push(`console.error: ${message.params.args.map((arg) => arg.value ?? arg.description).join(" ")}`);
      }
    });
  }

  send(method, params = {}) {
    const id = ++this.nextId;
    this.socket.send(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
  }
}

async function main() {
  const output = process.argv[2];
  assert(output && !output.startsWith("--"), "usage: web-screenshots.mjs <output-dir> [options]");
  const scenario = argument("--scenario", "healthy");
  const scheme = argument("--scheme", "dark");
  const widths = argument("--widths", "1280,768,375").split(",").map(Number);
  const chromium = argument("--chromium", process.env.CHROMIUM || "chromium");
  mkdirSync(output, { recursive: true });

  const toolsDir = dirname(fileURLToPath(import.meta.url));
  const { createMockServer } = await import(pathToFileURL(join(toolsDir, "web-mock.mjs")).href);
  const { server } = createMockServer({ scenario });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const base = `http://127.0.0.1:${server.address().port}/`;

  const profile = mkdtempSync(join(tmpdir(), "rfbridge-shots-"));
  const browser = spawn(chromium, ["--headless=new", "--remote-debugging-port=0", `--user-data-dir=${profile}`,
                                   "--no-first-run", "--no-default-browser-check", "--hide-scrollbars",
                                   "--force-color-profile=srgb", "about:blank"], { stdio: "ignore" });
  try {
    const port = (await waitForFile(join(profile, "DevToolsActivePort"), 15000)).split("\n")[0];
    const target = await (await fetch(`http://127.0.0.1:${port}/json/new?about:blank`, { method: "PUT" })).json();
    const socket = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
    const page = new Page(socket);
    await page.send("Page.enable");
    await page.send("Runtime.enable");
    await page.send("Emulation.setEmulatedMedia", { features: [
      { name: "prefers-color-scheme", value: scheme }, { name: "prefers-reduced-motion", value: "reduce" },
    ] });

    for (const width of widths) {
      await page.send("Emulation.setDeviceMetricsOverride",
                      { width, height: 900, deviceScaleFactor: 1, mobile: width < 768 });
      await page.send("Page.navigate", { url: base });
      await sleep(2000);
      for (const view of VIEWS) {
        await page.send("Runtime.evaluate", { expression: `activateView(${JSON.stringify(view)})` });
        await sleep(900);
        const metrics = await page.send("Page.getLayoutMetrics");
        const overflow = (await page.send("Runtime.evaluate", {
          expression: "document.documentElement.scrollWidth > document.documentElement.clientWidth",
          returnByValue: true,
        })).result.value;
        if (overflow) page.problems.push(`horizontal overflow: ${view} at ${width}px`);
        const height = Math.ceil(metrics.cssContentSize.height);
        const shot = await page.send("Page.captureScreenshot", {
          format: "png", captureBeyondViewport: true, clip: { x: 0, y: 0, width, height, scale: 1 },
        });
        const file = join(output, `${scenario}-${scheme}-${view}-${width}.png`);
        writeFileSync(file, Buffer.from(shot.data, "base64"));
        console.log(`${file} (${width}x${height})`);
      }
    }
    socket.close();
    if (page.problems.length) {
      console.log(`Problems:\n  ${page.problems.join("\n  ")}`);
      process.exitCode = 1;
    } else {
      console.log("No page errors or horizontal overflow.");
    }
  } finally {
    browser.kill();
    server.closeAllConnections();
    server.close();
    await sleep(300);
    rmSync(profile, { recursive: true, force: true });
  }
}

await main();
