# Deploying shhterm

## Quick install (Ubuntu/Debian)

On a fresh VPS, from the repo root:

```bash
git clone https://github.com/AhmadFaraz-crypto/shhterm && cd shhterm

# With a domain (trusted cert, clients need no flags) — domain must point here:
sudo ./scripts/install-server.sh chat.example.com

# Or with no domain (self-signed cert for this box's public IP):
sudo ./scripts/install-server.sh
```

It installs dependencies, builds the server, creates a locked-down service
account, obtains a certificate (Let's Encrypt with a domain, self-signed
otherwise), installs a `systemd` service, and prints the exact client command.
**Also open inbound TCP 8443 in your cloud firewall.** The sections below explain
each step if you prefer to do it manually or want the web client.

---

Ways to run your own shhterm server. Pick one:

- [Public VPS with a domain](#public-vps-with-a-domain-recommended) — trusted
  cert, clients need no flags; add the [web client](#adding-the-web-client) for
  browsers/phones.
- [Public VPS with just an IP](#public-vps-with-just-an-ip-no-domain) — quickest,
  but each client needs the cert file.
- [Local network only](#local-network-only-same-office--wifi) — no cloud, no
  internet.

The server keeps everything in memory and never logs message contents or room
keys. It runs single-threaded, uses ~1–2 MB of RAM, and fits comfortably on the
smallest (free-tier-eligible) cloud VM.

---

## Public VPS with a domain (recommended)

Puts a production TLS server behind a real (Let's Encrypt) certificate, so
clients connect with no `--ca` / `--verify-host` flags.

### 1. Build on the server (or copy a build over)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build libboost-dev libssl-dev git
git clone https://github.com/AhmadFaraz-crypto/shhterm shhterm && cd shhterm
cmake -S . -B build -G Ninja -DSHHTERM_DEFAULT_SERVER=chat.example.com:8443
cmake --build build
sudo install -D build/shhterm-server /opt/shhterm/bin/shhterm-server
```

### 2. Get a trusted certificate

Point a DNS A record (e.g. `chat.example.com`) at the VPS. A free
[DuckDNS](https://www.duckdns.org) subdomain works great. Then:

```bash
sudo apt-get install -y certbot
sudo certbot certonly --standalone -d chat.example.com
# certs land in /etc/letsencrypt/live/chat.example.com/
```

Create the service account and make the certs readable by it:

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin shhterm
sudo mkdir -p /etc/shhterm
sudo ln -sf /etc/letsencrypt/live/chat.example.com/fullchain.pem /etc/shhterm/fullchain.pem
sudo ln -sf /etc/letsencrypt/live/chat.example.com/privkey.pem   /etc/shhterm/privkey.pem
sudo chgrp -R shhterm /etc/letsencrypt/live /etc/letsencrypt/archive
sudo chmod -R g+rX     /etc/letsencrypt/live /etc/letsencrypt/archive
```

### 3. Install the systemd service

Point the unit's `--cert` / `--key` at `/etc/shhterm/fullchain.pem` and
`/etc/shhterm/privkey.pem`, then:

```bash
sudo cp scripts/shhterm-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now shhterm-server
systemctl status shhterm-server
```

The unit ([`scripts/shhterm-server.service`](scripts/shhterm-server.service))
runs as the unprivileged `shhterm` user with filesystem hardening and restarts
on failure.

### 4. Open the firewall

```bash
sudo ufw allow 8443/tcp          # plus 80,443 if you add the web client
```

### 5. Clients just need the address

Because the domain has a publicly trusted certificate, clients need no
`--ca` / `--verify-host` flags:

```bash
shhterm create --name Ahmad --server chat.example.com:8443
# or set SHHTERM_SERVER / bake it in at build time, then simply:
shhterm create --name Ahmad
```

### Certificate renewal

`certbot` renews automatically, but the server reads the cert only at startup —
so restart it after renewal. Install a deploy hook (this also re-applies group
read permissions on the new files):

```bash
sudo tee /etc/letsencrypt/renewal-hooks/deploy/restart-shhterm.sh >/dev/null <<'HOOK'
#!/usr/bin/env bash
chgrp -R shhterm /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null || true
chmod -R g+rX    /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null || true
systemctl restart shhterm-server shhterm-gateway
HOOK
sudo chmod +x /etc/letsencrypt/renewal-hooks/deploy/restart-shhterm.sh
```

---

## Adding the web client

For browsers — **including iPhone/iPad, which can't run the terminal app** — a
small Node gateway serves the web client and bridges browser WebSockets to the
C++ server. It shares rooms and end-to-end encryption with terminal users.

```
  browser (phone/desktop)
     │  HTTPS (page) + wss (chat), via the trusted cert
     ▼
  Node gateway  ──TLS──►  C++ server (localhost:8443)
  (serves web/, bridges WebSocket ⇄ length-prefixed JSON frames)
```

The gateway ([`web/gateway.js`](web/gateway.js)) is dependency-free and never
decrypts anything — it only translates transports. It needs the **same trusted
certificate** (browsers reject self-signed certs, especially iOS Safari).

```bash
# Prereqs: the domain + Let's Encrypt cert from above; ports 80 and 443 open.
sudo apt-get install -y nodejs

# Install the web files
sudo mkdir -p /opt/shhterm/web
sudo cp web/index.html web/app.js web/shhterm.js web/gateway.js /opt/shhterm/web/

# Point the gateway at the cert (reuse the Let's Encrypt files)
sudo ln -sf /etc/letsencrypt/live/chat.example.com/fullchain.pem /etc/shhterm/web-fullchain.pem
sudo ln -sf /etc/letsencrypt/live/chat.example.com/privkey.pem   /etc/shhterm/web-privkey.pem

# Install and start the gateway service (binds :443 via CAP_NET_BIND_SERVICE)
sudo cp web/shhterm-gateway.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now shhterm-gateway
```

Tip: you can point the **C++ server** at the same Let's Encrypt cert
(`--cert /etc/shhterm/web-fullchain.pem --key /etc/shhterm/web-privkey.pem`)
so terminal clients also connect with no cert flags.

### Using the web client

- Open `https://chat.example.com` — **Create** or **Join** a room.
- Share a room: the page shows **Copy link** / **Copy key**, and `/invite` copies
  a `https://chat.example.com/room/<key>` link. Opening it pre-fills the key — the
  recipient just enters a name.
- Same commands as the terminal: `/invite`, `/copy`, `/clear`, `/users`,
  `/nick`, `/msg`, `/help`, `/quit`.

---

## Public VPS with just an IP (no domain)

Quickest path, works on any cloud VM (GCP, AWS, DigitalOcean, Hetzner, …), at the
cost of each client needing the cert file.

```bash
# Generate a cert whose SAN is the public IP
IP=203.0.113.10
sudo mkdir -p /etc/shhterm
sudo openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout /etc/shhterm/server.key -out /etc/shhterm/server.crt \
  -days 825 -subj "/CN=$IP" -addext "subjectAltName=IP:$IP"
# point the systemd unit at server.crt / server.key, then start it
```

Open TCP **8443** in the cloud firewall. Because the cert is self-signed, each
client needs the `server.crt` file and must pass it explicitly:

```bash
shhterm join <key> --name Ali \
    --server 203.0.113.10:8443 --ca server.crt --verify-host 203.0.113.10
```

The browser/web client does **not** work with a self-signed cert on an IP — use a
domain for that.

---

## Local network only (same office / WiFi)

To let people on the **same network** chat without any cloud server or internet,
one person hosts the server and others connect to its LAN IP. A helper automates
it:

```bash
scripts/run-lan-server.sh            # auto-detects your LAN IP, makes a cert, runs
```

It detects your LAN address, generates a matching TLS certificate
(`certs/lan.crt`), starts the server, and prints the exact commands plus the cert
file to share. Coworkers then run (using the printed IP):

```bash
shhterm join <key> --name Ali \
    --server 192.168.1.50:8443 --ca lan.crt --verify-host 192.168.1.50
```

They need the client binary and the `lan.crt` file.

**No WiFi at all?** Have one person start a phone/laptop hotspot; everyone joins
it, then use `run-lan-server.sh` as above.

Notes:
- The host machine must stay awake and on the same network — it *is* the server.
- Allow incoming connections on the port if your OS firewall prompts.
- LAN-only works for the **terminal client**; browsers reject self-signed certs
  on a local IP, so for a browser/phone experience use a cloud server + domain.
