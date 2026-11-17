#!/usr/bin/env bash
#
# Run a shhterm server on your local network (LAN) so people in the same
# office / on the same WiFi can chat without any cloud server or internet.
#
# It auto-detects your LAN IP, generates a matching TLS certificate, starts the
# server, and prints the exact commands your coworkers should run.
#
# Usage:
#   scripts/run-lan-server.sh [port]        # default port 8443
#
# Coworkers also need the client binary and the generated cert file
# (certs/lan.crt). Share both; see the printed instructions.

set -euo pipefail

PORT="${1:-8443}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="$HERE/build/shhterm-server"
CERT_DIR="$HERE/certs"
CRT="$CERT_DIR/lan.crt"
KEY="$CERT_DIR/lan.key"

# ---- locate the server binary ----------------------------------------------
if [[ ! -x "$SERVER_BIN" ]]; then
  echo "error: server binary not found at $SERVER_BIN" >&2
  echo "Build it first:  cmake --build build --target shhterm-server" >&2
  exit 1
fi

# ---- detect the LAN IP ------------------------------------------------------
detect_ip() {
  local ip=""
  if command -v ipconfig >/dev/null 2>&1; then           # macOS
    for iface in en0 en1 en2; do
      ip="$(ipconfig getifaddr "$iface" 2>/dev/null || true)"
      [[ -n "$ip" ]] && break
    done
  fi
  if [[ -z "$ip" ]] && command -v hostname >/dev/null 2>&1; then  # Linux
    ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
  fi
  if [[ -z "$ip" ]] && command -v ip >/dev/null 2>&1; then
    ip="$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="src") print $(i+1)}')"
  fi
  echo "$ip"
}

LAN_IP="${SHHTERM_LAN_IP:-$(detect_ip)}"
if [[ -z "$LAN_IP" ]]; then
  echo "error: could not auto-detect your LAN IP." >&2
  echo "Set it manually:  SHHTERM_LAN_IP=192.168.x.x $0 $PORT" >&2
  exit 1
fi

# ---- generate a cert for that IP (reuse if it already matches) -------------
mkdir -p "$CERT_DIR"
need_cert=1
if [[ -f "$CRT" ]] && openssl x509 -in "$CRT" -noout -text 2>/dev/null \
      | grep -q "IP Address:$LAN_IP"; then
  need_cert=0
fi
if [[ "$need_cert" -eq 1 ]]; then
  echo "Generating a TLS certificate for $LAN_IP …"
  openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
    -keyout "$KEY" -out "$CRT" \
    -subj "/CN=$LAN_IP" \
    -addext "subjectAltName=IP:$LAN_IP,IP:127.0.0.1,DNS:localhost" >/dev/null 2>&1
  chmod 600 "$KEY"
fi

# ---- print coworker instructions -------------------------------------------
cat <<EOF

────────────────────────────────────────────────────────────────────────
  shhterm LAN server
  Your LAN address : ${LAN_IP}:${PORT}
  Certificate      : ${CRT}
────────────────────────────────────────────────────────────────────────

  Share with coworkers on the same WiFi/LAN:
    1) the shhterm client binary (or let them build it), AND
    2) the certificate file:  ${CRT}

  They then run:

    shhterm create --name NAME \\
        --server ${LAN_IP}:${PORT} --ca lan.crt --verify-host ${LAN_IP}

    shhterm join <key> --name NAME \\
        --server ${LAN_IP}:${PORT} --ca lan.crt --verify-host ${LAN_IP}

  (Or put these in ~/.shhtermrc so they can just run 'shhterm create':
     server = ${LAN_IP}:${PORT}
     ca = /path/to/lan.crt
     verify_host = ${LAN_IP}
   )

  Notes:
    • Keep this machine awake and on the same network — it IS the server.
    • If prompted by your OS firewall, allow incoming connections on ${PORT}.
    • Press Ctrl-C here to stop the server.
────────────────────────────────────────────────────────────────────────

Starting server on 0.0.0.0:${PORT} …
EOF

exec "$SERVER_BIN" --address 0.0.0.0 --port "$PORT" --cert "$CRT" --key "$KEY"
