// shhterm web gateway.
//
// Serves the static web client over HTTPS and bridges each browser WebSocket to
// the existing C++ server (raw TLS, length-prefixed JSON) at SHHTERM_UPSTREAM.
//
// The browser speaks the shhterm JSON protocol directly and does its own E2E
// encryption; this gateway only translates transports:
//   browser WS text message  <->  4-byte length-prefixed JSON frame
//
// No application logic, no decryption — it never sees plaintext message bodies.
//
// Env vars:
//   SHHTERM_TLS_CERT   path to the HTTPS/wss certificate (fullchain.pem)
//   SHHTERM_TLS_KEY    path to the private key (privkey.pem)
//   SHHTERM_PORT       listen port (default 443)
//   SHHTERM_UPSTREAM   host:port of the C++ server (default 127.0.0.1:8443)
//   SHHTERM_UPSTREAM_CA path to the C++ server's cert, if self-signed
//   SHHTERM_WEBROOT    directory of static files (default ./ next to this file)

"use strict";

const https = require("node:https");
const tls = require("node:tls");
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");

const PORT = parseInt(process.env.SHHTERM_PORT || "443", 10);
const [UP_HOST, UP_PORT] = (process.env.SHHTERM_UPSTREAM || "127.0.0.1:8443").split(":");
const WEBROOT = process.env.SHHTERM_WEBROOT || __dirname;
const MAX_FRAME = 64 * 1024; // mirror proto::kMaxFrameSize

const tlsOptions = {
  cert: fs.readFileSync(process.env.SHHTERM_TLS_CERT),
  key: fs.readFileSync(process.env.SHHTERM_TLS_KEY),
};
const upstreamCA = process.env.SHHTERM_UPSTREAM_CA
  ? fs.readFileSync(process.env.SHHTERM_UPSTREAM_CA) : undefined;

// ---- static file serving ---------------------------------------------------

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".ico": "image/x-icon",
};
function serveStatic(req, res) {
  let urlPath;
  try {
    urlPath = decodeURIComponent(req.url.split("?")[0]);
  } catch {
    // Malformed percent-encoding (common from internet scanners). Don't crash.
    res.writeHead(400).end("bad request");
    return;
  }
  // /room/<key> is a client-side route -> serve the app shell. Only rewrite
  // extensionless paths so real asset requests (e.g. /room/app.js from a
  // relative <script>) are never served the HTML shell.
  if (urlPath === "/" ||
      (urlPath.startsWith("/room/") && !path.extname(urlPath))) {
    urlPath = "/index.html";
  }
  // Prevent path traversal.
  const file = path.normalize(path.join(WEBROOT, urlPath));
  if (!file.startsWith(WEBROOT)) { res.writeHead(403).end(); return; }
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404).end("not found"); return; }
    res.writeHead(200, {
      "Content-Type": MIME[path.extname(file)] || "application/octet-stream",
      "Cache-Control": "no-cache",
    });
    res.end(data);
  });
}

const server = https.createServer(tlsOptions, serveStatic);

// ---- minimal WebSocket (RFC 6455) server upgrade ---------------------------
// Implemented inline to avoid any external npm dependency.

const WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

