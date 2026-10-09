# Corded

A modular, end-to-end-encrypted messaging platform: one headless C++20 core, a tiny
self-hostable relay server, and any number of frontends built on top.

> [!WARNING]
> **Corded is in early beta (pre-alpha).** The repository currently contains the design
> and implementation plan, not a working product. Nothing here has been security audited.
> Do not use Corded to protect anything that matters until a 1.0 release says otherwise.

## The idea

Corded separates mechanism from policy, the way an operating system kernel does.

- **The core ("kernel")** is a headless C++20 engine. It owns the network connection, the
  wire protocol, the cryptographic ratchets and the encrypted local vault. It has no UI and
  exposes a plain C ABI.
- **Frontends ("distros")** are separate applications (terminal, desktop, mobile, bots) that
  load the core as a shared library. They render state and send user actions. They never
  touch key material or sockets.
- **The server** is an untrusted relay. It routes and stores opaque ciphertext, enforces
  membership and rate limits, and is small enough to run on a Raspberry Pi or a $3 VPS.

## Status

| Area | State |
|---|---|
| Design blueprint | Done ([PDF](master_plan/Modular%20E2EE%20Messaging%20Platform%20-%20Technical%20Architecture%20Blueprint.pdf)) |
| Implementation plan | Done, under review ([master_plan/](master_plan/00-overview.md)) |
| Code | Not started |

## Where to look

- [Overview](master_plan/00-overview.md): architecture, trust model, tech stack, stage map
- [Roadmap](master_plan/01-roadmap.md): every stage broken into steps
- [Decisions](master_plan/02-decisions.md): choices made and gaps found in the blueprint
- [Feature roadmap](master_plan/03-feature-roadmap.md): threads, replies, reactions and the rest
- [Stage plans](master_plan/stages/): the detailed plan for each stage

## License

Apache-2.0. See [LICENSE](LICENSE).
