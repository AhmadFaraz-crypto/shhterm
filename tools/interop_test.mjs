// Verify the browser Web Crypto scheme (via Node's identical crypto.webcrypto)
// is byte-compatible with the C++ MessageCipher (build/crypto_tool).
//
//   node tools/interop_test.mjs
//
// Exits 0 only if BOTH directions round-trip:
//   C++ encrypt  -> JS decrypt
//   JS  encrypt  -> C++ decrypt

import { execFileSync } from "node:child_process";
import { webcrypto as crypto } from "node:crypto";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const __dirname = dirname(fileURLToPath(import.meta.url));
const TOOL = join(__dirname, "..", "build", "crypto_tool");
const HKDF_INFO = "shhterm-e2e-v1";

// --- JS side (copied minimal form of web/shhterm.js crypto) ---
function b64encode(bytes) {
  return Buffer.from(bytes).toString("base64");
}
function b64decode(str) {
  return new Uint8Array(Buffer.from(str, "base64"));
}
async function deriveKey(secretStr) {
  const enc = new TextEncoder();
  const material = await crypto.subtle.importKey(
    "raw", enc.encode(secretStr), "HKDF", false, ["deriveKey"]);
  return crypto.subtle.deriveKey(
    { name: "HKDF", hash: "SHA-256", salt: new Uint8Array(0),
      info: enc.encode(HKDF_INFO) },
    material, { name: "AES-GCM", length: 256 }, false, ["encrypt", "decrypt"]);
}
async function jsEncrypt(key, plaintext) {
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const ct = new Uint8Array(await crypto.subtle.encrypt(
    { name: "AES-GCM", iv }, key, new TextEncoder().encode(plaintext)));
  const packed = new Uint8Array(iv.length + ct.length);
  packed.set(iv, 0); packed.set(ct, iv.length);
  return b64encode(packed);
}
async function jsDecrypt(key, blob) {
  const packed = b64decode(blob);
  const iv = packed.slice(0, 12);
  const ctAndTag = packed.slice(12);
  const pt = await crypto.subtle.decrypt({ name: "AES-GCM", iv }, key, ctAndTag);
  return new TextDecoder().decode(pt);
}

function cppEncrypt(secret, text) {
  return execFileSync(TOOL, ["encrypt", secret, text]).toString().trim();
}
function cppDecrypt(secret, blob) {
  return execFileSync(TOOL, ["decrypt", secret, blob]).toString().trim();
}

const secret = "K4M92XQP7TAERJ0FNB1CW8G3QZ"; // 26-char base32 (a secret half)
const message = "hello across C++ and the browser 🔐 — ünïcode too";
let ok = true;

// Direction 1: C++ encrypts, JS decrypts.
{
  const key = await deriveKey(secret);
  const blob = cppEncrypt(secret, message);
  const out = await jsDecrypt(key, blob);
  const pass = out === message;
  console.log(`C++ encrypt -> JS decrypt : ${pass ? "PASS" : "FAIL"}`);
  if (!pass) { console.log("  got:", JSON.stringify(out)); ok = false; }
}

// Direction 2: JS encrypts, C++ decrypts.
{
  const key = await deriveKey(secret);
  const blob = await jsEncrypt(key, message);
  const out = cppDecrypt(secret, blob);
  const pass = out === message;
  console.log(`JS  encrypt -> C++ decrypt: ${pass ? "PASS" : "FAIL"}`);
  if (!pass) { console.log("  got:", JSON.stringify(out)); ok = false; }
}

console.log(ok ? "INTEROP OK" : "INTEROP FAILED");
process.exit(ok ? 0 : 1);
