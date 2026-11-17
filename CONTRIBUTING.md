# Contributing to shhterm

Thanks for your interest in improving shhterm! Contributions of all kinds are
welcome — bug reports, features, docs, and tests.

## Getting set up

See the [README](README.md#dependencies) for platform-specific dependencies.
In short (Linux):

```bash
sudo apt-get install -y build-essential cmake ninja-build libboost-dev libssl-dev git
git clone https://github.com/AhmadFaraz-crypto/shhterm && cd shhterm
cmake -S . -B build -G Ninja
cmake --build build
```

## Running the tests

```bash
./build/shhterm_tests          # or: ctest --test-dir build --output-on-failure
```

Please make sure the full test suite passes before opening a pull request. If
you change the protocol or crypto, also run the browser interop check:

```bash
node tools/interop_test.mjs
```

## Project layout

| Directory | What it holds |
|---|---|
| `proto/`  | Shared wire protocol: framing, JSON messages, key generation |
| `server/` | TLS server: sessions, rooms, lifecycle, rate limiting |
| `client/` | Terminal client: networking, reconnection, FTXUI UI, E2E crypto |
| `web/`    | Browser client + Node WebSocket↔TLS gateway |
| `tests/`  | doctest suite (unit + real-TLS end-to-end) |
| `tools/`  | Crypto interop helpers |

## Guidelines

- **Match the surrounding style** — the code uses a consistent, commented C++
  style; please follow it. (`clang-format` with a Google-ish profile is a good
  approximation.)
- **Add tests** for new behavior, especially around protocol validation, room
  access/routing, expiry, and crypto.
- **Keep the security model intact.** Message bodies are end-to-end encrypted;
  the server must never need plaintext. If a change touches crypto or the
  split-key handling, call it out explicitly in the PR and keep the C++ and
  JavaScript implementations in sync (verified by `tools/interop_test.mjs`).
- **Don't commit secrets.** Certificates and keys are git-ignored; never add
  real certs, keys, or server addresses you don't intend to publish.

## Reporting bugs

Open an issue with: what you did, what you expected, what happened, your OS, and
the command you ran. For anything security-sensitive, see
[SECURITY.md](SECURITY.md) instead of filing a public issue.

## License

By contributing, you agree that your contributions are licensed under the
project's [MIT License](LICENSE).
