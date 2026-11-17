#!/usr/bin/env bash
#
# Build shhterm on a Debian/Ubuntu machine (incl. WSL). Installs the required
# packages, then builds the client and server.
#
# Optionally bake in a default server address so users can run `shhterm create`
# without --server:
#
#   SHHTERM_SERVER=chat.example.com:8443 ./BUILD_ON_UBUNTU.sh
#
set -euo pipefail

echo "==> Installing build dependencies (needs sudo)…"
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build libboost-dev libssl-dev git

CMAKE_ARGS=(-S . -B build -G Ninja)
if [[ -n "${SHHTERM_SERVER:-}" ]]; then
  echo "==> Baking in default server: ${SHHTERM_SERVER}"
  CMAKE_ARGS+=("-DSHHTERM_DEFAULT_SERVER=${SHHTERM_SERVER}")
fi

echo "==> Configuring…"
cmake "${CMAKE_ARGS[@]}"

echo "==> Building…"
cmake --build build

cat <<'EOF'

================================================================
  Built:
    ./build/shhterm          (client)
    ./build/shhterm-server   (server)
    ./build/shhterm_tests    (tests)

  Run the tests:
    ./build/shhterm_tests

  Start a local server + chat (see README for certificate setup):
    scripts/gen_dev_certs.sh
    ./build/shhterm-server --cert certs/dev.crt --key certs/dev.key
    ./build/shhterm create --name Me \
        --server localhost:8443 --ca certs/dev.crt --verify-host localhost
================================================================
EOF
