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
- **The server** is one community, the way a Discord server is: it has channels, an owner,
  and roles with permissions. If you want two communities, you run two servers. It routes
  and stores ciphertext it cannot read, and is meant to be small enough to run on a
  Raspberry Pi or a $3 VPS.

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

**Terminal 1, the server.** Name the community and say who owns it:

```sh
./build/dev/bin/cordedd --port 7443 --data ./server-data --name "My Server" --owner alice
```

A new server only accepts connections from the computer it runs on. To let other
machines in, say how far it should reach:

| | Who can connect |
|---|---|
| `--scope machine` (default) | Only this computer |
| `--scope network` | Devices on your local network; anything else is turned away |
| `--scope internet` | Anyone. New accounts then need an invite unless you add `--open-registration` |

Settings given on the command line are remembered, so later starts only need `--data`
(and `--port` if you changed it).

**Terminal 2, Alice (the owner):**

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
twice). Once both say `live` they are both in `#general`, so just start typing.

Talking:

| Type | What it does |
|---|---|
| any text | Sends a message to the open channel or chat |
| `/open <name>` | Opens a channel or chat whose name contains the text |
| `/chat <username>` | Starts or opens a direct message with another member |
| `/reply 12 <text>` | Replies to message 12. Every message shows a number; leave it out to mean the last one you received |
| `/thread 12 <text>` | Replies in a thread under that message |
| `/react 12 <emoji or text>` | Reacts to that message |
| `/edit 12 <text>` | Changes one of your own messages (no number: your last one) |
| `/delete 12` | Removes one of your messages (others' clients erase their copy; it cannot force them to) |
| `/servers` | Lists the servers you are in |
| `/server switch <name or number>` | Shows another server's channels |
| `/server join <invite link or host:port>` | Joins another server without leaving this one |
| `/once 30s <text>` | Sends one message that disappears after the given time (`30s`, `5m`, `2h`, `1d`) |
| `/disappear 1h` | Makes every new message in the open chat disappear after that long; `/disappear off` stops it |
| `/group bob carol : Weekend plans` | Starts a private group chat; the part after the colon is an optional name |
| `/add <username>`, `/leave`, `/name <text>` | Add someone to the open group, leave it, or rename it |
| `/verify` | Shows a safety number for each person in the chat, to compare with them out of band |
| `/verified <username>` | Marks someone as checked after comparing numbers |
| `/history` | Asks members again for earlier messages in the open chat |
| `/share-history on` or `off` | Whether your client shares earlier messages with newcomers who ask |
| `/members`, `/roles` | Lists the server's members and roles |
| `/help` | Shows the command list |
| `/exit` or `/quit` | Leaves |
| Tab | Switches between the chat list and the message box |

Running the server. The owner can do all of these; anyone else needs a role that grants
the matching permission:

| Type | What it does |
|---|---|
| `/channel new <name>` | Creates a channel |
| `/channel rename <name>`, `/channel delete` | Renames or deletes the open channel |
| `/channel private <role>` | Hides the open channel from everyone except that role |
| `/channel readonly` | Only people with extra permissions can post in the open channel |
| `/channel open` | Removes those restrictions |
| `/role new <name> [permission ...]` | Creates a role, for example `/role new mods kick_members manage_messages` |
| `/role give <user> <role>`, `/role take <user> <role>` | Gives or takes away a role |
| `/role delete <name>` | Deletes a role |
| `/delete 12` or `/remove` | With `manage_messages`: deletes someone else's message, by number or the latest |
| `/invite [uses]` | Makes an invite link |
| `/settings`, `/set <name> <value>` | Shows or changes the server's settings (needs `manage_server`; scope is owner only) |
| `/status` | Version, uptime, members, storage used |
| `/reboot` | Restarts the server program; everyone reconnects by themselves |
| `/kick <user>` | Removes someone from the server; they can rejoin |
| `/ban <user>`, `/unban <user>` | Blocks or restores someone's access |

Permissions a role can carry: `view_channel`, `send_messages`, `add_reactions`,
`attach_files`, `mention_everyone`, `manage_messages`, `manage_channels`, `manage_roles`,
`manage_nicknames`, `kick_members`, `ban_members`, `create_invite`, `manage_server`,
`administrator`. Nobody can give out a permission they do not hold, or act on someone
ranked at or above themselves, and nobody can act on the owner. If you start the server
without `--owner`, the first person to register owns it.

**Inviting people.** By default anyone who can reach the server can create an account.
Start it with `--invite-only` and new accounts need an invite:

```sh
./build/dev/bin/cordedd --port 7443 --data ./server-data --name "My Server" --owner alice --invite-only
```

The owner (who can always get in) types `/invite` in the client, or `/invite 3` for a
link that works three times. The link carries the server's address, its key and a code,
so the person invited needs nothing else:

```sh
./build/dev/bin/corded-tui --vault ./bob-vault --name bob --join 'corded://...'
```

The link is long, so the client also saves it to `last-invite.txt` in your vault
directory. Other members can make invites once they have a role with the `create_invite`
permission. `--closed` stops new accounts entirely while existing members keep working.
If you make a link while connected as `localhost`, the link says `localhost` too; connect
by the address other people will use.

To talk between two computers, start the server with `--scope network` on one, and point
the other's `--server` at that machine's address. Quit a client and start it again with the same `--vault` to
see it unlock, restore history and pick up anything it missed.

To run the tests:

```sh
ctest --preset dev
```

## What the prototype does and does not do

It does:

- Real end-to-end encryption: X3DH key agreement and the Double Ratchet, built on
  libsodium. The server stores only ciphertext, and a test checks that.
- A community per server: channels that every member sees, an owner, and roles with
  permissions. Channels can be made private to a role or read-only. Members can be kicked
  and banned by people whose role allows it.
- Disappearing messages, for one message or a whole chat. When the time comes the message
  is erased from every member's vault and from the server. In a channel, only someone who
  can manage channels sets the timer; in a direct message or group, anyone in it can.
  Like deletion, this relies on everyone's client cooperating: it cannot stop someone
  who copied the text or runs a modified client.
- Running the server from a client: every setting is stored in the server and can be
  changed with `/set`, including a regular restart (`/set restart weekly sun 04:00`) and
  how long messages are kept (30 days by default). `/reboot` restarts it on the spot.
- Moderation: a role with `manage_messages` can delete other people's messages (`/remove`).
- Invite links made from the client, with optional use limits.
- Direct messages and private group chats alongside the channels. Group names are
  encrypted, so the server never learns them.
- Messages in channels are end-to-end encrypted too, so the server cannot hand a newcomer
  the history. Instead **a newcomer's client asks, and existing members' clients share the
  earlier messages**, encrypted to the newcomer. It happens by itself on joining. The
  server can forbid it (`cordedd --no-history-sharing`) and any member can opt out
  (`/share-history off`), so a newcomer may or may not get history. Shared messages are
  marked as such, deleted and disappearing messages are never shared, and someone who can
  share needs to be online.
- One client can be a member of several servers at once, with the same identity on each,
  and switch between them.
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

- Categories, nicknames, role colours, transferring ownership.
- Scale to large channels. Each message is encrypted once per member, which is fine for
  dozens of people and too slow for hundreds; sender keys are planned for that.
- Hide channel names, role names or the member list from the server. Only message
  content is hidden.
- Warn you when a contact's key changes. You can compare safety numbers with `/verify`,
  but a contact whose key later changes is not yet flagged; their messages just fail to
  decrypt.
- More than one device per user.
- Run anywhere but Linux.

## Where to look

- [Overview](master_plan/00-overview.md): architecture, trust model, tech stack, stage map
- [Roadmap](master_plan/01-roadmap.md): every stage broken into steps
- [Decisions](master_plan/02-decisions.md): choices made and gaps found in the blueprint
- [Feature roadmap](master_plan/03-feature-roadmap.md): threads, replies, reactions and the rest
- [Community model](master_plan/04-community-model.md): one server is one community; the change plan
- [Operations and platforms](master_plan/05-operations-and-platforms.md): remote administration, several devices, Windows and mobile, a browser client
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
