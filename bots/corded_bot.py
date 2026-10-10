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
import json
import os
import queue
import secrets
import shutil
import subprocess
import threading
import time


class Message:
    """One message a bot received."""

    def __init__(self, data):
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

    def __repr__(self):
        return f"<Message from {self.sender}: {self.body[:40]!r}>"


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


class Bot:
    def __init__(self, vault, username, cli=None, prefix="!", about=None):
        """vault: a folder for this bot's keys and messages (made on first run).
        username: the bot's name on the server. prefix: what commands start with.
        about: a line for the bot's profile."""
        self.vault = os.path.abspath(vault)
        self.username = username
        self.prefix = prefix
        self.about = about
        self.rooms = {}          # room id -> {"title", "kind", "members", ...}
        self.me = ""             # this bot's user id, once known
        self.live = threading.Event()
        self._cli_path = cli or find_cli()
        if not self._cli_path:
            raise RuntimeError("corded-cli was not found; set CORDED_CLI to where it is")
        self._commands = {}
        self._handlers = []
        self._timers = []
        self._lock = threading.Lock()
        self._proc = None

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

    def react(self, message, emoji):
        return self.request({"cmd": "send_event", "room_id": message.room_id, "type": "m.reaction",
                             "content": {"key": emoji},
                             "relation": {"kind": "annotation", "target": message.event_id, "key": emoji}})

    def send_file(self, room, path, caption=""):
        return self.request({"cmd": "send_file", "room_id": self.room_id(room), "path": os.path.abspath(path),
                             "caption": caption}, timeout=120)

    def room_id(self, room):
        if room in self.rooms:
            return room
        for rid, info in self.rooms.items():
            if info.get("title") == room:
                return rid
        raise KeyError(f"no room called {room!r}; known: {sorted(r.get('title', '') for r in self.rooms.values())}")

    def profile_of(self, user_id):
        """Someone's profile as this bot has received it ({} if it has not)."""
        try:
            return self.request({"cmd": "get_profile", "user_id": user_id}).get("profile") or {}
        except RuntimeError:
            return {}

    def request(self, cmd, timeout=30):
        """Sends any core command and returns its result's data. Raises
        RuntimeError with the core's message if it was refused."""
        answer = queue.Queue()
        with self._lock:
            # corded-cli answers commands in the order they were given, so the
            # next answer that arrives after those already waited for is ours.
            self._order.put(answer)
            self._send(cmd)
        try:
            result = answer.get(timeout=timeout)
        except queue.Empty:
            raise RuntimeError(f"no answer to {cmd.get('cmd')}") from None
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
        profile = {"cmd": "set_profile", "bot": True}
        if self.about:
            profile["bio"] = self.about
        try:
            self.request(profile)
        except RuntimeError:
            pass

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
        self._order = queue.Queue()
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
                try:
                    self._order.get_nowait().put(event)
                except queue.Empty:
                    pass   # corded-cli's own first steps (opening the vault), not ours
            elif kind == "connection_state":
                if event.get("state") == "live":
                    self.live.set()
                else:
                    self.live.clear()
            elif kind == "room_updated":
                room = event.get("room") or {}
                self.rooms[room.get("room_id", "")] = room
                for member in room.get("members", []):
                    if member.get("me"):
                        self.me = member.get("user_id", self.me)
            elif kind == "room_removed":
                self.rooms.pop(event.get("room_id", ""), None)
            elif kind == "event_received":
                data = event.get("data") or {}
                if data.get("mine") or data.get("shared_history") or data.get("type") != "m.text":
                    continue
                threading.Thread(target=self._handle, args=(Message(data),), daemon=True).start()

    def _handle(self, message):
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
