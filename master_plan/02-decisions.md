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
| D-20 | Reference GUI toolkit | Flutter (dart:ffi). The community is free to build other clients on the same C interface, using platform-specific tools where they are faster | Locked (owner decision) | 6 |
| D-21 | Server retention | Ciphertext kept for a configurable window, default 30 days | Proposed | 2 |
| D-22 | Naming | `libcorded`, `cordedd`, `corded-tui`, `corded_` prefix | Proposed | 0 |
| D-23 | Ratchet implementation | Write X3DH, Double Ratchet and sender keys in C++ on libsodium, or wrap an audited library | Open | 4 |
| D-24 | Mobile push notifications | Needs its own design before any mobile release | Open | 6 |
| D-25 | Multi-device in 1.0 | Recommended yes (feature milestone F7 before 1.0) | Open | 7 |
| D-26 | Server administrator | Superseded by D-27: the administrator is now the server's owner, and other powers come from roles | Superseded | 2 |
| D-27 | Community model | One server is one Discord-style community: channels, one owner, roles with permissions. Two communities means two servers. See [04-community-model.md](04-community-model.md) | Locked (owner requirement) | 1, 2, 3, 5 |
| D-28 | History for new members | A newcomer's client asks for old messages. If the server allows it, the request is passed to existing members' clients, which hand the messages over unless that member has turned sharing off. So a newcomer may or may not get history, depending on the server's and the members' settings | Locked (owner decision) | 2, 3 |
| D-34 | Server settings from the client | Every server setting is stored in the server and can be read and changed from the owner's client; nothing needs the server's command line after first setup | Locked (owner requirement) | 2, 5 |
| D-35 | Maintenance | The server does its own housekeeping, can restart itself on a schedule, and can be restarted from the owner's client | Locked (owner requirement) | 2 |
| D-36 | Remote update | The owner can update the server from their client. Only official releases verified by signature are installed, never an older one, with rollback on failure | Locked (owner requirement); who holds the signing key is open | 2, 7 |
| D-37 | Several devices per person | The same identity on several devices, added by linking or by a recovery phrase. The secret that makes two clients the same person is the identity key, which the server never sees | Locked (owner requirement); builds on D-15 | 1, 2, 3, 4 |
| D-38 | Client platforms | Linux, Windows, macOS, iOS and Android from one GUI codebase, built on GitHub's machines so no Mac is needed. Windows first | Locked (owner requirement) | 6 |
| D-39 | Browser client | Not being pursued for now (owner decision). Recorded as possible later, by compiling the core to WebAssembly and adding a WebSocket listener to the server | Deferred | none |
| D-40 | Isolated home hosting | A server at home must be able to run walled off from the home network, without relying on the server being secure: the Pi on a guest network or its own router, a setup script that applies a firewall and an unprivileged account, and an outgoing-tunnel mode so the home router accepts no incoming connections | Locked (owner requirement) | 2, 7 |
| D-41 | GUI architecture | A Flutter app over a Dart binding (`corded_dart`) over the existing C interface; the app holds no message store, does no cryptography and opens no sockets. See 06 | Proposed | 6 |
| D-42 | Android before desktop | The first graphical build to reach the owner is an Android APK; the Linux desktop build exists as the development vehicle | Locked (owner request, 2026-10-09) | 6 |
| D-43 | Clients made by others | The C interface and its JSON commands and events are a documented, versioned contract; a prebuilt `libcorded` and header ship with every release so a client needs no C++ build | Proposed (owner wants community clients) | 6 |
| D-44 | Typing and read receipts | Typing notices are relayed by the server unencrypted and never stored, until sender keys make encrypting them cheap. Read receipts are encrypted events shared with the room by default, with a per-client off switch | Built; owner asked for the data to exist for GUIs | 5 |
| D-45 | Files and photos | A file is encrypted on the sender's device under a key made for it alone (XChaCha20-Poly1305, bound to a random id), uploaded in 256 KB pieces, and announced by an `m.file` message that carries the key, name, type, size and, for a picture, a small preview. One event type for every kind of file; the type inside says what it is. The server sees only who uploaded how many bytes. Limits are server settings (`max_file_mb`, `storage_limit_mb`); files follow `retention_days`; deleting the message removes the bytes. A fetched file is kept decrypted in a `files` folder beside the vault (app-private storage on Android) | Built; the plaintext cache is a trade-off to revisit (encrypt it, or clear it on lock) | F4 |
| D-33 | Server scope | The person running a server chooses who can reach it: this machine only, the local network, or the internet. The safe choice is the default, and the wider scopes switch on the protections they need | Locked (owner requirement); details proposed | 2 |
| D-32 | Turning end-to-end encryption off | Always on by default; can be disabled if wanted. Proposed shape: a per-server switch held by the owner, shown plainly in every client | Locked in principle (owner decision); details proposed | 2, 3, 5 |
| D-29 | What the server sees of a community | Channel names, role names and the member list are visible to the server; message content is not | Proposed | 1 |
| D-30 | Sender keys timing | Needed before communities grow, because channels have many members; moves earlier than D-04 implied | Proposed | 4 |
| D-31 | Direct messages | Within one server only; cross-server direct messages would need federation | Proposed | 1 |

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

