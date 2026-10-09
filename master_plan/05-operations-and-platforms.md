# Operations, Devices and Platforms

Requirements the owner gave after trying the working system across two machines. Each is
recorded as a decision in [02-decisions.md](02-decisions.md) and as a step in the change
plan in [04-community-model.md](04-community-model.md), section 6.

| Topic | Decision | Change-plan step | State |
|---|---|---|---|
| Leaving the client with a command | (small) | done | `/exit` built |
| Switching between servers in the client | D-27, C5 | C5 | Core built; terminal client switcher next |
| Acting on one specific message | (design already supports it) | C4 | Core built; terminal client needs a way to pick a message |
| Server scope: machine, network, internet | D-33 | C12 | Planned |
| All server settings from the owner's client | D-34 | C16 | Planned |
| Scheduled maintenance and restarts | D-35 | C16 | Planned |
| Remote update of the server | D-36 | C16 | Planned; needs a decision on release signing |
| One person on several devices | D-37 | C13 | Planned |
| Clients on Linux, Windows, macOS, iOS, Android | D-38 | C14 | Planned |
| A browser client | D-39 | C15 | Planned, last |

## 1. Running a server without sitting at it (D-34, D-35, D-36)

The owner will set up a server on a Raspberry Pi at home and then be away from it for
months. Everything that today needs the server's command line has to be reachable from
the owner's client.

### Settings live in the server, not on its command line (D-34)

Today, things like invite-only registration are command-line flags that must be repeated
on every start. They move into the server's database:

| Setting | Values | Default |
|---|---|---|
| `name` | text | `corded` |
| `scope` | `machine`, `network`, `internet` | `machine` |
| `registration` | `open`, `invite`, `closed` | `open`; `invite` when scope is `internet` |
| `history_sharing` | on, off | on |
| `retention_days` | number, 0 = keep forever | 30 |
| `maintenance.restart` | off, or a schedule such as `weekly sun 04:00` | off |
| `maintenance.housekeeping` | on, off | on |
| `update.channel` | `off`, `notify`, `security`, `all` | `notify` |
| rate limits and caps | numbers | as today |

- Command-line flags still work and set the stored value, so first-time setup is unchanged.
- The owner, and anyone with `MANAGE_SERVER`, reads and changes settings from a client:
  `/settings` lists them, `/set <name> <value>` changes one. Changes are announced to
  members where they affect them (for example registration closing).
- Two settings stay owner-only and cannot be delegated by a role: `scope` and anything
  under `update`. They change who can reach the machine and what code it runs.
- Settings that need a restart to take effect (the listening address) say so.

### Maintenance (D-35)

- **Housekeeping**, on by default, runs daily at a quiet time: drop expired invites and
  messages past the retention window, checkpoint and compact the database, rotate logs.
- **Scheduled restart**, off by default: `/set maintenance.restart weekly sun 04:00`.
  The server warns connected clients a minute ahead, finishes what is in flight, and
  restarts itself. Clients reconnect on their own, as they already do.
- **Restart now**: `/reboot` from the owner's client (owner or `MANAGE_SERVER`). This
  restarts the server program, not the computer it runs on.
- **How a restart works.** The server replaces itself with a fresh copy of its own
  program after a clean shutdown, so it needs nothing else installed. Where it runs
  under a service manager such as systemd it instead exits and lets that restart it.
- **Status**: `/server` shows uptime, version, last restart, last housekeeping run,
  storage used and connected members.

### Remote update (D-36)

`/update` from the owner's client makes the server update itself. This is the most
sensitive feature in the project: by design it changes the code a machine runs, triggered
over the network. The rules that make it safe:

1. **Only official releases, verified by signature.** The server downloads a release for
   its own platform and checks a signature made with the project's release key, whose
   public half is built into every server. A release that does not verify is refused.
   The download source is not trusted; only the signature is.
2. **So a stolen owner account cannot run its own code.** The worst someone who takes
   over the owner's client can do through `/update` is move the server to another
   official release.
3. **Never backwards.** A server refuses to "update" to an older version than it is
   running, so it cannot be pushed back to a release with a known hole.
4. **Owner only.** No role can grant it.
5. **Safe to fail.** The old program is kept. The new one must start and answer a health
   check; if it does not, the old one is put back and the owner is told.
6. **Visible.** Every update, attempted or done, is recorded and shown in `/server`.

`update.channel` decides what happens without being asked: `off`; `notify` (tell the
owner a new release exists); `security` (install releases marked as security fixes on
their own); `all`.

What this needs before it can be built:

- **Release binaries** for Linux on x86-64 and ARM64 (the Pi), built by CI as single
  self-contained files. Planned already in Stage 2 and Stage 7; it moves forward.
- **A release signing key.** Someone has to hold the private half. See open question 1.

Until both exist, a server is updated by hand on the machine (`git pull` and
`./build.sh`). An interim "rebuild from source at a tagged version" command was
considered and rejected: it would trust whatever the network returns for that tag.

## 2. One person, several devices (D-37)

The owner uses a PC and a laptop and wants the server to know they are the same person.
The idea offered was a secret the clients share: if two clients present the same secret,
they are the same person.

