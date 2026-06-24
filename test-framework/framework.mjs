#!/usr/bin/env node
// ---------------------------------------------------------------------------
// QML Inspector Test Framework
//
// Reusable test framework for Qt/QML applications using the Qt Inspector.
// Import { test, run } and define your tests, then call run().
//
// Usage in a test file:
//
//   import { test, run } from "./path/to/framework.mjs";
//
//   test("my app: does something", async (app) => {
//     await app.click("my_button");
//     await app.expectTexts(["Expected Label"]);
//   });
//
//   run();
//
// Environment:
//   QML_INSPECTOR_HOST  (default: localhost)
//   QML_INSPECTOR_PORT  (default: 3768)
// ---------------------------------------------------------------------------

import net from "node:net";
import { spawn } from "node:child_process";

const HOST = process.env.QML_INSPECTOR_HOST || "localhost";
const PORT = parseInt(process.env.QML_INSPECTOR_PORT || "3768", 10);
const TIMEOUT_MS = 15000;

// ---------------------------------------------------------------------------
// Qt Inspector TCP bridge (newline-delimited JSON)
// ---------------------------------------------------------------------------
export class Inspector {
  constructor() {
    this.socket = null;
    this.requestId = 0;
    this.pending = new Map();
    this.buffer = "";
  }

  async connect() {
    if (this.socket && !this.socket.destroyed) return;
    return new Promise((resolve, reject) => {
      const sock = net.createConnection({ host: HOST, port: PORT });
      sock.once("connect", () => { this.socket = sock; resolve(); });
      sock.once("error", (err) => reject(new Error(`Cannot connect to inspector: ${err.message}`)));
      sock.on("data", (chunk) => { this.buffer += chunk.toString("utf-8"); this._drain(); });
      sock.on("close", () => {
        this.socket = null;
        for (const [, p] of this.pending) { clearTimeout(p.timer); p.reject(new Error("Connection closed")); }
        this.pending.clear();
      });
      sock.on("error", () => {});
    });
  }

  _drain() {
    let idx;
    while ((idx = this.buffer.indexOf("\n")) !== -1) {
      const line = this.buffer.slice(0, idx).trim();
      this.buffer = this.buffer.slice(idx + 1);
      if (!line) continue;
      try {
        const msg = JSON.parse(line);
        const p = this.pending.get(String(msg.id));
        if (p) { clearTimeout(p.timer); this.pending.delete(String(msg.id)); p.resolve(msg); }
      } catch {
        console.error("Error processing reply message:", line);
      }
    }
  }

  async send(command, params = {}) {
    await this.connect();
    const id = ++this.requestId;
    const payload = JSON.stringify({ id, command, params }) + "\n";
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(String(id)); reject(new Error(`Timeout: ${command}`)); }, TIMEOUT_MS);
      this.pending.set(String(id), { resolve, reject, timer });
      this.socket.write(payload);
    });
  }

  disconnect() {
    if (this.socket) this.socket.destroy();
  }
}

// ---------------------------------------------------------------------------
// Test helpers — high-level API for writing tests
// ---------------------------------------------------------------------------
export class App {
  constructor(inspector) {
    this.inspector = inspector;
  }

  /** Clicks an element by its text label. Performs case-insensitive,
   *  approximate (substring) matching; e.g. click("Storage") will
   *  match buttons with text "storage", "storage_ui", or
   *  "ui_storage_button" as labels.
   */
  async click(text, opts = {}) {
    const res = await this.inspector.send("findAndClick", { text, ...opts });
    if (res.error) throw new Error(`click("${text}"): ${res.error}`);
    return res;
  }

  /** Clicks an element by object ID. */
  async clickById(objectId) {
    const res = await this.inspector.send("click", { objectId });
    if (res.error) throw new Error(`clickById("${objectId}"): ${res.error}`);
    return res;
  }

  /** Finds a clickable object with matching property value, and clicks it.
   * Unlike {@link App#click}, this performs exact matches on property values.
   */
  async clickByProperty(prop, value) {
    const matches = await this.findByProperty(prop, value);
    if (matches.error) throw new Error(`clickByProperty: ${matches.error}`);

    for (const candidate of matches.matches) {
      try {
        const result = await this.clickById(candidate.id);
        result.matchedText = candidate.text;
        result.matchedType = candidate.type;
        result.matchedId = candidate.id;

        return result;
      } catch (e) {
        // Continue to next candidate.
        // FIXME we should do a better job at distinguishing exceptions
        //  because things are not clickable from exceptions because of
        //  other things (e.g. can't connect to inspector server).
      }
    }
    throw new Error(`clickByProperty: no matches for ${JSON.stringify(prop)}=${JSON.stringify(value)}`);
  }

