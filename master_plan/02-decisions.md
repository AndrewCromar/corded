# Decisions and Blueprint Gaps

Two lists. **Decisions** (D-nn) record what was chosen and why, including every place the
blueprint said "A or B". **Gaps** (G-nn) record where the blueprint is incomplete or
contradicts itself, how this plan resolves each one, and which stage owns it.

Status values: **Locked** (chosen by the project owner), **Proposed** (this plan's
recommendation, open to change before the owning stage starts), **Open** (needs a decision
before the owning stage starts).

To change a decision, edit this file in the same pull request as the change.

## Decisions

| ID | Topic | Decision | Status | Owning stage |
|---|---|---|---|---|
| D-01 | Repository | Public on GitHub, marked beta | Locked | done |
| D-02 | License | Apache-2.0 | Locked | done |
| D-03 | Package manager | vcpkg in manifest mode | Locked | 0 |
| D-04 | Group cryptography | Megolm-style sender keys | Locked | 4 |
| D-05 | Networking library | standalone asio, C++20 coroutines | Proposed | 2 |
| D-06 | Crypto library | libsodium for all end-to-end crypto | Proposed | 4 |
| D-07 | Transport security | TLS 1.3 plus signed challenge auth, with pinning | Proposed | 1 |
| D-08 | Wire serialization | Length-prefixed FlatBuffers frames | Proposed | 1 |
| D-09 | Event content encoding | JSON inside the encrypted payload | Proposed | 1 |
| D-10 | AEAD cipher | XChaCha20-Poly1305 only | Proposed | 4 |
| D-11 | Client vault encryption | SQLCipher with a wrapped random key | Proposed | 3 |
| D-12 | Database access layer | Thin hand-written wrapper, no `sqlite_orm` | Proposed | 2 |
| D-13 | Message model | Typed events with relations | Locked (owner requirement) | 1 |
| D-14 | Identity model | User identity key plus per-device keys | Proposed | 1 |
| D-15 | Multi-device | Designed in from the start, shipped after the core | Proposed | 1 |
| D-16 | Direct messages | Pairwise Double Ratchet; rooms of 3+ use sender keys | Proposed | 4 |
| D-17 | C ABI event delivery | Callback and pull queue, JSON payloads | Proposed | 5 |
| D-18 | TUI toolkit | FTXUI | Proposed | 5 |
| D-19 | Test framework | Catch2 v3, CTest, libFuzzer | Proposed | 0 |
| D-20 | Reference GUI toolkit | Flutter (dart:ffi) | Open | 6 |
| D-21 | Server retention | Ciphertext kept for a configurable window, default 30 days | Proposed | 2 |
| D-22 | Naming | `libcorded`, `cordedd`, `corded-tui`, `corded_` prefix | Proposed | 0 |
| D-23 | Ratchet implementation | Write X3DH, Double Ratchet and sender keys in C++ on libsodium, or wrap an audited library | Open | 4 |
| D-24 | Mobile push notifications | Needs its own design before any mobile release | Open | 6 |
| D-25 | Multi-device in 1.0 | Recommended yes (feature milestone F7 before 1.0) | Open | 7 |

### D-03 vcpkg in manifest mode
One `vcpkg.json` with a pinned `builtin-baseline`, consumed through the CMake toolchain
file. Every dependency in the blueprint has a port. Static triplets
(`x64-linux`, `arm64-linux`, `x64-windows-static`, `arm64-osx`, `x64-osx`) give the single
static server binary the blueprint asks for. Conan was the alternative; it offers finer
cross-compilation control at the cost of a Python toolchain on every machine.

### D-04 Megolm-style sender keys
Each sending device keeps one outbound session per room: a symmetric chain key that
ratchets forward on every message, and a signing key. The session is shared with every
member device over that device's pairwise Double Ratchet session, so a room message is
encrypted once and fanned out by the server. Sessions rotate when a member leaves or is
removed, after 100 messages, or after 7 days.

Trade-off accepted: no post-compromise security inside a session lifetime, and a removed
member can read until the next rotation takes effect. MLS (RFC 9420) fixes both and scales
better, but is a far larger build. The engine hides group crypto behind an interface so
MLS can be added later as a second implementation.

### D-05 asio over uWebSockets
asio gives raw TCP, timers, TLS and coroutine support in one library, and has no opinion
about HTTP. uWebSockets would add a WebSocket and HTTP layer the protocol does not need.
If browser clients are ever wanted, a WebSocket listener can be added behind the same
frame codec.

