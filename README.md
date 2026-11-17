# shhterm

[![CI](https://github.com/AhmadFaraz-crypto/shhterm/actions/workflows/ci.yml/badge.svg)](https://github.com/AhmadFaraz-crypto/shhterm/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

**[📖 Website &amp; docs](https://ahmadfaraz-crypto.github.io/shhterm/)** · **[🌐 Live web client](https://shhterm.duckdns.org)**


**End-to-end encrypted terminal chat.** No registration, no email, no passwords —
a room is created on demand and its **room key is the only credential**. Share
the key and your friend joins from their terminal (or a browser). Messages are
end-to-end encrypted; the server routes only ciphertext.

![shhterm demo](docs/shhterm-demo.gif)

```
$ shhterm create --name Ahmad
  → shows a room key like  5244-MV1W-NMTG-B4E9-1VKJ-V28C-GC

$ shhterm join 5244-MV1W-NMTG-B4E9-1VKJ-V28C-GC --name Ali
  → both of you are now chatting
```

## Try it right now

A community server is live, so you can chat immediately:

- **Browser (any device, incl. iPhone):** open **[shhterm.duckdns.org](https://shhterm.duckdns.org)** — nothing to install.
- **Terminal:** `git clone` + build (below), then just `./build/shhterm create --name YourName` — the public server address is the default and uses a trusted certificate, so **no flags or cert files are needed**.

> The community server is a small, best-effort box with no uptime guarantee and
> no logging (messages are end-to-end encrypted and never stored). For anything
> you care about, [self-host](DEPLOYMENT.md) — it's one small VPS.

## Features

- 🔒 **End-to-end encrypted** messages (AES-256-GCM; the server sees only ciphertext)
- 🔑 **No accounts** — a 256-bit room key is the only credential
- 💻 **Terminal UI** (FTXUI) with colored names, timestamps, typing indicators, and **themes**
- 🌐 **Browser client** — works on any phone/desktop, including **iPhone**, sharing rooms with terminal users
- 🔁 **Auto-reconnect** with backoff; no false "delivered" claims (ack-tracked `·`/`✓`/`✗`)
- 🏢 **Self-hostable** — one cheap VPS, or a LAN-only server for same-network use
- 🧵 Multiple rooms, multiple users, join/leave notices, `/invite`, `/msg`, `/nick`, and more
- 🧪 Tested — unit, real-TLS end-to-end, and C++↔browser crypto interop

### Built with

- **Language / build:** C++20, CMake, Ninja
- **Networking:** Boost.Asio (asynchronous), OpenSSL (TLS 1.2+)
- **UI:** [FTXUI](https://github.com/ArthurSonzogni/FTXUI) terminal interface; Web Crypto in the browser

### Platform support

| Platform | Supported? | How |
|---|---|---|
| **macOS** | ✅ | Homebrew deps, build from source |
| **Linux** | ✅ | apt/dnf deps, build from source |
| **Android** | ✅ | [Termux](https://termux.dev) (from **F-Droid**, not Play Store) — builds natively |
| **Windows** | ✅ via **WSL** | `wsl --install`, then follow the Linux steps |
| **Windows (native MSVC)** | ⚠️ needs porting | a few POSIX calls (`localtime_r`, `popen`) need Windows equivalents |
| **iPhone / iPad** | ✅ via **web client** | no terminal app can run this, but the browser client works in Safari — see [Web client](#web-client) |
| **Any browser** (incl. iPhone/Android) | ✅ | open the web client URL — nothing to install |

> The terminal app targets macOS/Linux/Android(Termux)/WSL. For phones —
> **especially iPhone, which has no real terminal** — use the **web client**,
> which runs in any browser and shares rooms (and end-to-end encryption) with
> terminal users.

---

## Security model (read this)

- **Message bodies are end-to-end encrypted** (AES-256-GCM). The room key has
  two halves: a *routing* half the server uses to find the room, and a *secret*
  half that **never leaves the clients** and is used to derive the message key
  (HKDF-SHA256). The server therefore routes only opaque ciphertext and
  **cannot read message contents** — verified: its logs contain none.
- **TLS also protects the connection** to the server; the client verifies the
  server's certificate (hostname + chain). So messages are protected both in
  transit (TLS) and from the server itself (E2E).
- **What is NOT hidden:** metadata the server must act on — usernames, who is in
  a room, and message size/timing. E2E protects *contents*, not *metadata*.
  Direct messages (`/msg`) are routed to one recipient but share the same room
  key, so they are private from the server, not cryptographically private from
  other room members.
- The **room key carries 256 bits of CSPRNG entropy** total (128 routing + 128
  secret, OpenSSL `RAND_bytes`, Crockford base32). Anyone with the full key can
  join *and* decrypt — treat it like a password and share it over a channel you
  trust.
- **Nothing is persisted.** Rooms and messages live only in server memory;
  message contents and room keys are never written to disk or logged.
- A room is **deleted after it has been empty for 5 minutes**, which also gives a
  brief window to reconnect after a drop without losing the room.

---

## Repository layout

```
proto/     Shared wire protocol: framing, JSON messages, key generation
server/    TLS server: sessions, rooms, lifecycle, rate limiting
client/    TLS client: networking, reconnection, FTXUI interface, E2E crypto
web/       Browser client (HTML/JS, Web Crypto E2E) + Node WebSocket gateway
tests/     doctest suite (unit + real-TLS end-to-end)
tools/     Crypto interop helpers (C++ ↔ browser verification)
scripts/   Dev certificate generator, systemd unit
```

The protocol is documented at the top of [`proto/protocol.hpp`](proto/protocol.hpp).

---

## Dependencies

Build tools and two system libraries are required; three smaller libraries
(FTXUI, nlohmann/json, doctest) are fetched automatically by CMake.

### macOS (Homebrew)

```bash
brew install cmake ninja boost openssl@3
```

### Ubuntu / Debian

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build \
    libboost-dev libssl-dev git
```

> A C++20 compiler is required (GCC 11+ or Clang 14+).

### Android (Termux)

Install **Termux from [F-Droid](https://f-droid.org)** (the Play Store version is
outdated — do not use it). Then inside Termux:

```bash
pkg update -y && pkg upgrade -y
pkg install -y clang cmake ninja git openssl boost boost-headers wget
```

> **Important:** install **both** `boost` *and* `boost-headers` — Termux ships
> Boost's compiled libraries and its headers (including Boost.Asio) in separate
> packages. Without `boost-headers` the build fails with
> `fatal error: 'boost/asio/executor_work_guard.hpp' file not found`.

Termux has no `sudo` and uses `pkg` instead of `apt`, so do **not** run the
Ubuntu `apt-get` commands or `BUILD_ON_UBUNTU.sh` there. Build with CMake
directly (see below), then run the client as `./build/shhterm` — it is not on
`$PATH` unless you copy it into `$PREFIX/bin`.

### Windows

Use **WSL** (Windows Subsystem for Linux): run `wsl --install` in an
Administrator PowerShell, reboot, then follow the Ubuntu steps inside the Ubuntu
shell. A native MSVC build is not yet supported out of the box (a few POSIX
calls need Windows equivalents).

---

## Build

```bash
git clone <this-repo> shhterm && cd shhterm
cmake -S . -B build -G Ninja \
    -DCMAKE_PREFIX_PATH=$(brew --prefix) \
    -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)      # macOS only
cmake --build build
```

On Linux and Android (Termux) the two `-D` flags are usually unnecessary:

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

To build only the client (e.g. on a phone, skipping the server and tests):

```bash
cmake -S . -B build -G Ninja -DSHHTERM_BUILD_TESTS=OFF
cmake --build build --target shhterm
```

Artifacts:

- `build/shhterm-server` — the server
- `build/shhterm` — the client
- `build/shhterm_tests` — the test suite

### Baking in a default server address

So that ordinary users only need `create` / `join`, the client has a default
server address compiled in. Set it at configure time:

```bash
cmake -S . -B build -G Ninja -DSHHTERM_DEFAULT_SERVER=chat.example.com:8443
```

Developers can still override it at runtime (see below) without rebuilding.

---

## Run locally

shhterm always uses TLS, so local development needs a certificate. Generate a
self-signed one and tell the client to trust it:

```bash
scripts/gen_dev_certs.sh                 # writes certs/dev.crt and certs/dev.key

# terminal 1 — server
./build/shhterm-server --cert certs/dev.crt --key certs/dev.key

# terminal 2 — create a room
./build/shhterm create --name Ahmad \
    --server localhost:8443 --ca certs/dev.crt --verify-host localhost

# terminal 3 — join it (use the key printed in terminal 2)
./build/shhterm join <room-key> --name Ali \
    --server localhost:8443 --ca certs/dev.crt --verify-host localhost
```

- `--ca certs/dev.crt` tells the client to trust the dev certificate.
- `--verify-host localhost` is the hostname checked against the certificate
  (the dev cert's SAN includes `localhost` and `127.0.0.1`).
- For a quick throwaway test you may use `--insecure` to skip verification
  entirely. **Never** use `--insecure` against a real server.

### In-app commands

| Command             | Effect                                            |
|---------------------|---------------------------------------------------|
| `/invite [name]`    | copy a ready-to-run join command to the clipboard |
| `/copy`             | copy just the room key                            |
| `/clear`            | clear your local chat view (does not affect peers)|
| `/theme [name]`     | change the color theme (dark, ocean, matrix, sunset, light, mono) |
| `/users`            | list everyone in the room                         |
| `/nick <name>`      | change your display name                          |
| `/msg <user> <text>` | send a private (direct) message                  |
| `/help`             | show available commands                           |
| `/quit`             | leave the room and exit (also Ctrl-C / Ctrl-D)    |

**Sharing a room the easy way:** run `/invite` — it builds the full
`shhterm join <key> --name YOURNAME …` command and copies it to your clipboard
(via `pbcopy` / `wl-copy` / `xclip` / `xsel` / `termux-clipboard-set`). Paste it
to a friend; they just change the name and run it. (If a dev certificate is in
use, they also need that `.crt` file.)

The interface also shows **colored usernames**, **HH:MM timestamps**, a
**typing indicator** ("Ali is typing…"), and a 🔒 e2e marker in the status bar.

### Themes

The terminal UI has several color themes — **dark** (default), **ocean**,
**matrix**, **sunset**, **light**, and **mono**. Switch live with `/theme ocean`,
set one at launch with `--theme matrix`, or make it permanent with `theme=matrix`
in `~/.shhtermrc`. (Truecolor themes look best in a terminal with 24-bit color;
`mono` uses only the terminal's own palette.)

### Config file

To avoid passing flags every time, create `~/.shhtermrc`:

```ini
name = Ahmad
server = 34.10.129.25:8443
ca = /home/ahmad/gcp-server.crt
verify_host = 34.10.129.25
theme = ocean
```

Then just run `shhterm create` or `shhterm join <key>`. CLI flags and
`SHHTERM_SERVER` still override the file.

Messages you send show `·` until the server acknowledges them, then `✓`. If the
connection drops before an acknowledgement, they are marked
`✗ (not delivered)` — shhterm never claims delivery for an unacknowledged
message. Incoming messages append to the history and **never disturb the line
you are currently typing**.

---

## Running on a local network (same office / WiFi)

To let people on the **same network** chat without any cloud server or internet,
one person runs `scripts/run-lan-server.sh` (it auto-detects the LAN IP, makes a
cert, and prints the exact connect commands) and others join that IP. Details in
**[DEPLOYMENT.md](DEPLOYMENT.md#local-network-only-same-office--wifi)**.

## Configuring the server address

Resolution order used by the client (first match wins):

1. `--server host:port` command-line option
2. `SHHTERM_SERVER` environment variable
3. the address compiled in via `SHHTERM_DEFAULT_SERVER`

```bash
export SHHTERM_SERVER=chat.example.com:8443
shhterm create --name Ahmad          # no --server needed
```

---

## Web client

For browsers — **including iPhone/iPad, which cannot run the terminal app** — a
web client in [`web/`](web/) provides the same chat in any browser, with **no
install**. It shares rooms and end-to-end encryption with terminal users: a
browser user and a terminal user can read each other's messages.

### How it fits together

```
  browser (phone/desktop)
     │  HTTPS (loads page) + wss (chat), via a trusted cert
     ▼
  Node gateway  ──raw TLS──►  C++ server (localhost:8443)
  (serves web/, bridges        same RoomManager, same rooms
   WebSocket ⇄ length-prefixed JSON frames)
```

- The browser speaks the **same JSON protocol** and performs the **same E2E
  encryption** (AES-256-GCM + HKDF-SHA256 via the **Web Crypto API**). The
  room key still splits into routing + secret halves; the secret stays in the
  browser, so the gateway and server only ever see ciphertext.
- The gateway ([`web/gateway.js`](web/gateway.js)) is a dependency-free Node
  program that only translates transports — it never decrypts anything.
- Crypto compatibility between the browser and the C++ client is verified by
  [`tools/interop_test.mjs`](tools/interop_test.mjs) (run after a test build:
  `node tools/interop_test.mjs`).

A browser client needs a **real TLS certificate** (browsers reject self-signed
certs on a bare IP, especially iOS Safari), so it requires a domain + Let's
Encrypt — a free [DuckDNS](https://www.duckdns.org) subdomain works well.
Setup steps are in **[DEPLOYMENT.md](DEPLOYMENT.md#adding-the-web-client)**.

Using it: open the site, **Create** or **Join** a room, and share the
**Copy link** / **Copy key** buttons (or `/invite`, which copies a
`…/room/<key>` link that pre-fills the key). Same slash commands as the terminal.

---

## Protocol summary

Each frame is a 4-byte big-endian length prefix followed by that many bytes of
UTF-8 JSON:

```
[ uint32 length N ][ N bytes JSON ]
```

- Maximum frame size: **64 KiB**; larger frames close the connection.
- Maximum message text: **4000 bytes**.
- Inbound chat is **rate limited** per connection (token bucket: burst 10,
  refill 5/s); excess messages get a `rate_limited` error and are not routed.

Message types and error codes are documented in
[`proto/protocol.hpp`](proto/protocol.hpp).

---

## Tests

```bash
./build/shhterm_tests          # or:  ctest --test-dir build --output-on-failure
```

Coverage:

- **Protocol** — framing round-trip, split/partial reads, oversized-frame
  rejection, message validation, key entropy and normalization.
- **Room access** — key lookup/normalization, rejection of unknown keys,
  duplicate-name rejection.
- **Message routing** — broadcast reaches peers but not the sender; separate
  rooms are isolated.
- **Room expiry** — deletion after the empty TTL, and cancellation when a client
  rejoins within the window.
- **End-to-end over real TLS** — a live server plus two real clients: create,
  join, presence, message delivery, acknowledgement, and clean failure on a bad
  key. (These generate a throwaway cert with the system `openssl`; they skip if
  it is absent.)

---

## Deployment

Running your own server is easy and cheap (~$3/mo, or free-tier on some clouds).
See **[DEPLOYMENT.md](DEPLOYMENT.md)** for step-by-step guides:

- **[Public VPS with a domain](DEPLOYMENT.md#public-vps-with-a-domain-recommended)** — trusted cert, clients need no flags (recommended)
- **[Adding the web client](DEPLOYMENT.md#adding-the-web-client)** — browser/phone access via the Node gateway
- **[Public VPS with just an IP](DEPLOYMENT.md#public-vps-with-just-an-ip-no-domain)** — quickest, clients need the cert file
- **[Local network only](DEPLOYMENT.md#local-network-only-same-office--wifi)** — no cloud, no internet

---

## Troubleshooting

Real issues seen in practice and how to fix them:

| Symptom | Cause / fix |
|---|---|
| `bash: wget: command not found` (Termux) | `pkg install -y wget` (or use `curl -O <url>`). |
| `fatal error: 'boost/asio/executor_work_guard.hpp' file not found` | Boost headers missing. On Termux: `pkg install -y boost-headers`. On Debian/Ubuntu: `sudo apt-get install -y libboost-dev`, then rebuild. |
| `command not found: shhterm` | The binary is not on `$PATH`. Run it as `./build/shhterm …` from the project folder, or `cp build/shhterm $PREFIX/bin/` (Termux) / a directory on your `PATH`. |
| `tar: Ignoring unknown extended header keyword 'LIBARCHIVE.xattr.com.apple.provenance'` | Harmless. The archive was created on macOS; these Apple-only metadata tags are simply skipped. Extraction still succeeds. |
| `GPG error … kali-rolling … NO_PUBKEY …` (Kali, incl. Kali-on-Android) | Kali rotated its repo signing key. Install the current one: `wget -qO - https://archive.kali.org/archive-key.asc \| gpg --dearmor -o /usr/share/keyrings/kali-archive-keyring.gpg` then `apt update`. Not a shhterm issue. |
| CMake error: *CMakeCache.txt directory is different* | You moved the project folder after building. Delete `build/` and re-run `cmake -S . -B build …` (CMake caches absolute paths). |
| `[unable to decrypt message]` in the chat | The other side used a different room key, or only the routing half was shared. Make sure everyone uses the **full** key from `/invite` or `/copy` (it contains the secret half). |
| TLS verification fails against a self-signed server | Pass `--ca <cert>` and `--verify-host <ip-or-host>` matching the cert's SAN. `--insecure` disables verification (dev only). |

---

## Graceful handling

shhterm is built to degrade cleanly:

- **Invalid key / duplicate name** → a clear error; the client stops instead of
  retrying a request that cannot succeed.
- **Server outage or dropped connection** → the client reconnects automatically
  with exponential backoff (1s → 30s cap), showing `reconnecting…` in the status
  bar and `connected` once it rejoins. Thanks to the 5-minute empty-room window,
  a room survives a brief server/client blip.
- **Disconnected peers** → the server detects the closed socket, removes the
  member, and broadcasts a leave notification.
- **Oversized frames / flooding** → enforced by size limits and per-connection
  rate limiting.

---

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for how to
build, test, and submit changes. Please run the test suite (and
`node tools/interop_test.mjs` if you touch the protocol or crypto) before
opening a pull request.

## Security & privacy

shhterm is honest about what it protects. For the security model and how to
report a vulnerability, see [SECURITY.md](SECURITY.md); for what the server does
and doesn't collect, see [PRIVACY.md](PRIVACY.md). In short: message bodies are
end-to-end encrypted and never persisted or logged; metadata (usernames, room
membership, timing) is visible to the server; the room key is the only
credential — share it over a channel you trust.

> TLS protects the transport and message bodies are end-to-end encrypted, but
> this is a hobby/educational project — it has not had a formal security audit.
> Don't rely on it for high-stakes confidentiality.

## License

[MIT](LICENSE) © Ahmad Faraz.