  /** Take a screenshot. Returns { image (base64), width, height }. */
  async screenshot() {
    return this.inspector.send("screenshot", {});
  }

  /** Get the object tree. */
  async getTree(opts = {}) {
    return this.inspector.send("getTree", opts);
  }

  /** List all interactive elements. */
  async listInteractive() {
    return this.inspector.send("listInteractive", {});
  }

  /** List all file dialogs. */
  async listFileDialogs() {
    return this.inspector.send("listFileDialogs", {});
  }

  /** Interact with an existing dialog. */
  async fileDialogAction(objectId, action, path = undefined) {
    var params = { objectId, action };
    if (path !== undefined) {
      params.path = path;
    }
    return this.inspector.send("fileDialogAction", params);
  }

  /** Find elements by property value. */
  async findByProperty(property, value) {
    const params = { property };
    if (value !== undefined) params.value = value;
    return this.inspector.send("findByProperty", params);
  }

  /** Get properties of an object. */
  async getProperties(objectId) {
    return this.inspector.send("getProperties", { objectId });
  }

  async getProperty(objectId, name) {
    const props = await this.getProperties(objectId);
    if (props.error) throw new Error(`getProperties failed: ${props.error}`);
    const prop = props.properties.find((p) => p.name === name);
    if (!prop) throw new Error(`Property "${name}" not found on ${objectId}`);
    return prop.value;
  }

  /** Assert that elements with the given texts exist in the UI. */
  async expectTexts(texts) {
    const missing = [];
    for (const expected of texts) {
      const res = await this.inspector.send("findByProperty", { property: "text", value: expected });
      if (res.error || !res.matches || res.matches.length === 0) {
        missing.push(expected);
      }
    }

    if (missing.length > 0) {
      throw new Error(`Expected texts not found: ${JSON.stringify(missing)}`);
    }
  }

  /** Assert the status label contains the given text. */
  async expectStatus(expected) {
    const res = await this.findByProperty("text", expected);
    if (!res.matches || res.matches.length === 0) {
      throw new Error(`Expected status "${expected}" not found in UI`);
    }
  }

  /** Find an object by type and return a specific property value. */
  async getPropertyByType(typeName, propName) {
    const res = await this.inspector.send("findByType", { typeName });
    if (res.error || !res.matches || res.matches.length === 0) {
      throw new Error(`No object found with type "${typeName}"`);
    }
    return this.expectPropertyOnObject(res.matches[0].id, propName);
  }

  /** Assert a property value on an object found by type. */
  async expectProperty(typeName, propName, expected) {
    const res = await this.inspector.send("findByType", { typeName });
    if (res.error || !res.matches || res.matches.length === 0) {
      throw new Error(`No object found with type "${typeName}"`);
    }
    return await this.expectPropertyOnObject(res.matches[0].id, propName, expected);
  }

  /** Assert a property value on a specific object (by object ID). */
  async expectPropertyOnObject(objectId, propName, expected) {
    const prop = await this.getProperty(objectId, propName);
    if (prop !== expected) {
      throw new Error(
        `Expected ${objectId}.${propName} to be ${JSON.stringify(expected)}, got ${JSON.stringify(prop)}`
      );
    }
  }

  /** Wait for a condition (polls). */
  async waitFor(fn, { timeout = 5000, interval = 300, description = "condition" } = {}) {
    const start = Date.now();
    while (Date.now() - start < timeout) {
      try {
        return await fn();
      } catch {
        await new Promise((r) => setTimeout(r, interval));
      }
    }
    // One last try — let it throw
    await fn();
  }
}

// ---------------------------------------------------------------------------
// CI helpers
// ---------------------------------------------------------------------------
async function waitForInspector(maxRetries = 30, intervalMs = 500) {
  for (let i = 0; i < maxRetries; i++) {
    try {
      const sock = net.createConnection({ host: HOST, port: PORT });
      await new Promise((resolve, reject) => {
        sock.once("connect", () => { sock.destroy(); resolve(); });
        sock.once("error", reject);
      });
      return;
    } catch {
      await new Promise((r) => setTimeout(r, intervalMs));
    }
  }
  throw new Error(`Inspector not available at ${HOST}:${PORT} after ${maxRetries * intervalMs}ms`);
}

function launchOffscreen(appBin, verbose = false) {
  if (verbose) console.log(`Launching: ${appBin} -platform offscreen`);
  const child = spawn(appBin, ["-platform", "offscreen"], {
    stdio: ["ignore", "pipe", "pipe"],
    env: {
      ...process.env,
      QT_QPA_PLATFORM: "offscreen",
      QT_FORCE_STDERR_LOGGING: "1",
    },
  });

  return child;
}