### D-06 libsodium over OpenSSL for end-to-end crypto
libsodium's API is hard to misuse, is constant-time throughout, and covers everything the
protocol needs (X25519, Ed25519, XChaCha20-Poly1305, HKDF-SHA256 from 1.0.19, Argon2id,
locked memory). OpenSSL is still linked, but only for TLS and as SQLCipher's backend.

### D-07 TLS 1.3 plus signed challenge
The blueprint specifies no transport security at all. Proposal: TLS 1.3 only, through
`asio::ssl`. After the TLS handshake, the client proves control of its device key by
signing a server challenge bound to the TLS channel (exporter value), so the server never
holds a password or reusable token.

Self-hosters without a public certificate use a self-signed certificate. Its SPKI
fingerprint is carried in the server's invite link and pinned by the client.

Alternative considered: a Noise handshake (Noise_XK) over raw TCP using only libsodium.
It removes the OpenSSL dependency from the server and authenticates both sides in one
step. Rejected for now because it means hand-writing a handshake state machine, and it is
blocked by more firewalls and proxies than TLS on port 443. Worth revisiting in Stage 7.

### D-08 FlatBuffers on the wire
Frames are a 4-byte little-endian length followed by a FlatBuffers `Frame` table. The
schema lives in `proto/` and is the contract. FlatBuffers gives forward and backward
compatible schema evolution, zero-copy reads on the server, and generated code for other
languages. Every buffer from the network goes through the FlatBuffers verifier before any
field is read. Alternative: a hand-rolled binary format. Smaller dependency, but every
field addition becomes a manual compatibility exercise.

### D-09 JSON event content
Inside the encrypted payload, an event's `content` is UTF-8 JSON. The server never parses
it, so its cost is paid only on clients. JSON makes unknown event types trivially
storable and forwardable, lets frontends and bots define their own event types without
recompiling the core, and matches the blueprint's JSON callback payloads. The envelope has
a `content_encoding` field so a binary encoding can be introduced later.

### D-10 XChaCha20-Poly1305 only
The blueprint offers AES-256-GCM or ChaCha20-Poly1305 with an "AES-NI fallback". That is
backwards: AES-GCM is the one that needs hardware support to be fast and constant-time,
and libsodium refuses to provide it without. Raspberry Pi 4 has no AES instructions. One
cipher everywhere removes negotiation and a class of downgrade bugs. The 192-bit nonce of
the X variant makes random nonces safe where a counter is not available.

### D-11 SQLCipher with a wrapped key
A random 256-bit vault key encrypts the database (SQLCipher raw-key mode). That key is
stored wrapped by a key derived from the passphrase with Argon2id. Changing the passphrase
re-wraps 32 bytes instead of re-encrypting the database, and it leaves room for OS
keychain or hardware-key unlock later.

### D-12 No `sqlite_orm`
`sqlite_orm` targets stock SQLite and its compile-time schema does not combine cleanly
with SQLCipher or with numbered SQL migrations. Both binaries use a small RAII wrapper
(`Database`, `Statement`, `Transaction`) over the C API, with schema defined in plain
`.sql` migration files that can be reviewed and diffed.

### D-13 Typed events with relations
See [00-overview.md](00-overview.md) section 3 and
[stage-1-protocol-spec.md](stages/stage-1-protocol-spec.md) step 1.6. This is the owner's
requirement that threads, replies and later features fit without redesign.

### D-14 and D-15 Identity and devices
A user is identified by a long-term Ed25519 **user identity key**; its public half,
base64url encoded, is the `user_id` (as in the blueprint). Each device has its own Ed25519
**device signing key** and X25519 **device DH key**, certified by the user identity key.
Sessions are between devices, never between users.

Every table and frame carries a `device_id` from day one. The first releases support one
device per user. Linking a second device is a feature milestone, not a redesign.

### D-16 Direct messages versus rooms
A two-person conversation encrypts each event separately to each recipient device with
Double Ratchet, which gives forward secrecy and post-compromise security per message.
Rooms with three or more members use sender keys (D-04). Both paths carry the same
`Event`, so nothing above the crypto layer knows the difference.

### D-17 C ABI event delivery
The blueprint has a single callback taking a JSON string. Kept, with additions: a pull
function (`corded_next_event`) for runtimes that dislike callbacks from foreign threads,
numeric error codes on every call, request ids to match results to commands, and an ABI
version function. Details in [stage-5-c-abi-and-tui.md](stages/stage-5-c-abi-and-tui.md).

