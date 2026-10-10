"""A small kit for writing Corded bots in Python.

A bot is an ordinary member of a server: it has a username, its own keys and
its own encrypted vault, and it reads and writes messages exactly as a person's
client does. Nothing on the server knows or cares that it is a program. This
kit drives `corded-cli`, the headless client that ships with Corded, so a bot
needs no encryption code of its own.

    from corded_bot import Bot

    bot = Bot(vault="./ping-vault", username="pingbot")

    @bot.command("ping")
    def ping(message, words):
        return "pong"

    bot.connect("127.0.0.1:7443")
    bot.run()

Only the Python standard library is used.
"""
import collections
import json
import os
import queue
import re
import secrets
import shutil
import subprocess
import threading
import time

# The same rule the apps use: usernames are a-z, 0-9, _ and -.
_MENTION = re.compile(r"(?:^|[^A-Za-z0-9_@-])@([A-Za-z0-9_-]+)")


def mentioned_names(text):
    """The names called with @ in a message, lower-cased, without the @."""
    return {name.lower() for name in _MENTION.findall(text or "")}


class Message:
    """One message a bot received."""

    def __init__(self, data, me="", direct=False):
        content = data.get("content") or {}
        relation = data.get("relation") or {}
        self.raw = data
        self.room_id = data.get("room_id", "")
        self.event_id = data.get("event_id", "")
        self.sender = data.get("sender_username", "")       # the name commands use
        self.sender_name = data.get("sender_name", self.sender)  # the name people see
        self.sender_id = data.get("sender_user", "")
        self.body = content.get("body", "")
        self.type = data.get("type", "")
        self.thread = relation.get("target") if relation.get("kind") == "thread" else None
        # The message this one answers, in a thread or out of one.
        self.reply_to = relation.get("target") if relation.get("kind") == "reply" else content.get("reply_to")
        self.mentions_me = bool(me) and me.lower() in mentioned_names(self.body)
        self.direct = direct                                # said in a direct chat with the bot
        self.file = ({"name": content.get("name", ""), "size": content.get("size", 0), "mime": content.get("mime", "")}
                     if self.type == "m.file" else None)

    def __repr__(self):
        return f"<Message from {self.sender}: {self.body[:40]!r}>"


class Store(dict):
    """What a bot keeps between runs: a dictionary saved as one JSON file in
    the bot's folder. Change it like any dictionary, then call save()."""

    def __init__(self, path):
        super().__init__()
        self.path = path
        self._lock = threading.Lock()
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                self.update(json.load(f))

    def save(self):
        with self._lock:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            # Written beside and moved into place, so a crash never leaves half a file.
            with open(self.path + ".new", "w", encoding="utf-8") as f:
                json.dump(self, f, indent=1)
            os.replace(self.path + ".new", self.path)


def find_cli():
    """Where corded-cli is: beside this file's repository build, in a release
    folder next to the bots folder, or on PATH."""
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (os.environ.get("CORDED_CLI", ""),
                      os.path.join(here, "..", "build", "dev", "bin", "corded-cli"),
                      os.path.join(here, "..", "corded-cli"),
                      os.path.join(here, "..", "corded-cli.exe"),
                      os.path.join(here, "corded-cli")):
        if candidate and os.path.isfile(candidate):
            return os.path.abspath(candidate)
    return shutil.which("corded-cli") or shutil.which("corded-cli.exe")


# Commands that put a message in a room, and what a few others answer with.
_SENDS = {"send_text": "m.text", "send_file": "m.file", "add_task": "m.task", "send_event": None}
_ANSWERS = {"start_chat": "room", "get_profile": "profile", "member_list": "members", "fetch_timeline": "events"}


