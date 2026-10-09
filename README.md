# Corded

A modular, end-to-end-encrypted messaging platform: one headless C++20 core, a tiny
self-hostable relay server, and any number of frontends built on top.

> [!WARNING]
> **Corded is in early beta (pre-alpha).** What exists is a working prototype and a plan.
> Nothing here has been security audited, and the encryption code was written quickly and
> has had no independent review. Do not use Corded to protect anything that matters until
> a 1.0 release says otherwise.

## The idea

Corded separates mechanism from policy, the way an operating system kernel does.

- **The core ("kernel")** is a headless C++20 engine. It owns the network connection, the
  wire protocol, the cryptographic ratchets and the encrypted local vault. It has no UI and
  exposes a plain C ABI.
- **Frontends ("distros")** are separate applications (terminal, desktop, mobile, bots) that
  load the core as a shared library. They render state and send user actions. They never
  touch key material or sockets.
- **The server** is an untrusted relay. It routes and stores opaque ciphertext, enforces
  membership, and is meant to be small enough to run on a Raspberry Pi or a $3 VPS.

## Status

| Area | State |
|---|---|
| Design blueprint | Done ([PDF](master_plan/Modular%20E2EE%20Messaging%20Platform%20-%20Technical%20Architecture%20Blueprint.pdf)) |
| Implementation plan | Done ([master_plan/](master_plan/00-overview.md)) |
| Prototype | Working on Linux: server, core library, terminal client. One-to-one and group chats, end-to-end encrypted |
| Everything else in the plan | Not started |

## Try the prototype

You need Linux, a C++20 compiler (GCC 13+ or Clang 17+), CMake 3.25+, git, Python 3 and
the libsodium development package (1.0.19 or newer).

```sh
git clone https://github.com/AndrewCromar/corded.git
cd corded
./build.sh          # first run builds the dependencies and takes a while
```

Then open three terminals in the `corded` directory.

**Terminal 1, the server:**

```sh
./build/dev/bin/cordedd --port 7443 --data ./server-data
```

**Terminal 2, Alice:**

```sh
./build/dev/bin/corded-tui --vault ./alice-vault --server localhost:7443 --name alice
```

**Terminal 3, Bob:**

```sh
./build/dev/bin/corded-tui --vault ./bob-vault --server localhost:7443 --name bob
```

The server prints a line like `server fingerprint: C8sJ...` when it starts. That is its
identity. A client remembers the fingerprint it sees the first time it connects and
refuses to connect if it ever changes. To be strict from the first connection, pass it
to the client with `--fingerprint <value>`.

Each client asks you to choose a passphrase the first time (at least 8 characters, typed
twice). Once both say `live`, type this in Alice's window:

```
/chat bob
```

and start typing. Things to try:

| Type | What it does |
|---|---|
| any text | Sends a message to the open chat |
| `/chat <username>` | Starts or opens a chat with someone on the same server |
| `/edit <text>` | Changes your last message |
| `/delete` | Removes your last message (others' clients erase their copy; it cannot force them to) |
| `/group bob carol : Weekend plans` | Starts a group chat; the part after the colon is an optional name |
| `/name <text>` | Renames the open chat |
| `/verify` | Shows a safety number for each person in the chat, to compare with them out of band |
| `/verified <username>` | Marks someone as checked after comparing numbers |
| `/reply <text>` | Replies to the last message you received |
| `/thread <text>` | Replies in a thread under the last message you received |
| `/react <emoji or text>` | Reacts to the last message you received |
| `/help` | Shows the command list |
| `/quit` | Leaves |
| Tab | Switches between the chat list and the message box |

By default anyone who can reach the server can create an account. To restrict that,
start it with `--invite-code <code>` (at least 8 characters) and give the code to the
people you want; they pass `--invite <code>` to `corded-tui` the first time. `--closed`
stops new accounts entirely while existing users keep working.

To talk between two computers, run the server on one, and point the other's `--server`
at that machine's address. Quit a client and start it again with the same `--vault` to
see it unlock, restore history and pick up anything it missed.

To run the tests:

```sh
ctest --preset dev
```

## What the prototype does and does not do

It does:

- Real end-to-end encryption: X3DH key agreement and the Double Ratchet, built on
  libsodium. The server stores only ciphertext, and a test checks that.
- Group chats. Each message is encrypted separately for every member, and room names are
  encrypted too, so the server never learns them.
- An encrypted local vault. Keys and message history are stored in an encrypted SQLite
  database unlocked by your passphrase (Argon2id).
- Store-and-forward. Messages sent while the other person is offline arrive when they
  return, in order. Clients reconnect on their own, and queued messages survive restarts.
- An encrypted connection to the server: TLS 1.3, with the server's self-signed key
  pinned by the client, and sign-in bound to that TLS session.
- Threads: replies that hang under the message that started them.
- Editing and deleting your own messages.
- Server protections: invite-only or closed registration, a per-connection rate limit
  that slows down floods, and a cap on connections per address.
- Safety numbers, so two people can check that nobody is sitting between them.
- Typed events with relations instead of plain strings. Replies and reactions already use
  this, and it is what threads, edits and the rest will be built on.
- A frontend that uses only the public header, [`corded.h`](include/corded/corded.h).

It does not, yet:

- Change who is in a group after it is created, or leave one.
- Warn you when a contact's key changes. You can compare safety numbers with `/verify`,
  but a contact whose key later changes is not yet flagged; their messages just fail to
  decrypt.
- More than one device per user, or more than one server per vault.
- Run anywhere but Linux.

## Where to look

- [Overview](master_plan/00-overview.md): architecture, trust model, tech stack, stage map
- [Roadmap](master_plan/01-roadmap.md): every stage broken into steps
- [Decisions](master_plan/02-decisions.md): choices made and gaps found in the blueprint
- [Feature roadmap](master_plan/03-feature-roadmap.md): threads, replies, reactions and the rest
- [Stage plans](master_plan/stages/): the detailed plan for each stage

Code layout:

| Path | What |
|---|---|
| `proto/corded.fbs` | The wire protocol schema |
| `server/` | `cordedd`, the relay server |
| `core/` | `libcorded`: crypto, vault, engine and the C ABI shim |
| `include/corded/corded.h` | The public C interface |
| `frontends/tui/` | `corded-tui`, the terminal client |
| `tools/corded-cli.cpp` | A headless client that speaks JSON lines, for scripts and debugging |
| `tests/` | Crypto unit tests, end-to-end tests, a TUI smoke test |

## License

Apache-2.0. See [LICENSE](LICENSE).