That is the right shape, and the pieces are already in the design. Every person has a
long-term **identity key**; its public half is their user id on every server. The secret
that makes two clients "the same person" is the private half of that key. What is added:

- **Getting the identity onto a second device.** Two ways, both ending with the same
  identity key on the new device:
  - *Linking.* The existing device shows a short code or QR; the new device enters or
    scans it; the identity passes between them through an encrypted channel. Nothing to
    write down. Needs the first device at hand.
  - *Recovery phrase.* When an identity is created, the client can show 24 words that
    encode it. Typing the words into a new client recreates the identity. This also
    answers "I lost my only device", which today means starting over.
- **The server never sees the secret.** It sees the same public user id arriving from
  another device, with that device's own keys signed by the identity key, and so knows
  it is the same person without being able to pretend to be them.
- **Each device keeps its own keys and its own vault passphrase.** Losing a laptop means
  removing that one device, not changing identity.
- **Messages reach every device.** Senders encrypt to each of a person's devices, and a
  person's own other devices get a copy of what they send.
- **A new device has no history.** It asks for it the same way a newcomer does
  (section 3 of the community model), and the person's own other device answers.
- **Other people are told.** When someone adds a device, contacts who verified them see
  that a device was added; it is trusted because the identity key signed it.

What it changes in the code: the server stores several devices per person and hands out
keys per device; clients keep an encrypted session per device instead of per person; the
device list is signed and checked. This was planned from the start (decision D-15) and
is step C13.

## 3. Platforms (D-38)

**Can a local GUI be built for Linux, Windows, macOS, iOS and Android?** Yes. The core is
portable C++ with a plain C interface, and that is exactly what it was designed for.

- **One GUI codebase.** The plan's recommendation is Flutter, which builds for all five
  from one codebase and can call the core directly. Qt is the alternative if desktop
  matters more than mobile.
- **The core on each platform.** Linux works today. Windows needs a small port (a few
  Unix-only calls in the server and core, and Windows build settings). macOS, iOS and
  Android need build configurations and testing, not redesign.
- **No Mac is needed to build.** GitHub's build machines include macOS and Windows, free
  for public repositories. Builds for every platform can run there on each change.
- **What does cost money is distribution through app stores, not building:**

| Platform | Free route | Paid route |
|---|---|---|
| Linux | Download, package managers | none needed |
| Windows | Download; Windows shows an "unknown publisher" warning | Code-signing certificate removes the warning |
| Android | Download the APK directly | Google Play: one-time fee |
| macOS | Download; the user must approve an unsigned app by hand | Apple Developer Program, yearly, for signing and notarising |
| iOS | Effectively none for other people; a free Apple account can install on your own phone for 7 days at a time | Apple Developer Program, yearly, for TestFlight and the App Store |

So everything except iOS can be shipped without paying anyone. iOS for other people needs
the yearly Apple fee; that is Apple's rule, not a technical limit.

Order of work (step C14): a Windows build of the core and terminal client first, since
the owner's laptop runs Windows; then the GUI on desktop; then Android; then macOS and iOS.

## 4. A browser client (D-39)

**Is a browser GUI possible?** Yes, but it is the hardest client and the weakest in what
it can promise.

- **Browsers cannot open the kind of connection the server speaks.** A web page can only
  use WebSockets, not raw network sockets. The server needs a second listener that
  carries the same protocol inside a secure WebSocket. That part is modest.
- **The core has to run in the browser.** Rewriting the encryption in JavaScript would
  mean two implementations to keep correct, which is how security bugs happen. The
  better route is compiling the existing C++ core to WebAssembly: libsodium and SQLite
  both already run that way, and the vault would live in the browser's private storage.
  The networking layer needs a WebSocket version. This is real work but not a rewrite.
- **The trust problem cannot be fully solved.** A desktop app is installed once. A web
  app is sent again by its host on every visit, so whoever hosts the page can, at any
  time, send a version that steals keys. Signal declines to offer a web client for this
  reason. It can be narrowed (the owner hosts the page themselves; an installable web
  app that updates only on request) but not removed.

Recommendation: build it last (step C15), after the desktop and mobile GUI, and label it
honestly as the convenient client rather than the most secure one.

## 5. Hosting

Where a server can run, and how to do it without paying, is answered at length in the
GitHub issue on hosting. In short: it needs a machine that stays on, accepts incoming
connections on a port, and has a disk that persists. Serverless hosts and shared web
hosting cannot do that; a small virtual server, a Raspberry Pi behind a router that
forwards a port, or a tunnel from a machine that cannot accept connections directly, can.
The server-side features that make hosting easier are the scope setting (D-33), a single
self-contained binary, and the remote administration in section 1.

## 6. Open questions for the owner

1. **Who holds the release signing key?** Remote update is only as safe as that key. The
   options: the owner generates it and keeps the private half offline, signing each
   release by hand (safest, a manual step per release); or it is stored as a secret in
   GitHub's build system and releases are signed automatically (convenient; anyone who
   compromises the repository or GitHub account can then ship code to every server).
   Recommended: the first, at least until there are other contributors.
2. **Recovery phrase, linking, or both** for adding a device? Recommended: both, with
   linking as the everyday way and the phrase as the backup.
3. **Flutter or Qt** for the GUI (decision D-20, still open). Recommended: Flutter,
   because of mobile.