class Bot:
    def __init__(self, vault, username, cli=None, prefix="!", about=None, is_bot=True, hear_bots=False):
        """vault: a folder for this bot's keys and messages (made on first run).
        username: the bot's name on the server. prefix: what commands start with.
        about: a line for the bot's profile. is_bot: say so in the profile, so
        people see a BOT tag. hear_bots: also handle what other bots say."""
        self.vault = os.path.abspath(vault)
        self.username = username
        self.prefix = prefix
        self.about = about
        self.is_bot = is_bot
        self.hear_bots = hear_bots
        self.rooms = {}          # room id -> {"title", "kind", "members", ...}
        self.me = ""             # this bot's user id, once known
        self.live = threading.Event()
        self.store = Store(os.path.join(self.vault, "store.json"))
        self.join_delay = 15     # seconds between someone joining and on_join, so their profile has arrived
        self._cli_path = cli or find_cli()
        if not self._cli_path:
            raise RuntimeError("corded-cli was not found; set CORDED_CLI to where it is")
        self._commands = {}
        self._handlers = []
        self._file_handlers = []
        self._joiners = []
        self._timers = []
        self._lock = threading.Lock()
        self._proc = None
        self._waiting = []                          # commands whose answer has not come yet
        self._mine = collections.OrderedDict()      # event id -> what this bot just sent
        self._recent = collections.OrderedDict()    # event id -> Message, the last couple of thousand
        self._profiles = {}                         # user id -> profile
        self._ready = threading.Event()             # connect() has finished
        self._rooms_changed = 0
        self._met = set()                           # user ids seen in a room since this start
        self._announcing = False
        self._jobs = None
        self._jobs_ahead = 0

    # ---- what a bot author writes ----

    def command(self, name, help=""):
        """@bot.command("roll") def roll(message, words): return "4"
        The function gets the message and the words after the command. What it
        returns (if anything) is sent as a reply in the same place."""
        def register(fn):
            self._commands[name.lower()] = (fn, help)
            return fn
        return register

    def on_message(self, fn):
        """@bot.on_message def seen(message): ...   Called for every message
        from someone else, commands included."""
        self._handlers.append(fn)
        return fn

    def on_file(self, fn):
        """@bot.on_file def got(message): ...   Called for every file someone
        else sends; message.file has its name, size and kind, message.body its caption."""
        self._file_handlers.append(fn)
        return fn

    def on_join(self, fn):
        """@bot.on_join def welcome(member): ...   Called once for each person
        who joins the server after this bot's first run, also if they joined
        while the bot was off. member has "username", "display_name" and
        "user_id". Other bots are left out."""
        self._joiners.append(fn)
        return fn

    def every(self, seconds):
        """@bot.every(3600) def hourly(): ...   Called on that beat while running."""
        def register(fn):
            self._timers.append([seconds, time.time() + seconds, fn])
            return fn
        return register

    # ---- talking ----

    def say(self, room, text, reply_to=None, thread=None):
        """Sends text to a room (its id or its title, like "#general")."""
        cmd = {"cmd": "send_text", "room_id": self.room_id(room), "body": text}
        if reply_to:
            cmd["reply_to"] = reply_to
        if thread:
            cmd["thread"] = thread
        return self.request(cmd)

    def reply(self, message, text):
        """Answers where the message was said: in its thread if it had one."""
        return self.say(message.room_id, text, reply_to=message.event_id, thread=message.thread)

    def dm(self, username, text=None):
        """Opens the direct chat with someone (or finds it) and returns its
        room id; says text there if given."""
        room = self.request({"cmd": "start_chat", "username": username})["room"]
        self.rooms[room["room_id"]] = room
        if text:
            self.say(room["room_id"], text)
        return room["room_id"]

    def react(self, message, emoji):
        return self.request({"cmd": "send_event", "room_id": message.room_id, "type": "m.reaction",
                             "content": {"key": emoji},
                             "relation": {"kind": "annotation", "target": message.event_id, "key": emoji}})

    def send_file(self, room, path, caption="", reply_to=None, thread=None):
        """Sends a file to a room; thread is the id of the message whose
        thread it goes under."""
        cmd = {"cmd": "send_file", "room_id": self.room_id(room), "path": os.path.abspath(path), "caption": caption}
        if reply_to:
            cmd["reply_to"] = reply_to
        if thread:
            cmd["thread"] = thread
        return self.request(cmd, timeout=600)

    def typing(self, room):
        """Shows "typing" in a room for a few seconds; call again while working."""
        try:
            self.request({"cmd": "typing", "room_id": self.room_id(room)})
        except (RuntimeError, KeyError):
            pass

    def room_id(self, room):
        if room in self.rooms:
            return room
        for rid, info in self.rooms.items():
            if info.get("title") == room:
                return rid
        raise KeyError(f"no room called {room!r}; known: {sorted(r.get('title', '') for r in self.rooms.values())}")

    def members(self):
        """Everyone this bot shares a room with, itself left out: user id -> member."""
        found = {}
        for room in list(self.rooms.values()):
            for member in room.get("members", []):
                if not member.get("me"):
                    found[member.get("user_id", "")] = member
        return found

    def profile_of(self, user_id):
        """Someone's profile as this bot has received it ({} if it has not)."""
        if user_id not in self._profiles:
            try:
                self._profiles[user_id] = self.request({"cmd": "get_profile", "user_id": user_id}).get("profile") or {}
            except RuntimeError:
                return {}
        return self._profiles[user_id]

    def recall(self, event_id):
        """A recent message by its id (None if it is older than the bot
        remembers): what message.reply_to and message.thread point at."""
        return self._recent.get(event_id)

    def work(self, fn, *args):
        """Runs fn(*args) after the jobs already waiting, one at a time: for
        work that must not run twice at once, like a model on one graphics
        card. Returns how many jobs are ahead of this one."""
        with self._lock:
            if self._jobs is None:
                self._jobs = queue.Queue()
                threading.Thread(target=self._work, daemon=True).start()
            ahead = self._jobs_ahead
            self._jobs_ahead += 1
        self._jobs.put((fn, args))
        return ahead

    def request(self, cmd, timeout=30):
        """Sends any core command and returns its result's data. Raises
        RuntimeError with the core's message if it was refused."""
        name = cmd.get("cmd")
        waiter = {"answer": queue.Queue(), "send": name in _SENDS, "key": _ANSWERS.get(name),
                  "type": cmd.get("type") or _SENDS.get(name), "body": cmd.get("body")}
        with self._lock:
            self._waiting.append(waiter)
            self._send(cmd)
        try:
            result = waiter["answer"].get(timeout=timeout)
        except queue.Empty:
            raise RuntimeError(f"no answer to {name}") from None
        finally:
            with self._lock:
                if waiter in self._waiting:
                    self._waiting.remove(waiter)
        if not result.get("ok"):
            raise RuntimeError((result.get("error") or {}).get("message", "refused"))
        return result.get("data") or {}

    # ---- running ----

    def connect(self, address, invite=None):
        """address: "host:port", or an invite link that starts with corded://."""
        self._start()
        if address.startswith("corded://"):
            self._connect = {"cmd": "connect", "link": address}
        else:
            host, _, port = address.rpartition(":")
            self._connect = {"cmd": "connect", "host": host, "port": int(port)}
            if invite:
                self._connect["invite"] = invite
        if not self.live.wait(3):   # a vault that already knows its server reconnects by itself
            self.request(self._connect)
        if not self.live.wait(30):
            raise RuntimeError("could not reach the server")
        # A bot says that it is one, so people see a BOT tag beside its name.
        profile = {"cmd": "set_profile", "bot": self.is_bot}
        if self.about:
            profile["bio"] = self.about
        try:
            self.request(profile)
        except RuntimeError:
            pass
        if self._joiners and not self.store.get("kit.members_known"):
            # First run: whoever is here already did not just join. The rooms
            # arrive one by one, so wait until they have stopped arriving.
            until = time.time() + 60
            while time.time() < until and (not self.rooms or time.time() - self._rooms_changed < 4):
                time.sleep(0.5)
            self.store["kit.members"] = sorted(self.members())
            self.store["kit.members_known"] = True
            self.store.save()
        self._ready.set()

    def run(self):
        """Handles messages until the process is stopped."""
        try:
            while self._proc.poll() is None:
                now = time.time()
                for timer in self._timers:
                    if now >= timer[1]:
                        timer[1] = now + timer[0]
                        self._guard(timer[2])
                time.sleep(0.2)
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()

    def stop(self):
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()

    # ---- inside ----

    def _start(self):
        os.makedirs(self.vault, exist_ok=True)
        # The passphrase only guards the vault on disk; it is kept beside it,
        # readable by this user alone, so the bot can start unattended.
        pass_file = os.path.join(self.vault, "bot-passphrase")
        if not os.path.exists(pass_file):
            with os.fdopen(os.open(pass_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as f:
                f.write(secrets.token_urlsafe(24))
        passphrase = open(pass_file).read().strip()
        self._proc = subprocess.Popen(
            [self._cli_path, "--vault", os.path.join(self.vault, "vault"), "--pass", passphrase,
             "--name", self.username],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        threading.Thread(target=self._read, daemon=True).start()

    def _send(self, cmd):
        self._proc.stdin.write(json.dumps(cmd) + "\n")
        self._proc.stdin.flush()

    def _read(self):
        for line in self._proc.stdout:
            try:
                event = json.loads(line)
            except ValueError:
                continue
            kind = event.get("event")
            if kind == "command_result":
                self._answer(event)
            elif kind == "connection_state":
                if event.get("state") == "live":
                    self.live.set()
                else:
                    self.live.clear()
            elif kind == "room_updated":
                room = event.get("room") or {}
                self.rooms[room.get("room_id", "")] = room
                self._rooms_changed = time.time()
                for member in room.get("members", []):
                    if member.get("me"):
                        self.me = member.get("user_id", self.me)
                self._notice_joins(room)
                self._notice_strangers(room)
            elif kind == "room_removed":
                self.rooms.pop(event.get("room_id", ""), None)
            elif kind == "profile_updated":
                self._profiles[event.get("user_id", "")] = event.get("profile") or {}
            elif kind == "event_received":
                data = event.get("data") or {}
                if data.get("mine"):
                    self._mine[data.get("event_id", "")] = data
                    while len(self._mine) > 200:
                        self._mine.popitem(last=False)
                if data.get("shared_history") or data.get("type") not in ("m.text", "m.file"):
                    continue
                direct = self.rooms.get(data.get("room_id", ""), {}).get("kind") == "direct"
                message = Message(data, me=self.username, direct=direct)
                if message.type == "m.text":
                    self._recent[message.event_id] = message
                    while len(self._recent) > 2000:
                        self._recent.popitem(last=False)
                if data.get("mine"):
                    continue
                if not self.hear_bots and self.members().get(message.sender_id, {}).get("bot"):
                    continue   # two bots answering each other would never stop
                threading.Thread(target=self._handle, args=(message,), daemon=True).start()

    def _answer(self, result):
        """Gives a command's answer to the request that is waiting for it.
        corded-cli does not say which command an answer belongs to, and the
        client also sends things of its own (its profile to a new room, say)
        whose answers arrive in between, so an answer is matched by what it
        holds: one about a sent message goes to the request that sent that
        message, one with a room to the request that asked for a room."""
        data = result.get("data") or {}
        with self._lock:
            waiter = None
            if not result.get("ok"):
                waiter = self._waiting[0] if self._waiting else None
            elif "event_id" in data:
                sent = self._mine.get(data["event_id"])
                if sent is None:
                    return   # the client's own doing, not ours
                senders = [w for w in self._waiting if w["send"]]
                fits = [w for w in senders if w["type"] in (None, sent.get("type"))
                        and (w["type"] != "m.text" or w["body"] == (sent.get("content") or {}).get("body"))]
                waiter = (fits or senders or [None])[0]
            else:
                plain = [w for w in self._waiting if not w["send"]]
                keyed = [w for w in plain if w["key"] and w["key"] in data]
                waiter = (keyed or [w for w in plain if not w["key"]] or [None])[0]
            if waiter is None:
                return       # corded-cli's own first steps (opening the vault)
            self._waiting.remove(waiter)
        waiter["answer"].put(result)

    def _notice_joins(self, room):
        if not self._joiners or not self.store.get("kit.members_known"):
            return
        known = set(self.store.get("kit.members", []))
        new = [m for m in room.get("members", []) if not m.get("me") and m.get("user_id") not in known]
        if not new:
            return
        self.store["kit.members"] = sorted(known | {m["user_id"] for m in new})
        self.store.save()
        for member in new:
            threading.Thread(target=self._joined, args=(member["user_id"],), daemon=True).start()

    def _notice_strangers(self, room):
        """A profile reaches someone new only with the next thing its owner
        says, so a quiet bot would look like a person to them. A few seconds
        after someone new appears, the bot says again that it is one."""
        here = {m.get("user_id") for m in room.get("members", []) if not m.get("me")}
        strangers = here - self._met
        self._met |= here
        if strangers and self.is_bot and self._ready.is_set() and not self._announcing:
            self._announcing = True
            timer = threading.Timer(8, self._announce)   # once their keys are published
            timer.daemon = True
            timer.start()

    def _announce(self):
        self._announcing = False
        try:
            self.request({"cmd": "set_profile", "bot": True})
        except RuntimeError:
            pass

    def _joined(self, user_id):
        self._ready.wait()
        time.sleep(self.join_delay)   # a bot says it is one a moment after it joins
        member = self.members().get(user_id)
        if member and not member.get("bot"):
            for fn in self._joiners:
                self._guard(fn, member)

    def _work(self):
        while True:
            fn, args = self._jobs.get()
            self._guard(fn, *args)
            with self._lock:
                self._jobs_ahead -= 1

    def _handle(self, message):
        if message.type == "m.file":
            for handler in self._file_handlers:
                self._guard(handler, message)
            return
        for handler in self._handlers:
            self._guard(handler, message)
        body = message.body.strip()
        if not body.startswith(self.prefix):
            return
        words = body[len(self.prefix):].split()
        if not words:
            return
        name = words[0].lower()
        if name == "help" and "help" not in self._commands:
            lines = [f"{self.prefix}{n}  {h}".rstrip() for n, (_, h) in sorted(self._commands.items())]
            self._guard(self.reply, message, "\n".join(lines) or "I have no commands yet.")
            return
        if name in self._commands:
            answer = self._guard(self._commands[name][0], message, words[1:])
            if isinstance(answer, str) and answer:
                self._guard(self.reply, message, answer)

    def _guard(self, fn, *args):
        """One command going wrong must not take the bot down."""
        try:
            return fn(*args)
        except Exception as error:  # noqa: BLE001 - a bot keeps running
            print(f"[{self.username}] {getattr(fn, '__name__', fn)} failed: {error}", flush=True)
            return None