### D-27 Community model
Requirement from the owner, given after D-26: each server is meant to be one Discord-type
server. It has channels; one user is the admin who owns the server; they can make roles
and give permissions; someone who wants to own two servers sets up a second server.

The full design, its consequences for encryption, and the step-by-step plan for changing
the existing code are in [04-community-model.md](04-community-model.md). Decisions D-28 to
D-31 record the choices that design raises.

### D-26 Server administrator (superseded by D-27)
The text below is kept for history. Under D-27 the "administrator" is the server's owner,
and anything short of that is granted through roles.

Requirement from the owner: when someone sets up a server, they must be able to assign
their own client as an administrator that has all permissions.

How it works:

- **Becoming the administrator.** The operator names the administrator when starting the
  server (`cordedd --admin <username>`, later also `cordedd init` and the configuration
  file). Whoever holds that username is the administrator. If nobody has registered it
  yet, the name is reserved and the first registration of it becomes the administrator;
  on an invite-only server that still needs the invite code. Several administrators can
  be named. Naming is done on the server's own command line or configuration, so only
  someone who controls the server machine can grant it.
- **What "all permissions" means.** An administrator can do anything any role on the
  server can do, in every room they are a member of and across the server: remove members
  from rooms, add members, rename rooms, delete rooms, ban and unban users, create and
  revoke invite codes, close or open registration, and grant or remove administrator
  status for others.
- **What it does not mean.** An administrator cannot read messages in rooms they are not
  a member of, and cannot decrypt anything they were not sent. That is a property of the
  end-to-end encryption, not a permission, and no server role can override it. An
  administrator who joins a room is visible to its members like anyone else.
- **How clients learn of it.** The server tells a client at sign-in whether its account
  is an administrator, and marks administrators in room member lists, so frontends can
  show admin controls and other members can see who has that power.
- **Relation to room roles.** Room-level roles (owner, admin, member) from the Stage 1
  rooms specification still exist. A server administrator outranks all of them.

Stage 2 owns the server side (steps 2.1, 2.5, 2.7, 2.11 and 2.12); Stage 5 exposes the
admin commands through the C ABI and the TUI.

## Prototype notes

A thin prototype (server, core, TUI; Linux only) was built ahead of the staged plan. It
follows the decisions above except where noted here. Each item needs to be settled
properly when its owning stage is reached.

| Topic | What the prototype does | Why | Revisit in |
|---|---|---|---|
| D-11 vault encryption | SQLite3 Multiple Ciphers (ChaCha20-Poly1305) instead of SQLCipher | vcpkg's SQLCipher port only supports Windows | Stage 3 |
| D-03 libsodium source | System libsodium by default; vcpkg copy behind the `vendored-sodium` manifest feature | The vcpkg port needs `autoconf-archive` installed on the host | Stage 0 |
| D-07 transport security | TLS 1.3 with a self-signed server certificate, pinned by clients on first use or from a fingerprint given up front; sign-in signature covers a TLS exporter value. Tag `prototype-v0` predates this and uses plain TCP | CA-validated certificates and invite links carrying the fingerprint are not built yet | Stage 2 |
| D-16 group rooms | Groups work, but by encrypting each message separately for every member (pairwise Double Ratchet) instead of sender keys. Membership is fixed at creation. Tag `prototype-v0` has two-person rooms only | Stronger per-message security and no new crypto, at the cost of message size growing with the group; sender keys become worthwhile once groups are large or membership can change | Stage 4 |
| D-15 devices | One device per user | As planned for first releases | F7 |
| D-27 community model | Steps C1 to C4 of [04-community-model.md](04-community-model.md) are built: `cordedd --owner <username>`, roles and permissions, channels, private and read-only channels, kick and ban, and the TUI commands to run it. Not yet: several servers per vault, invite links, moderator deletion, sender keys, categories and the rest of C5 to C10 | In progress | Stages 2, 3, 5 |
| Stage 1 | Wire schema written directly as `proto/corded.fbs`, no prose specification | Prototype shortcut | Stage 1 |
| Threading | One engine thread does both networking and engine work | Simpler; the two-thread split is still the plan | Stage 3 |
| Servers per vault | One | Prototype shortcut | Stage 3 |
| X3DH first message | Carried inside the first room event rather than as a separate to-device message | Avoids a second message path | Stage 4 |

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
