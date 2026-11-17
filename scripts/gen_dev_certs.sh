#!/usr/bin/env bash
#
# Generate a self-signed certificate for LOCAL DEVELOPMENT ONLY.
#
# The client verifies the server certificate. For local testing you therefore
# need (a) a cert/key for the server, and (b) to tell the client to trust that
# cert. This script produces both files under ./certs by default.
#
# Usage:
#   scripts/gen_dev_certs.sh [output_dir] [hostname]
#
# Then run:
#   ./build/shhterm-server --cert certs/dev.crt --key certs/dev.key
#   ./build/shhterm create --name Ahmad --server localhost:8443 \
#       --ca certs/dev.crt --verify-host localhost
#
# Do NOT use these certificates in production. For a real deployment use a CA
# that clients already trust (e.g. Let's Encrypt) — see the README.

set -euo pipefail

OUT_DIR="${1:-certs}"
HOSTNAME="${2:-localhost}"

mkdir -p "$OUT_DIR"

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$OUT_DIR/dev.key" \
  -out "$OUT_DIR/dev.crt" \
  -days 365 \
  -subj "/CN=$HOSTNAME" \
  -addext "subjectAltName=DNS:$HOSTNAME,DNS:localhost,IP:127.0.0.1"

chmod 600 "$OUT_DIR/dev.key"

echo "Wrote:"
echo "  $OUT_DIR/dev.crt  (server certificate; also pass to the client via --ca)"
echo "  $OUT_DIR/dev.key  (server private key; keep secret)"
echo
echo "Certificate SANs: DNS:$HOSTNAME, DNS:localhost, IP:127.0.0.1"
