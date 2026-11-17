# Security Policy

## Reporting a vulnerability

If you find a security issue, **please do not open a public issue.** Instead,
report it privately — e.g. via GitHub's "Report a vulnerability" (Security
Advisories) on this repository, or by contacting the maintainer directly. We'll
acknowledge the report and work on a fix before any public disclosure.

## What shhterm protects (and what it does not)

shhterm aims to be honest about its guarantees:

- **End-to-end encryption of message bodies.** Messages are encrypted with
  AES-256-GCM using a key derived (HKDF-SHA256) from the *secret half* of the
  room key, which never leaves the clients. The server routes only ciphertext
  and cannot read message contents.
- **TLS transport.** Connections to the server are TLS-protected; the terminal
  and browser clients verify the server certificate.
- **No persistence.** Rooms and messages live only in server memory and are
  never written to disk or logged. Empty rooms are deleted after 5 minutes.

### Out of scope / known limitations

- **Metadata is not hidden.** The server necessarily sees usernames, room
  membership, and message size/timing. E2E protects *contents*, not *metadata*.
- **The room key is the only credential.** Anyone with the full key can join and
  decrypt. Share it over a channel you trust.
- **No forward secrecy / key rotation** within a room: one room key is used for
  the room's lifetime.
- **Direct messages (`/msg`)** are routed to one recipient but use the shared
  room key, so they are private from the server, not cryptographically private
  from other room members.
- **Trust in the server operator for availability and metadata.** Self-hosting
  gives you control over who runs the server.

If you believe any of these guarantees is not met by the implementation, that is
a security bug — please report it.
