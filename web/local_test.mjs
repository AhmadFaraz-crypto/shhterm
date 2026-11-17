// Local end-to-end test of the web path:
//   C++ server (raw TLS)  <--  Node gateway (wss)  <--  two browser clients
// using the REAL web/shhterm.js crypto + protocol code and the global
// WebSocket (Node 22), to prove encrypted messages flow through the gateway.
//
// Assumes the C++ server and the gateway are already running (see run script).

import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const T = require("./shhterm.js");

const GW = process.env.GW || "wss://127.0.0.1:9443/ws";

function mkClient(label) {
  return new Promise((resolve) => {
    const ws = new WebSocket(GW);
    const c = { ws, label, key: null, inbox: [], onmsg: null };
    ws.onmessage = async (e) => {
      const m = JSON.parse(e.data);
      if (m.t === "welcome") { c.welcome = m; if (c._wres) c._wres(m); }
      else if (m.t === "msg") {
        const text = await T.decryptMessage(c.key, m.enc);
        c.inbox.push({ from: m.from, text });
        if (c.onmsg) c.onmsg();
      } else if (m.t === "error") { c.inbox.push({ error: m }); }
    };
    ws.onopen = () => resolve(c);
    ws.onerror = (e) => { console.error(label, "ws error", e.message || e); };
  });
}
const send = (c, obj) => c.ws.send(JSON.stringify(obj));
const waitWelcome = (c) => new Promise((r) => { c._wres = r; });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  let ok = true;

  // Browser A creates a room.
  const secretA = T.randomSecret();
  const a = await mkClient("A");
  a.key = await T.deriveKey(secretA);
  send(a, { t: "create", name: "WebAhmad", ver: 1 });
  const wA = await waitWelcome(a);
  const fullKey = T.composeRoomKey(wA.key, secretA);
  console.log("A created room, full key:", fullKey.slice(0, 20) + "…");

  // Browser B joins with the shared full key.
  const halves = T.splitRoomKey(fullKey);
  const b = await mkClient("B");
  b.key = await T.deriveKey(halves.secret);
  send(b, { t: "join", key: halves.routing, name: "WebAli", ver: 1 });
  await waitWelcome(b);
  console.log("B joined room");

  // A -> B.
  const gotB = new Promise((r) => { b.onmsg = r; });
  send(a, { t: "msg", id: 1, enc: await T.encryptMessage(a.key, "hi from the web") });
  await Promise.race([gotB, sleep(3000)]);
  const b_ok = b.inbox.some((m) => m.from === "WebAhmad" && m.text === "hi from the web");
  console.log("B received+decrypted A's message:", b_ok);
  ok = ok && b_ok;

  // B -> A.
  const gotA = new Promise((r) => { a.onmsg = r; });
  send(b, { t: "msg", id: 1, enc: await T.encryptMessage(b.key, "reply over wss") });
  await Promise.race([gotA, sleep(3000)]);
  const a_ok = a.inbox.some((m) => m.from === "WebAli" && m.text === "reply over wss");
  console.log("A received+decrypted B's message:", a_ok);
  ok = ok && a_ok;

  a.ws.close(); b.ws.close();
  console.log(ok ? "WEB E2E OK" : "WEB E2E FAILED");
  process.exit(ok ? 0 : 1);
}
main().catch((e) => { console.error(e); process.exit(1); });