### D-20 Reference GUI (open)
The blueprint lists Qt and Flutter. Recommendation is Flutter, because one codebase covers
Linux, macOS, Windows, Android and iOS through `dart:ffi`, which exercises the C ABI on
mobile. Qt 6 is the alternative if desktop polish matters more than mobile reach. To be
decided before Stage 6.

### D-21 Server retention
The server keeps room ciphertext for a configurable window (default 30 days) so offline
devices can catch up and clients can page back through history. Per-device messages (key
shares, direct messages) are deleted as soon as the device acknowledges them.

### D-23 Ratchet implementation (open)
The plan as written follows the blueprint: implement X3DH, Double Ratchet and the group
ratchet in C++ on top of libsodium primitives. That keeps a pure C++ build and full
control, and it is the single largest security risk in the project, because protocol
code written from a specification is easy to get subtly wrong.

The alternative is to link an existing audited implementation. vodozemac (Rust, the
successor to libolm, used by Matrix) implements exactly this pair of protocols and has
been audited. Using it would add a Rust toolchain to the build and reduce Stage 4 to
integration, persistence, key distribution and verification.

To be decided by the owner before Stage 4 starts. Stage 4's `CryptoProvider` interface
is the same either way, so the choice does not affect Stages 2, 3 or 5.

## Gaps and contradictions in the blueprint

| ID | Gap | Resolution | Owning stage |
|---|---|---|---|
| G-01 | "3DH" handshake has no signed prekey or one-time prekeys, so it cannot work for offline recipients and has weak forward secrecy | Use X3DH: identity key, signed prekey, one-time prekeys | 1 (spec), 4 (code) |
| G-02 | Server schema has no tables for prekeys, memberships, invites, devices or delivery state, though section 3 requires all of them | Full schema in Stage 2 | 2 |
| G-03 | `ratchet_states` lacks counters, skipped-message keys, the local DH private key and the previous chain length | Complete session state in Stage 4 | 4 |
| G-04 | "No metadata beyond routing tokens" contradicts `messages_store.sender_id` and server-side membership | State the real metadata exposure in the threat model; sealed sender as future work | 1 |
| G-05 | No transport security or authentication handshake is specified | D-07 | 1 |
| G-06 | `engine_connect_server(..., auth_key)` and `servers.auth_token` imply bearer tokens | Replace with challenge signing by the device key; no stored token | 1, 3 |
| G-07 | Passphrase passed as a NUL-terminated `const char*` | Pass pointer and length; copy into locked memory; zero after use | 5 |
| G-08 | C API has no error codes, no versioning, no threading contract, no way to match results to requests | D-17 | 5 |
| G-09 | A message is a single plaintext string in both the API and the vault schema | D-13 | 1, 3, 5 |
| G-10 | `sqlite_orm` with SQLCipher | D-12 | 2, 3 |
| G-11 | "AES-256-GCM with AES-NI fallback" | D-10 | 4 |
| G-12 | Single static server binary versus an OpenSSL dependency | Static-link OpenSSL through vcpkg static triplets; measure size and memory in Stage 2 | 2 |
| G-13 | Multi-device is never mentioned | D-14, D-15 | 1 |
| G-14 | No way for users to verify each other's keys | Safety numbers and trust-on-first-use with key-change warnings | 4 |
| G-15 | Attachments are never mentioned | Encrypted blob store on the server, keys carried in events | 1 (spec), 2 (store), feature milestone |
| G-16 | Group section names both Megolm and TreeKEM, which are different designs | D-04 | 4 |
| G-17 | Message ordering and history pagination are not defined | Server assigns a per-room sequence number; sync by cursor | 1, 2 |
| G-18 | "Lock-free event queues" to frontends, but the API is a callback | One event queue inside the core, drained by a dispatch thread or by the pull function | 5 |
| G-19 | No account recovery or key backup story | Out of scope for 1.0, documented as a known limitation; encrypted backup export is a feature milestone | 7 (docs) |
| G-20 | No abuse handling beyond rate limits (spam, illegal content reports) in an E2EE system | Server-level controls (invite-only registration, bans, quotas) in Stage 2; client-side reporting as a later feature | 2 |
| G-21 | Typing indicators, presence and read receipts need non-stored delivery | Ephemeral frame class that is routed but never written to disk | 1, 2 |
| G-22 | The blueprint starts with sockets before defining a protocol | Stage 1 exists to write the protocol down first | 1 |