function launchXvfb(appBin, verbose = false) {
  if (verbose) console.log(`Launching: ${appBin} with Xvfb`);

  const child = spawn(
    "xvfb-run",
    ["-a", appBin],
    {
      stdio: ["ignore", "pipe", "pipe"],
      env: {
        ...process.env,
        QT_QPA_PLATFORM: "xcb",
        QT_FORCE_STDERR_LOGGING: "1",
      },
    }
  );

  return child;
}

function launchApp(launchFn, appBin, verbose = false) {
  const child = launchFn(appBin, verbose);

  if (verbose) {
    child.stdout.on("data", (d) => process.stderr.write(`[app:out] ${d}`));
    child.stderr.on("data", (d) => process.stderr.write(`[app:err] ${d}`));
  } else {
    child.stdout.resume();
    child.stderr.resume();
  }

  child.on("exit", (code) => {
    if (code !== null && code !== 0) {
      console.error(`App exited with code ${code}`);
    }
  });

  return child;
}

// ---------------------------------------------------------------------------
// Test runner
// ---------------------------------------------------------------------------
const tests = [];

export function test(name, fn, opts = {}) {
  tests.push({ name, fn, skip: opts.skip || [] });
}

async function runTests(filter, appProcess, { mode = "normal" } = {}) {
  const inspector = new Inspector();
  const app = new App(inspector);

  try {
    await inspector.connect();
  } catch (err) {
    console.error(`\x1b[31m✗ Could not connect to Qt Inspector at ${HOST}:${PORT}\x1b[0m`);
    console.error(`  Make sure the app is running with the inspector enabled.`);
    if (appProcess) appProcess.kill();
    process.exit(1);
  }

  let toRun = filter
    ? tests.filter((t) => t.name.toLowerCase().includes(filter.toLowerCase()))
    : tests;

  // Skip tests that opt out of the current mode (e.g. skip: ["ci"])
  const skipped = toRun.filter((t) => t.skip.includes(mode));
  toRun = toRun.filter((t) => !t.skip.includes(mode));

  if (skipped.length > 0) {
    for (const t of skipped) {
      console.log(`  \x1b[33m○\x1b[0m ${t.name} (skipped in ${mode} mode)`);
    }
  }

  console.log(`\nRunning ${toRun.length} test(s)...\n`);

  let passed = 0;
  let failed = 0;

  for (const t of toRun) {
    try {
      await t.fn(app);
      console.log(`  \x1b[32m✓\x1b[0m ${t.name}`);
      passed++;
    } catch (err) {
      console.log(`  \x1b[31m✗\x1b[0m ${t.name}`);
      console.log(`    ${err.message}`);
      failed++;
    }
  }

  console.log(`\n${passed} passed, ${failed} failed\n`);
  inspector.disconnect();

  if (appProcess) {
    appProcess.kill();
    await new Promise((r) => setTimeout(r, 500));
  }

  process.exit(failed > 0 ? 1 : 0);
}

// ---------------------------------------------------------------------------
// run() — call this after registering tests
// ---------------------------------------------------------------------------
export async function run() {
  const rawArgs = process.argv.slice(2);
  const verbose = rawArgs.includes("--verbose") || rawArgs.includes("-v");

  // Strip flags to get positional args
  const args = rawArgs.filter(a => a !== "--verbose" && a !== "-v" && a !== "--ci" && a !== "--xvfb");
  const isCI = rawArgs.includes("--ci");

  if (isCI) {
    // CI mode: --ci <app-binary> [filter]
    const appBin = args[0];
    if (!appBin) {
      console.error("Usage: node <test-file> --ci <app-binary> [filter] [--verbose] [--xvfb]");
      process.exit(1);
    }
    const filter = args[1] || "";
    const useXvfb = rawArgs.includes("--xvfb");

    const appProcess = launchApp(useXvfb ? launchXvfb : launchOffscreen, appBin, verbose);

    if (verbose) console.log("Waiting for inspector to become available...");
    try {
      await waitForInspector();
    } catch (err) {
      console.error(err.message);
      appProcess.kill();
      process.exit(1);
    }
    if (verbose) console.log("Inspector connected.");

    // Give the app a moment to fully initialize plugins
    await new Promise((r) => setTimeout(r, 2000));

    const mode = useXvfb ? "xcb" : "offscreen";
    await runTests(filter, appProcess, { mode });
  } else {
    // Normal mode: app must already be running
    const filter = args[0] || "";
    const mode = process.env.QT_QPA_PLATFORM || "normal";
    await runTests(filter, null, { mode });
  }
}
