#!/usr/bin/env node
// A click's reply must come after the click has landed. Otherwise the client's next command can
// run first (it did on Windows) and find the Popup the click opens still closed.
//
//   node tests/click-order.mjs --ci <path to inspector-test-app>
import net from "node:net";
import { test, run } from "../test-framework/framework.mjs";

const HOST = process.env.QML_INSPECTOR_HOST || "localhost";
const PORT = parseInt(process.env.QML_INSPECTOR_PORT || "3768", 10);
const ROUNDS = 50;

async function idOf(app, objectName) {
  const res = await app.findByProperty("objectName", objectName);
  if (!res.matches?.length) throw new Error(`${objectName} not found`);
  return res.matches[0].id;
}

async function evaluate(app, expression) {
  const res = await app.inspector.send("evaluate", { expression });
  if (res.error) throw new Error(`evaluate ${expression}: ${res.error}`);
  return res.result;
}

// Writes every command in one go on a new connection; resolves with the replies in arrival order.
function sendAtOnce(commands) {
  return new Promise((resolve, reject) => {
    const sock = net.createConnection({ host: HOST, port: PORT });
    const replies = [];
    let buffer = "";
    const timer = setTimeout(() => {
      sock.destroy();
      reject(new Error(`timed out with ${replies.length} of ${commands.length} replies`));
    }, 15000);
    sock.on("connect", () => sock.write(commands.map((c) => JSON.stringify(c) + "\n").join("")));
    sock.on("data", (chunk) => {
      buffer += chunk.toString("utf-8");
      let idx;
      while ((idx = buffer.indexOf("\n")) !== -1) {
        replies.push(JSON.parse(buffer.slice(0, idx)));
        buffer = buffer.slice(idx + 1);
      }
      if (replies.length === commands.length) {
        clearTimeout(timer);
        sock.destroy();
        resolve(replies);
      }
    });
    sock.on("error", (err) => {
      clearTimeout(timer);
      reject(err);
    });
  });
}

test("click: the next click reaches the Popup the last one opened", async (app) => {
  const opener = await idOf(app, "opener");
  const inner = await idOf(app, "innerButton");
  const before = await evaluate(app, "innerClicks");
  for (let round = 1; round <= ROUNDS; round++) {
    for (const [objectId, name] of [[opener, "opener"], [inner, "innerButton"]]) {
      const res = await app.inspector.send("click", { objectId });
      if (res.error) throw new Error(`round ${round}, click ${name}: ${res.error}`);
    }
  }
  const clicks = (await evaluate(app, "innerClicks")) - before;
  if (clicks !== ROUNDS) throw new Error(`innerButton registered ${clicks} of ${ROUNDS} clicks`);
});

test("click: commands sent with it wait until it has landed", async (app) => {
  const opener = await idOf(app, "opener");
  const inner = await idOf(app, "innerButton");
  const field = await idOf(app, "field");
  const before = await evaluate(app, "innerClicks");
  const replies = await sendAtOnce([
    { id: 1, command: "click", params: { objectId: opener } },
    { id: 2, command: "click", params: { objectId: inner } }, // clears the field
    { id: 3, command: "setProperty", params: { objectId: field, property: "text", value: "typed" } },
  ]);
  const order = replies.map((r) => r.id).join(",");
  if (order !== "1,2,3") throw new Error(`replies came back as ${order}`);
  for (const r of replies) {
    if (r.error) throw new Error(`command ${r.id}: ${r.error}`);
  }
  const clicks = (await evaluate(app, "innerClicks")) - before;
  if (clicks !== 1) throw new Error(`innerButton registered ${clicks} of 1 clicks`);
  const text = await evaluate(app, "field.text");
  if (text !== "typed") throw new Error(`field text is ${JSON.stringify(text)}: the click landed after setProperty`);
});

// As with a click that opens a modal dialog: the reply must not wait for the handler's nested
// event loop, and clicks inside that loop must still land before their replies.
test("click: a handler's nested event loop does not hold the reply", async (app) => {
  const nester = await idOf(app, "nester");
  const opener = await idOf(app, "opener");
  const inner = await idOf(app, "innerButton");
  const before = await evaluate(app, "innerClicks");
  const res = await app.inspector.send("click", { objectId: nester });
  if (res.error) throw new Error(`click nester: ${res.error}`);
  if (!(await evaluate(app, "nestedLoop.running"))) throw new Error("the nested loop is not running");
  for (const [objectId, name] of [[opener, "opener"], [inner, "innerButton"]]) {
    const r = await app.inspector.send("click", { objectId });
    if (r.error) throw new Error(`click ${name} in the nested loop: ${r.error}`);
  }
  const clicks = (await evaluate(app, "innerClicks")) - before;
  if (clicks !== 1) throw new Error(`innerButton registered ${clicks} of 1 clicks in the nested loop`);
  await evaluate(app, "nestedLoop.quit()");
  await app.waitFor(async () => {
    if (await evaluate(app, "nestedLoop.running")) throw new Error("the nested loop did not end");
  });
});

run();
