// shhterm browser client logic: protocol + end-to-end encryption + WebSocket.
//
// This mirrors the C++/terminal client exactly so browser and terminal users
// share rooms and can read each other's end-to-end-encrypted messages:
//   - key: routing half (sent to server) + secret half (kept in the browser)
//   - KDF: HKDF-SHA256(secret-string) -> 32-byte AES key
//   - cipher: AES-256-GCM, per message, wire = base64(nonce(12) || ct || tag(16))
// See proto/protocol.hpp and client/crypto.cpp for the authoritative scheme.

"use strict";

const PROTO_VERSION = 1;
const HKDF_INFO = "shhterm-e2e-v1";
const CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// ---- base32 (Crockford) ----------------------------------------------------

function randomSecret() {
  // 128 bits -> 26 Crockford base32 symbols, matching proto::generate_secret().
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  let out = "";
  let buffer = 0, bits = 0;
  for (const b of bytes) {
    buffer = (buffer << 8) | b;
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      out += CROCKFORD[(buffer >> bits) & 0x1f];
    }
  }
  if (bits > 0) out += CROCKFORD[(buffer << (5 - bits)) & 0x1f];
  return out; // 26 chars
}

// ---- key handling (mirror of proto normalize/compose/split) ----------------

function normalizeKey(input) {
  let out = "";
  for (let ch of input.toUpperCase()) {
    if (!/[A-Z0-9]/.test(ch)) continue;
    if (ch === "O") ch = "0";
    else if (ch === "I" || ch === "L") ch = "1";
    out += ch;
  }
  return out;
}

function group(norm) {
  let out = "";
  for (let i = 0; i < norm.length; i++) {
    if (i > 0 && i % 4 === 0) out += "-";
    out += norm[i];
  }
  return out;
}

function composeRoomKey(routing, secret) {
  return group(normalizeKey(routing) + normalizeKey(secret));
}

// Returns {routing, secret} or null.
function splitRoomKey(full) {
  const norm = normalizeKey(full);
  if (norm.length !== 52) return null;
  return { routing: norm.slice(0, 26), secret: norm.slice(26) };
}

// ---- base64 ----------------------------------------------------------------

