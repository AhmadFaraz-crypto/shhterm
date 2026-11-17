// Scriptable "browser" peer for interop testing: behaves like the web client
// (same shhterm.js crypto + protocol over wss through the gateway), but is
// driven over stdin/stdout so a test harness can coordinate it with a real
// C++ terminal client.
//
//   node browser_peer.mjs create        -> prints "KEY=<fullkey>" once in a room
//   node browser_peer.mjs join <key>
// stdin commands:  SEND <text>
// stdout events:   KEY=<fullkey>, READY, RECV <from>|<text>

import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const T = require("./shhterm.js");

const GW = process.env.GW || "wss://shhterm.duckdns.org/ws";
const mode = process.argv[2];           // "create" | "join"
const joinKey = process.argv[3] || "";
const name = process.env.PEER_NAME || "WebUser";

let aesKey = null, secret = null, routing = null;

if (mode === "create") {
  secret = T.randomSecret();
} else {
  const h = T.splitRoomKey(joinKey);
  if (!h) { console.error("bad key"); process.exit(2); }
  routing = h.routing; secret = h.secret;
}
aesKey = await T.deriveKey(secret);

const ws = new WebSocket(GW);
ws.onopen = () => {
  ws.send(JSON.stringify(mode === "create"
    ? { t: "create", name, ver: 1 }
    : { t: "join", key: routing, name, ver: 1 }));
};
ws.onmessage = async (e) => {
  const m = JSON.parse(e.data);
  if (m.t === "welcome") {
    const fullKey = T.composeRoomKey(m.key, secret);
    console.log("KEY=" + fullKey);
    console.log("READY");
  } else if (m.t === "msg") {
    const text = await T.decryptMessage(aesKey, m.enc);
    console.log("RECV " + m.from + "|" + text);
  }
};
ws.onclose = () => process.exit(0);
ws.onerror = (e) => { console.error("wserr", e.message || e); };

let idc = 1;
process.stdin.setEncoding("utf8");
let buf = "";
process.stdin.on("data", async (d) => {
  buf += d;
  let nl;
  while ((nl = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, nl); buf = buf.slice(nl + 1);
    if (line.startsWith("SEND ")) {
      const text = line.slice(5);
      ws.send(JSON.stringify({ t: "msg", id: idc++, enc: await T.encryptMessage(aesKey, text) }));
    } else if (line === "QUIT") {
      ws.close();
    }
  }
});