server.on("upgrade", (req, socket) => {
  if (req.url.split("?")[0] !== "/ws") { socket.destroy(); return; }
  const key = req.headers["sec-websocket-key"];
  if (!key) { socket.destroy(); return; }
  const accept = crypto.createHash("sha1").update(key + WS_GUID).digest("base64");
  socket.write(
    "HTTP/1.1 101 Switching Protocols\r\n" +
    "Upgrade: websocket\r\n" +
    "Connection: Upgrade\r\n" +
    "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
  bridge(socket);
});

// Encode a text frame (server->client, unmasked).
function wsFrame(str) {
  const payload = Buffer.from(str, "utf8");
  const len = payload.length;
  let header;
  if (len < 126) {
    header = Buffer.from([0x81, len]);
  } else if (len < 65536) {
    header = Buffer.from([0x81, 126, (len >> 8) & 0xff, len & 0xff]);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x81; header[1] = 127;
    header.writeBigUInt64BE(BigInt(len), 2);
  }
  return Buffer.concat([header, payload]);
}

// Each connection: parse incoming WS frames, open a TLS socket to the C++
// server, and shuttle messages across with the right framing.
function bridge(browser) {
  let upstream = null;
  let closed = false;
  let inBuf = Buffer.alloc(0);          // browser -> us (WS frames)
  let upBuf = Buffer.alloc(0);          // upstream -> us (length-prefixed)

  const closeAll = () => {
    if (closed) return;
    closed = true;
    try { browser.destroy(); } catch {}
    try { if (upstream) upstream.destroy(); } catch {}
  };

  // Connect to the C++ server.
  upstream = tls.connect({
    host: UP_HOST, port: parseInt(UP_PORT, 10),
    ca: upstreamCA,
    // The upstream is on localhost; verify if a CA was provided, else accept.
    rejectUnauthorized: !!upstreamCA,
    servername: process.env.SHHTERM_UPSTREAM_SNI || UP_HOST,
  }, () => { /* connected */ });

  upstream.on("error", closeAll);
  upstream.on("close", closeAll);

  // Upstream -> browser: parse 4-byte length-prefixed JSON, forward as WS text.
  upstream.on("data", (chunk) => {
    try {
      upBuf = Buffer.concat([upBuf, chunk]);
      for (;;) {
        if (upBuf.length < 4) break;
        const n = upBuf.readUInt32BE(0);
        if (n > MAX_FRAME) { closeAll(); return; }
        if (upBuf.length < 4 + n) break;
        const json = upBuf.subarray(4, 4 + n).toString("utf8");
        upBuf = upBuf.subarray(4 + n);
        if (!closed) browser.write(wsFrame(json));
      }
    } catch { closeAll(); }
  });

  // Browser -> upstream: parse WS frames, forward payload as length-prefixed.
  browser.on("data", (chunk) => {
    try {
      inBuf = Buffer.concat([inBuf, chunk]);
      for (;;) {
        const frame = parseWsFrame(inBuf);
        if (!frame) break;
        inBuf = frame.rest;
        if (frame.opcode === 0x8) { closeAll(); return; }     // close
        if (frame.opcode === 0x9) continue;                    // ping (ignore)
        if (frame.opcode === 0x1 || frame.opcode === 0x0) {    // text/continuation
          const payload = frame.payload.toString("utf8");
          if (payload.length > MAX_FRAME) { closeAll(); return; }
          const hdr = Buffer.alloc(4);
          hdr.writeUInt32BE(Buffer.byteLength(payload), 0);
          if (upstream && !closed) upstream.write(Buffer.concat([hdr, Buffer.from(payload, "utf8")]));
        }
      }
    } catch { closeAll(); }
  });
  browser.on("error", closeAll);
  browser.on("close", closeAll);
}

// Parse one client->server WS frame (always masked). Returns
// {opcode, payload, rest} or null if incomplete.
function parseWsFrame(buf) {
  if (buf.length < 2) return null;
  const opcode = buf[0] & 0x0f;
  const masked = (buf[1] & 0x80) !== 0;
  let len = buf[1] & 0x7f;
  let offset = 2;
  if (len === 126) {
    if (buf.length < 4) return null;
    len = buf.readUInt16BE(2); offset = 4;
  } else if (len === 127) {
    if (buf.length < 10) return null;
    len = Number(buf.readBigUInt64BE(2)); offset = 10;
  }
  if (len > MAX_FRAME) return { opcode: 0x8, payload: Buffer.alloc(0), rest: Buffer.alloc(0) };
  const maskLen = masked ? 4 : 0;
  if (buf.length < offset + maskLen + len) return null;
  let payload;
  if (masked) {
    const mask = buf.subarray(offset, offset + 4);
    payload = Buffer.alloc(len);
    for (let i = 0; i < len; i++) payload[i] = buf[offset + 4 + i] ^ mask[i % 4];
  } else {
    payload = buf.subarray(offset, offset + len);
  }
  return { opcode, payload, rest: buf.subarray(offset + maskLen + len) };
}

// Last-resort guard: a relay should never die from one bad request/connection.
// Per-connection errors are already handled above; this just logs anything that
// slips through instead of crashing the whole service.
process.on("uncaughtException", (err) => {
  console.error("uncaughtException (ignored):", err && err.message);
});

server.listen(PORT, () => {
  console.log(`shhterm web gateway on :${PORT} -> ${UP_HOST}:${UP_PORT} (webroot ${WEBROOT})`);
});