function b64encode(bytes) {
  let bin = "";
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin);
}
function b64decode(str) {
  const bin = atob(str);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

// ---- crypto (Web Crypto; identical scheme to client/crypto.cpp) ------------

async function deriveKey(secretStr) {
  const enc = new TextEncoder();
  const material = await crypto.subtle.importKey(
    "raw", enc.encode(secretStr), "HKDF", false, ["deriveKey"]);
  return crypto.subtle.deriveKey(
    {
      name: "HKDF",
      hash: "SHA-256",
      salt: new Uint8Array(0),
      info: enc.encode(HKDF_INFO),
    },
    material,
    { name: "AES-GCM", length: 256 },
    false,
    ["encrypt", "decrypt"]);
}

async function encryptMessage(key, plaintext) {
  const iv = new Uint8Array(12);
  crypto.getRandomValues(iv);
  const ct = new Uint8Array(await crypto.subtle.encrypt(
    { name: "AES-GCM", iv }, key, new TextEncoder().encode(plaintext)));
  // wire = nonce(12) || ciphertext || tag(16)  (Web Crypto appends the tag)
  const packed = new Uint8Array(iv.length + ct.length);
  packed.set(iv, 0);
  packed.set(ct, iv.length);
  return b64encode(packed);
}

async function decryptMessage(key, blob) {
  try {
    const packed = b64decode(blob);
    if (packed.length < 12 + 16) return null;
    const iv = packed.slice(0, 12);
    const ctAndTag = packed.slice(12);
    const pt = await crypto.subtle.decrypt(
      { name: "AES-GCM", iv }, key, ctAndTag);
    return new TextDecoder().decode(pt);
  } catch (e) {
    return null; // wrong key / tampered / truncated
  }
}

// ---- client ----------------------------------------------------------------

// A small event-driven client the UI binds to. Usage:
//   const c = new shhtermClient();
//   c.on = (ev) => { ... };    // ev.type: status|welcome|chat|presence|nick|typing|users|ack|notice
//   c.connect({ create, key, name });
class shhtermClient {
  constructor() {
    this.ws = null;
    this.on = () => {};
    this.nextId = 1;
    this.pending = new Map();
    this.secret = null;
    this.key = null;        // AES CryptoKey
    this.routing = null;
    this.name = "";
    this.create = false;
    this._typingThrottle = 0;
  }

  async connect({ create, key, name }) {
    this.create = create;
    this.name = name;
    if (create) {
      this.secret = randomSecret();
    } else {
      const halves = splitRoomKey(key);
      if (!halves) {
        this.on({ type: "notice", text: "That room key looks invalid." });
        this.on({ type: "status", state: "failed", text: "invalid key" });
        return;
      }
      this.routing = halves.routing;
      this.secret = halves.secret;
    }
    this.key = await deriveKey(this.secret);

    // Same-origin WebSocket endpoint; the gateway bridges it to the server.
    const url = (location.protocol === "https:" ? "wss://" : "ws://") +
                location.host + "/ws";
    this.on({ type: "status", state: "connecting", text: "connecting…" });
    this.ws = new WebSocket(url);

    this.ws.onopen = () => {
      const hello = this.create
        ? { t: "create", name: this.name, ver: PROTO_VERSION }
        : { t: "join", key: this.routing, name: this.name, ver: PROTO_VERSION };
      this._send(hello);
    };
    this.ws.onmessage = (e) => this._onMessage(e.data);
    this.ws.onclose = () => this.on({ type: "status", state: "closed", text: "disconnected" });
    this.ws.onerror = () => this.on({ type: "notice", text: "connection error" });
  }

  _send(obj) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN)
      this.ws.send(JSON.stringify(obj));
  }

  async _onMessage(data) {
    let msg;
    try { msg = JSON.parse(data); } catch { return; }
    switch (msg.t) {
      case "welcome": {
        this.routing = msg.key;
        this.name = msg.name || this.name;
        this.on({
          type: "welcome",
          fullKey: composeRoomKey(msg.key, this.secret),
          name: this.name,
          users: msg.users || [],
        });
        this.on({ type: "status", state: "connected", text: "connected" });
        break;
      }
      case "msg": {
        const text = await decryptMessage(this.key, msg.enc || "");
        this.on({
          type: "chat",
          from: msg.from,
          text: text === null ? "[unable to decrypt]" : text,
          dm: !!msg.dm,
        });
        break;
      }
      case "ack":
        this.on({ type: "ack", id: msg.id });
        break;
      case "presence":
        this.on({ type: "presence", event: msg.event, name: msg.name });
        break;
      case "nick":
        if (msg.old === this.name) this.name = msg.name;
        this.on({ type: "nick", old: msg.old, name: msg.name });
        break;
      case "typing":
        this.on({ type: "typing", from: msg.from });
        break;
      case "users":
        this.on({ type: "users", users: msg.users || [] });
        break;
      case "error":
        this.on({ type: "notice", text: `error [${msg.code}]: ${msg.message}` });
        break;
    }
  }

  async sendChat(text, to) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return 0;
    const enc = await encryptMessage(this.key, text);
    const id = this.nextId++;
    const m = { t: "msg", id, enc };
    if (to) m.to = to;
    this._send(m);
    return id;
  }

  requestUsers() { this._send({ t: "users" }); }
  setNick(name) { this._send({ t: "nick", name }); }
  sendTyping() {
    const now = Date.now();
    if (now - this._typingThrottle < 1500) return;
    this._typingThrottle = now;
    this._send({ t: "typing" });
  }
  quit() { if (this.ws) this.ws.close(); }
}

// Expose for the page and for Node-based interop tests.
if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    randomSecret, normalizeKey, composeRoomKey, splitRoomKey,
    deriveKey, encryptMessage, decryptMessage, b64encode, b64decode,
    shhtermClient,
  };
}
