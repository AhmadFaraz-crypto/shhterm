# Privacy Policy

_Last updated: 2026-11-17_

shhterm is built to collect as little as possible. There are no accounts, no
email, no passwords, no cookies, and no analytics or tracking.

This policy covers two things: the **public community server** (the default
server the apps connect to) and the **shhterm website** (the docs/landing
pages). If you run your own server, **you** are its operator — this policy
describes how the reference software behaves, but the data practices of a
self-hosted server are your responsibility.

## What the server does and does not have

**Message contents are end-to-end encrypted.** The server only ever receives
ciphertext and the *routing* half of a room key — never the secret half needed
to decrypt. It therefore **cannot read your messages.**

**Nothing is stored.** Rooms and messages exist only in the server's memory for
the moment they are relayed. They are:

- never written to disk,
- never logged (no message contents, no room keys),
- not kept as history — someone who joins later cannot see earlier messages.

Empty rooms are automatically deleted after five minutes.

### What the server necessarily processes (transiently, in memory)

To route messages between people in a room, the server unavoidably handles some
data while a connection is open:

- **Network connection information** such as your IP address, as any internet
  server must, to send data back to you.
- **The display name** you choose and **which room** you are in (so it can
  deliver messages and show who's present).
- **Encrypted message payloads**, plus their **size and timing**, which it
  relays but cannot decrypt.

This is **metadata**, and end-to-end encryption does not hide it from the server
operator. It is held only in memory for the life of the connection and is not
persisted or logged. The community server does not use it for any purpose other
than routing your chat.

## The website (docs & landing pages)

The website is hosted on **GitHub Pages**. It uses no analytics, no cookies, and
no third-party trackers. As with any website, GitHub may process standard
request information (e.g. IP addresses) to serve the pages; see
[GitHub's Privacy Statement](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement).

## Your control

- The **room key is the only credential** — anyone you give it to can join and
  decrypt. Share it only with people you trust.
- For a server whose data practices you fully control, **host your own** (see
  [DEPLOYMENT.md](DEPLOYMENT.md)). When you do, you decide who can reach it.

## No guarantees

shhterm is a free, open-source hobby/educational project provided "as is",
without warranty, and without a formal security or privacy audit. Don't rely on
it for high-stakes confidentiality. See [SECURITY.md](SECURITY.md) for the
security model and how to report an issue.

## Changes

This policy may change as the project evolves; the "last updated" date above
reflects the latest revision. Material changes will be noted in the repository.

## Contact

Questions? Open an issue on the
[GitHub repository](https://github.com/AhmadFaraz-crypto/shhterm), or for
security-sensitive matters follow [SECURITY.md](SECURITY.md).
