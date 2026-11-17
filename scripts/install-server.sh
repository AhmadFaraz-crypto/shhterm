#!/usr/bin/env bash
#
# One-command shhterm server installer for Ubuntu/Debian (incl. most VPS).
#
# Usage (run from the repo root):
#   sudo ./scripts/install-server.sh                 # self-signed cert for this box's public IP
#   sudo ./scripts/install-server.sh chat.example.com  # trusted Let's Encrypt cert (domain must point here)
#
# With a DOMAIN you get a trusted certificate, so clients need no cert flags.
# Without one, a self-signed cert is generated for the public IP and clients
# must pass --ca server.crt --verify-host <ip>.
#
# This installs build tools, builds the server, creates a locked-down service
# account, obtains a certificate, installs a systemd service, and starts it.

set -euo pipefail

DOMAIN="${1:-}"
PORT="${SHHTERM_PORT:-8443}"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $EUID -ne 0 ]]; then
  echo "Please run with sudo: sudo $0 ${DOMAIN}" >&2
  exit 1
fi
if ! command -v apt-get >/dev/null 2>&1; then
  echo "This script targets Ubuntu/Debian (apt). For other systems see DEPLOYMENT.md." >&2
  exit 1
fi

echo "==> Installing build dependencies…"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential cmake ninja-build libboost-dev libssl-dev git curl
[[ -n "$DOMAIN" ]] && apt-get install -y -qq certbot

echo "==> Building the server…"
cd "$REPO_DIR"
cmake -S . -B build -G Ninja -DSHHTERM_BUILD_TESTS=OFF >/dev/null
cmake --build build --target shhterm-server >/dev/null
install -D build/shhterm-server /opt/shhterm/bin/shhterm-server

echo "==> Creating service account and directories…"
id shhterm >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin shhterm
mkdir -p /etc/shhterm

CERT=/etc/shhterm/server.crt
KEY=/etc/shhterm/server.key
EXTRA_FW=""

if [[ -n "$DOMAIN" ]]; then
  echo "==> Obtaining a Let's Encrypt certificate for $DOMAIN (needs port 80 reachable)…"
  certbot certonly --standalone --non-interactive --agree-tos \
    --register-unsafely-without-email -d "$DOMAIN"
  ln -sf "/etc/letsencrypt/live/$DOMAIN/fullchain.pem" "$CERT"
  ln -sf "/etc/letsencrypt/live/$DOMAIN/privkey.pem"   "$KEY"
  chgrp -R shhterm /etc/letsencrypt/live /etc/letsencrypt/archive
  chmod -R g+rX     /etc/letsencrypt/live /etc/letsencrypt/archive
  # Restart on renewal so the new cert is picked up.
  mkdir -p /etc/letsencrypt/renewal-hooks/deploy
  cat > /etc/letsencrypt/renewal-hooks/deploy/restart-shhterm.sh <<'HOOK'
#!/usr/bin/env bash
chgrp -R shhterm /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null || true
chmod -R g+rX    /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null || true
systemctl restart shhterm-server
HOOK
  chmod +x /etc/letsencrypt/renewal-hooks/deploy/restart-shhterm.sh
  EXTRA_FW="80/tcp"
  ADDR="$DOMAIN"
  CLIENT_FLAGS=""
else
  IP="$(curl -s --max-time 10 https://api.ipify.org || true)"
  [[ -z "$IP" ]] && IP="$(hostname -I | awk '{print $1}')"
  echo "==> Generating a self-signed certificate for $IP…"
  openssl req -x509 -newkey rsa:2048 -nodes -days 825 \
    -keyout "$KEY" -out "$CERT" \
    -subj "/CN=$IP" -addext "subjectAltName=IP:$IP" >/dev/null 2>&1
  chgrp shhterm "$KEY" "$CERT"; chmod 640 "$KEY"; chmod 644 "$CERT"
  ADDR="$IP"
  CLIENT_FLAGS=" --ca server.crt --verify-host $IP"
fi

echo "==> Installing and starting the systemd service…"
cat > /etc/systemd/system/shhterm-server.service <<UNIT
[Unit]
Description=shhterm server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=shhterm
Group=shhterm
ExecStart=/opt/shhterm/bin/shhterm-server --address 0.0.0.0 --port $PORT --cert $CERT --key $KEY --ttl-seconds 300
Restart=on-failure
RestartSec=2
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadOnlyPaths=/etc/shhterm /etc/letsencrypt

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable --now shhterm-server

# Open the firewall if ufw is active.
if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "Status: active"; then
  ufw allow "$PORT/tcp" >/dev/null 2>&1 || true
  [[ -n "$EXTRA_FW" ]] && ufw allow "$EXTRA_FW" >/dev/null 2>&1 || true
fi

sleep 1
echo
echo "────────────────────────────────────────────────────────────────"
echo "  shhterm server is running: $(systemctl is-active shhterm-server)"
echo "  Address: ${ADDR}:${PORT}"
echo
echo "  Clients connect with:"
echo "    shhterm create --name NAME --server ${ADDR}:${PORT}${CLIENT_FLAGS}"
if [[ -z "$DOMAIN" ]]; then
  echo "  (share $CERT with users — they pass it via --ca)"
else
  echo "  (trusted cert — no extra flags needed)"
fi
echo
echo "  Tip: build the client with -DSHHTERM_DEFAULT_SERVER=${ADDR}:${PORT}"
echo "       so users can just run 'shhterm create --name NAME'."
echo
echo "  NOTE: make sure your cloud firewall also allows inbound TCP ${PORT}."
echo "────────────────────────────────────────────────────────────────"
