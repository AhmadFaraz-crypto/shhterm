// shhterm web UI controller: binds the DOM to shhtermClient.
"use strict";

const $ = (id) => document.getElementById(id);
const client = new shhtermClient();

let mode = "join";            // "join" | "create"
let selfName = "";
let roomKey = "";
const pending = new Map();    // id -> message element
const typingAt = new Map();   // name -> last typing timestamp (ms)

// ---- helpers ---------------------------------------------------------------

// Stable per-name color.
function nameColor(name) {
  let h = 5381;
  for (const c of name) h = ((h << 5) + h + c.charCodeAt(0)) >>> 0;
  const hues = [150, 200, 280, 45, 220, 0, 170, 320, 95, 260];
  return `hsl(${hues[h % hues.length]} 70% 65%)`;
}
function hhmm() {
  const d = new Date();
  return String(d.getHours()).padStart(2, "0") + ":" +
         String(d.getMinutes()).padStart(2, "0");
}
function esc(s) {
  return s.replace(/[&<>"]/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}
function atBottom() {
  const m = $("messages");
  return m.scrollHeight - m.scrollTop - m.clientHeight < 60;
}
function scrollDown() { const m = $("messages"); m.scrollTop = m.scrollHeight; }

function addSystem(text) {
  const stick = atBottom();
  const el = document.createElement("div");
  el.className = "msg system";
  el.textContent = text;
  $("messages").appendChild(el);
  if (stick) scrollDown();
  return el;
}
function addChat(name, text, kind) {
  const stick = atBottom();
  const el = document.createElement("div");
  el.className = "msg" + (kind === "dm" ? " dm" : "");
  el.innerHTML =
    `<span class="time">${hhmm()}</span>` +
    `<span class="who" style="color:${nameColor(name)}">${esc(name)}:</span> ` +
    `<span class="body">${esc(text)}</span>`;
  $("messages").appendChild(el);
  if (stick) scrollDown();
  return el;
}
function setMark(el, cls, sym) {
  let m = el.querySelector(".mark");
  if (!m) { m = document.createElement("span"); m.className = "mark"; el.appendChild(m); }
  m.className = "mark " + cls;
  m.textContent = sym;
}

// ---- join screen -----------------------------------------------------------

function setMode(m) {
  mode = m;
  $("tab-join").classList.toggle("active", m === "join");
  $("tab-create").classList.toggle("active", m === "create");
  $("keyField").style.display = (m === "join") ? "" : "none";
  $("go").textContent = (m === "join") ? "Join" : "Create";
}
$("tab-join").onclick = () => setMode("join");
$("tab-create").onclick = () => setMode("create");

// Prefill a key from the URL: /room/<key>  or  #<key>
(function prefillKey() {
  let key = "";
  const path = location.pathname.match(/\/room\/(.+)$/);
  if (path) key = decodeURIComponent(path[1]);
  else if (location.hash.length > 1) key = decodeURIComponent(location.hash.slice(1));
  if (key) { $("keyInput").value = key; setMode("join"); }
})();

$("go").onclick = start;
$("nameInput").addEventListener("keydown", (e) => { if (e.key === "Enter") start(); });
$("keyInput").addEventListener("keydown", (e) => { if (e.key === "Enter") $("nameInput").focus(); });

function start() {
  const name = $("nameInput").value.trim();
  const key = $("keyInput").value.trim();
  if (!name) { $("joinErr").textContent = "Please enter a name."; return; }
  if (mode === "join" && !key) { $("joinErr").textContent = "Please paste a room key."; return; }
  selfName = name;
  $("join").style.display = "none";
  $("chat").style.display = "flex";
  client.connect({ create: mode === "create", key, name });
}

// ---- chat screen -----------------------------------------------------------

function setStatus(state, text) {
  $("status").textContent = text || state;
  const dot = $("dot");
  dot.className = "dot " +
    (state === "connected" ? "ok" :
     (state === "failed" || state === "closed") ? "bad" : "wait");
}

function copyText(text, okMsg) {
  const done = () => addSystem(okMsg);
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(done, () => addSystem(text));
  } else {
    addSystem(text);
  }
}

function inviteLink() {
  return location.origin + "/room/" + encodeURIComponent(roomKey);
}

function handleCommand(line) {
  const [cmd, ...restArr] = line.split(" ");
  const rest = restArr.join(" ");
  switch (cmd) {
    case "/help":
      addSystem("Commands:");
      addSystem("  /invite  - copy a shareable link to this room");
      addSystem("  /copy    - copy the room key");
      addSystem("  /clear   - clear your chat view");
      addSystem("  /users   - list people in this room");
      addSystem("  /nick <name>       - change your name");
      addSystem("  /msg <user> <text> - private message");
      addSystem("  /quit    - leave");
      break;
    case "/invite":
      if (!roomKey) { addSystem("No room yet."); break; }
      copyText(inviteLink(), "✓ Invite link copied: " + inviteLink());
      break;
    case "/copy":
      if (!roomKey) { addSystem("No room key yet."); break; }
      copyText(roomKey, "✓ Room key copied.");
      break;
    case "/clear":
      $("messages").innerHTML = "";
      pending.clear();
      addSystem("— cleared —");
      break;
    case "/users":
      client.requestUsers();
      break;
    case "/nick":
      if (!rest) addSystem("Usage: /nick <name>");
      else client.setNick(rest);
      break;
    case "/msg": {
      const sp = rest.indexOf(" ");
      if (sp <= 0) { addSystem("Usage: /msg <user> <message>"); break; }
      const to = rest.slice(0, sp), body = rest.slice(sp + 1);
      client.sendChat(body, to).then((id) => {
        if (!id) { addSystem("Not connected — DM not sent."); return; }
        const el = addChat(selfName + " → " + to, body, "dm");
        setMark(el, "pending", "·");
        pending.set(id, el);
      });
      break;
    }
    case "/quit":
      client.quit();
      addSystem("Left the room.");
      break;
    default:
      addSystem("Unknown command: " + cmd + "  (try /help)");
  }
}

function submit() {
  const line = $("input").value;
  $("input").value = "";
  if (!line.trim()) return;
  if (line[0] === "/") { handleCommand(line); return; }
  client.sendChat(line).then((id) => {
    if (!id) { addSystem("You: " + line + "  (not connected — not sent)"); return; }
    const el = addChat(selfName, line, "self");
    setMark(el, "pending", "·");
    pending.set(id, el);
  });
}
$("sendBtn").onclick = submit;
$("input").addEventListener("keydown", (e) => { if (e.key === "Enter") submit(); });
$("input").addEventListener("input", () => {
  if ($("input").value && $("input").value[0] !== "/") client.sendTyping();
});
$("leaveBtn").onclick = () => { client.quit(); location.href = "/"; };

// Refresh the typing indicator line periodically.
setInterval(() => {
  const now = Date.now();
  const active = [];
  for (const [name, ts] of typingAt) {
    if (now - ts < 3000) active.push(name); else typingAt.delete(name);
  }
  $("typing").textContent =
    active.length === 1 ? active[0] + " is typing…" :
    active.length > 1 ? active.length + " people are typing…" : "";
}, 800);

// ---- events from the client ------------------------------------------------

client.on = (ev) => {
  switch (ev.type) {
    case "status":
      setStatus(ev.state, ev.text);
      if (ev.state === "reconnecting" || ev.state === "failed" || ev.state === "closed") {
        for (const el of pending.values()) setMark(el, "fail", "✗ not delivered");
        pending.clear();
        if (ev.text) addSystem("[" + ev.text + "]");
      }
      break;
    case "welcome": {
      roomKey = ev.fullKey;
      selfName = ev.name;
      $("meta").textContent = "you: " + selfName + " · " + ev.users.length + " online";
      addSystem(mode === "create" ? "Room created. Share this to invite people:"
                                  : "Joined the room (end-to-end encrypted).");
      const kl = document.createElement("div");
      kl.className = "keyline";
      kl.innerHTML = `<code>${esc(roomKey)}</code>`;
      const b1 = document.createElement("button"); b1.textContent = "Copy link";
      b1.onclick = () => copyText(inviteLink(), "✓ Invite link copied.");
      const b2 = document.createElement("button"); b2.textContent = "Copy key";
      b2.onclick = () => copyText(roomKey, "✓ Room key copied.");
      kl.append(b1, b2);
      $("messages").appendChild(kl);
      addSystem("In room: " + ev.users.join(", "));
      scrollDown();
      break;
    }
    case "chat":
      typingAt.delete(ev.from);
      addChat(ev.from, ev.text, ev.dm ? "dm" : "chat");
      break;
    case "presence":
      typingAt.delete(ev.name);
      addSystem("* " + ev.name + (ev.event === "join" ? " joined" : " left"));
      break;
    case "nick":
      typingAt.delete(ev.old);
      if (ev.old === selfName) selfName = ev.name;
      addSystem("* " + ev.old + " is now " + ev.name);
      break;
    case "typing":
      typingAt.set(ev.from, Date.now());
      break;
    case "users":
      addSystem("Users (" + ev.users.length + "): " + ev.users.join(", "));
      $("meta").textContent = "you: " + selfName + " · " + ev.users.length + " online";
      break;
    case "ack": {
      const el = pending.get(ev.id);
      if (el) { setMark(el, "ok", "✓"); pending.delete(ev.id); }
      break;
    }
    case "notice":
      addSystem("[" + ev.text + "]");
      break;
  }
};
