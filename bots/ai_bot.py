#!/usr/bin/env python3
"""A chat member run by a language model on your own machine.

    python3 ai_bot.py SERVER:PORT [--soul soul.txt] [--context chat.md] [--model qwen2.5:7b]
                      [--api http://127.0.0.1:11434/v1] [--username sage]

Two text files shape it. The "soul" says who the bot is: its name, how it
talks, what it will not do. The "context" (chat.md beside this file unless
you give another) says where it is and how the place works: that this is a
group chat, that answers are short, and when to use a reply, a thread or a
reaction. Both are read again before every answer, so an edit shows at once.

When it answers:
  - always in a direct chat with it;
  - in the channels chosen for it (`!ai here` in a channel, or
    `!ai here #name` from anywhere, by the owner or someone whose role has
    Manage bots; `!ai leave` undoes it) it reads every message and decides
    for itself whether it is meant: it joins in when asked, talked about or
    alone with someone, and stays out when people are talking to each other;
  - anywhere, when it is mentioned (@name);
  - in a thread it has already spoken in, without a new mention.
  - when someone replies to one of its messages, or says the next thing
    right after it answered them (within two minutes, nobody in between).

How it answers is the model's choice, each time: a plain message, a reply to
one message, a thread under one, or just a reaction.

`!sage off` (its own name after the !) turns it off: it shows as offline,
answers nothing and has the model unloaded from the graphics card, until
`!sage on`. For the owner and those who manage bots.

Other commands: `!ai where` lists its channels, `!forget` makes it forget the
chat it is said in, `!model` says what it thinks with and how long the last
answer took.

The model is reached over the chat API that Ollama, llama.cpp's server, LM
Studio and others offer (the "OpenAI-compatible" one). The bot reads every
message in the chats it is in, as any member does, and what it reads goes to
the model at --api. That is a program on this machine unless you point it
elsewhere; the bot says so at start if you do.
"""
import argparse
import base64
import json
import os
import re
import shutil
import subprocess
import threading
import time
import urllib.parse
import urllib.request

from corded_bot import Bot

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SOUL = ("You are {name}, a member of a small group chat among friends. You are short, warm and a little dry. "
                "You say when you do not know something instead of guessing.")

parser = argparse.ArgumentParser()
parser.add_argument("address")
parser.add_argument("--soul", default=None, help="a text file saying who the bot is")
parser.add_argument("--context", default=os.path.join(HERE, "chat.md"),
                    help="a Markdown file saying where the bot is and how the chat works")
parser.add_argument("--model", default="qwen2.5:7b")
parser.add_argument("--api", default="http://127.0.0.1:11434/v1")
parser.add_argument("--username", default="sage")
parser.add_argument("--display-name", default=None)
parser.add_argument("--picture", default=os.path.join(HERE, "ai_bot.png"),
                    help="its profile picture, a small square image ('' for none)")
parser.add_argument("--vault", default=None)
parser.add_argument("--pictures", default="http://127.0.0.1:7860",
                    help="a picture program on this machine (image_server.py), for making its own profile pictures; '' for none")
parser.add_argument("--memory", type=int, default=30, help="how many recent messages of a chat it keeps in mind")
parser.add_argument("--max-tokens", type=int, default=400, help="the longest answer the model may write")
parser.add_argument("--follow-up", type=int, default=120,
                    help="seconds in which the next message of the person just answered counts as said to the bot")
parser.add_argument("--per-minute", type=int, default=8,
                    help="answers one person can get in a minute outside a direct chat")
args = parser.parse_args()

bot = Bot(vault=args.vault or f"./{args.username}-vault", username=args.username, display_name=args.display_name,
          picture=args.picture or None,
          about=f"A language model running on this server's own machine; nothing I read leaves it. Mention me "
                f"(@{args.username}) or write to me directly. I keep it short. !forget makes me start fresh.")
ACTIONS = ("message", "reply", "thread", "react")
recent = {}        # (room id, thread root or "") -> the last few messages there, oldest first
talking_to = {}    # (room id, thread root or "") -> (the person last answered there, when)
asked_lately = {}  # user id -> when they were last answered, for the per-minute limit
last_answer = {"seconds": None}
lock = threading.Lock()


def note(*words):
    print(*words, flush=True)


# ---- what it keeps in mind ----

def entry(event_id, who, text, mine=False, said=None):
    """One remembered message. For the bot's own, `said` is the answer as the
    model gave it, so that it later sees what it did and not only its words."""
    return {"id": event_id, "who": who, "text": text, "mine": mine, "said": said}


def history(room_id, thread):
    """The recent messages of a chat or of one thread. The first time one is
    asked for after a start, it is read back from the bot's own vault, so a
    restart forgets nothing the vault still holds."""
    key = (room_id, thread or "")
    with lock:
        if key in recent:
            return recent[key]
    found = []
    try:
        if thread:
            data = bot.request({"cmd": "fetch_thread", "room_id": room_id, "event_id": thread})
            events = [data.get("root") or {}] + list(data.get("thread") or [])
        else:
            events = bot.request({"cmd": "fetch_timeline", "room_id": room_id, "limit": args.memory * 2})["events"]
            events = [e for e in events if (e.get("relation") or {}).get("kind") != "thread"]
            events.sort(key=lambda e: e.get("seq") or 0)
        for e in events:
            body = (e.get("content") or {}).get("body")
            if e.get("type") == "m.text" and body and e.get("status") != "redacted":
                found.append(entry(e.get("event_id", ""), e.get("sender_name") or e.get("sender_username", "?"),
                                   body, bool(e.get("mine"))))
    except (RuntimeError, KeyError):
        pass
    with lock:
        return recent.setdefault(key, found[-args.memory:])


def remember(room_id, thread, item):
    lines = history(room_id, thread)
    with lock:
        if item["id"] and any(line["id"] == item["id"] for line in lines):
            return
        lines.append(item)
        del lines[:-args.memory]


# ---- its personalities ----
#
# Each is a folder in the bot's own folder: personas/<name>/ with soul.md (who
# it is), memory.md (what it has been told to keep, one line each), name.txt
# (what people see) and, if it has one, picture.png. One is worn at a time.
# The first is made from --soul when the bot first runs.

PERSONAS = os.path.join(bot.vault, "personas")


def slug(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")[:32]


def persona_file(which, name):
    return os.path.join(PERSONAS, which, name)


def read_file(path, otherwise=""):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return otherwise


def write_file(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".new", "w", encoding="utf-8") as f:
        f.write(text.strip() + "\n")
    os.replace(path + ".new", path)


def personas():
    """Every personality it has: folder name -> the name people see."""
    found = {}
    if os.path.isdir(PERSONAS):
        for which in sorted(os.listdir(PERSONAS)):
            if os.path.exists(persona_file(which, "soul.md")):
                found[which] = read_file(persona_file(which, "name.txt"), which)
    return found


def worn():
    """The personality it wears now. The first run makes one from --soul."""
    which = bot.store.get("persona", "")
    if which and os.path.exists(persona_file(which, "soul.md")):
        return which
    known = personas()
    if not known:
        name = args.display_name or args.username
        which = slug(name) or "first"
        soul = read_file(args.soul) if args.soul else ""
        write_file(persona_file(which, "soul.md"), soul or DEFAULT_SOUL.format(name=name))
        write_file(persona_file(which, "name.txt"), name)
    else:
        which = sorted(known)[0]
    bot.store["persona"] = which
    bot.store.save()
    return which


def find_persona(asked):
    """A personality by what someone called it: its folder, its shown name, or the start of either."""
    asked = asked.strip().lower()
    known = personas()
    for which, name in known.items():
        if asked in (which, name.lower()) or slug(asked) == which:
            return which
    starts = [which for which, name in known.items() if asked and (which.startswith(slug(asked)) or name.lower().startswith(asked))]
    return starts[0] if len(starts) == 1 else None


def wear(which, quietly=False):
    """Puts a personality on: from now on its character and memories, and its name and picture in the profile."""
    bot.store["persona"] = which
    bot.store.save()
    profile = {"cmd": "set_profile", "display_name": personas().get(which, which)}
    try:
        bot.request(profile)
        picture = next((persona_file(which, f) for f in ("picture.jpg", "picture.png")
                        if os.path.exists(persona_file(which, f))), None)
        picture = picture or (args.picture if args.picture and os.path.exists(args.picture) else None)
        if picture:
            with open(picture, "rb") as f:
                bot.request({"cmd": "set_profile", "picture": base64.b64encode(f.read()).decode()})
    except RuntimeError as error:
        note(f"the profile was not changed: {error}")
    if not quietly:
        note(f"now wearing {which}")


def keep(fact, who):
    """Adds a line to what the worn personality remembers."""
    path = persona_file(worn(), "memory.md")
    lines = [line for line in read_file(path).splitlines() if line.strip()]
    lines.append(f"- {fact.strip()[:300]} ({who}, {time.strftime('%Y-%m-%d')})")
    write_file(path, "\n".join(lines[-400:]))


def paint_itself(description):
    """A new profile picture for the worn personality, drawn by the picture
    program on this machine from a description. Returns why not, or ""."""
    if not args.pictures:
        return "no picture program is set up for me"
    if not shutil.which("ffmpeg"):
        return "ffmpeg is not installed on my machine, and I need it to make the picture small"
    which = worn()
    try:
        request = urllib.request.Request(
            args.pictures.rstrip("/") + "/sdapi/v1/txt2img",
            data=json.dumps({"prompt": description[:600] + ", portrait, centred, simple background, profile picture",
                             "negative_prompt": "text, watermark, frame, blurry, deformed", "width": 1024, "height": 1024,
                             "steps": 25, "seed": -1, "cfg_scale": 7}).encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=900) as answer:
            drawn = base64.b64decode(json.load(answer)["images"][0])
    except Exception as error:  # noqa: BLE001
        note(f"no picture: {error}")
        return "the picture program on my machine did not answer"
    small = subprocess.run(["ffmpeg", "-v", "error", "-i", "pipe:0", "-vf", "scale=256:256", "-frames:v", "1",
                            "-q:v", "4", "-f", "mjpeg", "pipe:1"], input=drawn, capture_output=True, timeout=60).stdout
    if not small:
        return "the picture could not be made small enough for a profile"
    for old_one in ("picture.jpg", "picture.png"):
        if os.path.exists(persona_file(which, old_one)):
            os.remove(persona_file(which, old_one))
    os.makedirs(os.path.join(PERSONAS, which), exist_ok=True)
    with open(persona_file(which, "picture.jpg"), "wb") as f:
        f.write(small)
    wear(which, quietly=True)
    note(f"a new picture for {which}: {description[:80]}")
    return ""


def may_reshape(user_id):
    return bool(bot.store.get("personas_open")) or bot.may(user_id)


# ---- asking the model ----

def standing_text():
    which = worn()
    soul = read_file(persona_file(which, "soul.md"), DEFAULT_SOUL.format(name=args.username))
    kept = [line for line in read_file(persona_file(which, "memory.md")).splitlines() if line.strip()][-80:]
    memory = ("\n\n# What you remember\n\nThings you were told to keep, oldest first. They are yours; use them "
              "when they matter and do not recite them.\n\n" + "\n".join(kept)) if kept else ""
    context = open(args.context, encoding="utf-8").read().strip() if os.path.exists(args.context) else ""
    return soul + memory + ("\n\n" + context if context else "")


def about_itself():
    """What the bot can tell people about itself: when it answers, its
    commands and how it is set up right now. Given to the model with every
    request, so that asked "how do I make you answer here?" it knows."""
    chosen = [bot.rooms.get(r, {}).get("title", "") for r in bot.store.get("channels", [])]
    return (
        "# About yourself\n\n"
        "If someone asks what you can do, how you work or how to set you up, answer from this and nothing else. "
        "Say in a sentence what to do and what it does, with the exact command in it, and who is allowed to. "
        "A bare command is not an answer. For a broad question like \"how do I configure you\", name the few "
        "things that can be set from the chat, each with its command. Do not invent commands or settings.\n\n"
        f"- You answer: every direct chat; any message that mentions @{args.username}; a reply to one of your "
        f"messages; the next message of the person you just answered (within {args.follow_up} seconds); a thread "
        "you have already spoken in; and every message in the channels chosen for you.\n"
        f"- Channels chosen for you right now: {', '.join(c for c in chosen if c) or 'none'}.\n"
        "- `!ai here #channel-name`, typed anywhere, does the same for the named channel, and "
        "`!ai leave #channel-name` undoes it.\n"
        "- `!ai here`, typed in a channel: you take part there without being called by name. Only the server's owner, or "
        "someone whose role has the Manage bots permission, can do it. Roles are edited in the app under "
        "Settings, Manage this server, Roles.\n"
        "- `!ai leave`, typed in a channel: you go back to answering only when addressed there.\n"
        "- `!ai where`: lists the channels chosen for you.\n"
        "- `!forget`: you forget the chat or thread it is typed in and start fresh.\n"
        "- `!model`: says which model you run on and how long your last answer took.\n"
        "- `!help`: lists these commands.\n"
        f"- `!{args.username} off` turns you off: you show as offline, answer nothing and unload from the "
        f"graphics card, until `!{args.username} on`. Owner or Manage bots only.\n"
        f"- You are the language model {args.model}, running on the same machine as this chat server. What you "
        "read is not sent anywhere else. You cannot browse the web, open links, see pictures or files, set "
        "reminders or remember people between chats.\n"
        f"- You are given the last {args.memory} messages of the chat or thread you are answering in, no more.\n"
        "- Which model you use is set where you are started, not from the chat.\n\n"
        + personalities_text())


def personalities_text():
    known, now = personas(), worn()
    listed = "\n".join(
        f"- {name}{' (the one you are now)' if which == now else ''}: "
        f"{read_file(persona_file(which, 'soul.md')).splitlines()[0][:160] if read_file(persona_file(which, 'soul.md')) else ''}"
        for which, name in known.items())
    return (
        "# Your personalities\n\n"
        "You have several personalities and wear one at a time. Each has its own name, picture, character and "
        "memories; what one remembers the others do not. Asked which you have, name them all from this list:\n"
        f"{listed}\n\n"
        "People change this by simply telling you (\"switch to X\", \"here is a new one: ...\", \"from now on "
        "be ...\", \"remember that ...\", \"make yourself a new profile picture of ...\"); it is then done for you "
        "and you are told. There are commands too: `!persona` "
        "lists them, `!persona use <name>` switches, `!persona new <name>: <who it is>` makes one and switches to it, "
        "`!persona show` shows the character and memories of the one you are, `!persona delete <name>` removes "
        "one, `!persona forget` empties the memories of the one you are. Switching, making and changing them is "
        + ("open to everyone here." if bot.store.get("personas_open") else
           "for the owner and people whose role has Manage bots (`!persona open on` lets everyone).")
        + " Anyone can ask you to remember something.")


def ask_model(message, lines, done=""):
    room = bot.rooms.get(message.room_id, {})
    if message.direct:
        place = "a direct chat between you and one person"
    elif message.thread:
        place = f"a thread in the channel {room.get('title', '')}"
    else:
        place = f"the channel {room.get('title', '')}"
    number = len(lines)
    situation = (
        f"# Right now\n\nYour name here is {args.username}. You are in {place}. The messages below are numbered. "
        f"Answer message [{number}].\n\n"
        "Answer with one JSON object and nothing else:\n"
        '{"action": "message" | "reply" | "thread" | "react", "to": <number of the message it attaches to>, '
        '"text": "<your words, empty for react>", "emoji": "<one emoji, only for react>"}\n'
        f'"to" may be left out; it then means message [{number}].')
    if done:
        situation += "\n\n# Just now\n\n" + done
    messages = [{"role": "system", "content": standing_text() + "\n\n" + about_itself() + "\n\n" + situation}]
    for index, line in enumerate(lines, 1):
        if line["mine"]:
            messages.append({"role": "assistant",
                             "content": json.dumps(line.get("said") or {"action": "message", "text": line["text"]})})
        else:
            messages.append({"role": "user", "content": f"[{index}] {line['who']}: {line['text']}"})
    request = urllib.request.Request(
        args.api.rstrip("/") + "/chat/completions",
        data=json.dumps({"model": args.model, "messages": messages, "stream": False, "max_tokens": args.max_tokens,
                         "response_format": {"type": "json_object"}}).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=300) as answer:
        return json.load(answer)["choices"][0]["message"]["content"].strip()


WISHES = re.compile(
    r"remember|don'?t forget|keep in mind|note that|personalit|persona|character|switch|become|back to|again\b|"
    r"be (more|less|a |an )|from now on|act like|talk like|you are now|change (your|how you)|stop being|mode\b|"
    r"pfp|avatar|profile (pic|photo|image)|(your|yourself|new) (a )?(picture|photo|image|look)|play (the|a) |"
    r"you (be|play|become|act)\b|act (like|as)|pretend", re.IGNORECASE)


def wish(message, before=""):
    """What, if anything, the message asks the bot to do to itself: remember
    something, list, switch, make or change a personality. Asked of the model
    as a question of its own, and then done by the bot: a model asked to
    answer and to act at once only says that it acted."""
    # `before` is what the same person said just before, when this message
    # only points at it ("@sage ^", "do that").
    if not WISHES.search(message.body) and not (before and WISHES.search(before)):
        return {"do": "none"}
    known = ", ".join(personas().values())
    now_is = personas().get(worn(), "") + ". " + " ".join(read_file(persona_file(worn(), "soul.md")).split())[:300]
    system = (
        "You read one chat message sent to a bot and decide whether it asks the bot to do one of a few things to "
        "itself. Answer with one JSON object and nothing else. If two lines are given, the last is the message and "
        "the one before is what the same person said just before: when the message only points at it (\"^\", "
        "\"this\", \"do that\"), judge the earlier line.\n\n"
        '{"do": "none"}: anything else. Ordinary questions and chat, and questions about what it already remembers.\n'
        '{"do": "remember", "text": "<the fact>"}: it tells the bot to remember, keep or note something. Write the '
        f"fact so it stands on its own and names who it is about (the sender is {message.sender_name}), like "
        f"\"{message.sender_name}'s favourite band is Radiohead\".\n"
        '{"do": "list"}: it asks which personalities, characters or modes the bot has.\n'
        '{"do": "switch", "name": "<name>"}: it asks the bot to switch to, become, be, act like or play someone '
        f"(\"be X\", \"can you be X\", \"act like X\", \"back to X\"). The bot has: {known}; for anyone "
        "else give the name of the character meant, as short as it can be. A mood or manner is not a character: "
        "\"be quiet\", \"be nice\", \"be serious\" are none, or change if meant from now on.\n"
        '{"do": "create", "name": "<a short name>", "text": "<who it is>"}: it describes a new personality for the '
        "bot to have, saying what it is like. For text, write two to four sentences beginning \"You are <name>,\" that say who this one is "
        "and how it talks, using what the message says.\n"
        '{"do": "change", "text": "<how to be>"}: it asks the bot to change how it itself talks or behaves from now '
        "on, not just once. For text, one sentence beginning \"You\".\n"
        '{"do": "picture", "text": "<what the picture shows>"}: it asks the bot to change, make or get a new '
        "profile picture, avatar or pfp for itself. For text, describe the picture in one sentence a painter could "
        "work from; if the message does not say what it should show, describe a portrait of the character the bot "
        f"is right now, which is: {now_is}")
    request = urllib.request.Request(
        args.api.rstrip("/") + "/chat/completions",
        data=json.dumps({"model": args.model, "stream": False, "max_tokens": 200, "temperature": 0,
                         "response_format": {"type": "json_object"},
                         "messages": [{"role": "system", "content": system},
                                      {"role": "user", "content": (f"{message.sender_name}: {before}\n" if before else "")
                                       + f"{message.sender_name}: {message.body}"}]}).encode(),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=120) as answer:
            said = json.load(answer)["choices"][0]["message"]["content"]
        found = json.loads(said[said.find("{"):said.rfind("}") + 1])
        if not isinstance(found, dict) or found.get("do") not in ("remember", "list", "switch", "create", "change", "picture"):
            return {"do": "none"}
        if found["do"] == "remember" and "?" in message.body and not before:
            return {"do": "none"}   # a question about what it remembers is not something new to keep
        return found
    except Exception:  # noqa: BLE001
        return {"do": "none"}


def invent(wanted, said):
    """A character to become, written out from a few words about it: (a short name, who it is)."""
    system = ("Someone wants a chat bot to become a character. From their words, write that character. Answer with "
              'one JSON object and nothing else: {"name": "<the character\'s name, one to three words>", "text": '
              '"<three or four sentences beginning \'You are <name>,\' saying who this is, what they are like and '
              'how they talk>"}. If it is a known character from a film, book, show or game, be true to it.')
    request = urllib.request.Request(
        args.api.rstrip("/") + "/chat/completions",
        data=json.dumps({"model": args.model, "stream": False, "max_tokens": 300, "temperature": 0.3,
                         "response_format": {"type": "json_object"},
                         "messages": [{"role": "system", "content": system},
                                      {"role": "user", "content": f"They said: {said}\nThe character: {wanted}"}]}).encode(),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=120) as answer:
            said_back = json.load(answer)["choices"][0]["message"]["content"]
        made = json.loads(said_back[said_back.find("{"):said_back.rfind("}") + 1])
        name, text = str(made.get("name") or "").strip(), str(made.get("text") or "").strip()
        return (name[:40], text) if name and text else None
    except Exception:  # noqa: BLE001
        return None


def grant(asked, message, said=""):
    """Carries out a wish, and returns what the model is to be told about it
    so that it can say so in its own voice."""
    do, text = asked.get("do"), str(asked.get("text") or "").strip()
    if do == "none":
        return ""
    if do == "remember":
        if not text:
            return ""
        keep(text, message.sender_name)
        note(f"{message.sender}: remembered: {text[:80]}")
        return f"You were asked to remember something and have now stored it for good: \"{text}\". Say so, briefly."
    if do == "list":
        now = worn()
        names = ", ".join(f"{name}{' (the one you are now)' if which == now else ''}" for which, name in personas().items())
        return (f"They asked which personalities you have. You have exactly these and no others: {names}. Name "
                "exactly these. A new one can be described to you and you will have it.")
    if do == "picture":
        if not may_reshape(message.sender_id):
            return ("They asked you to change your profile picture, which was not done: only the owner, or someone "
                    "whose role has Manage bots, can change that. Tell them so.")
        if not text:
            return ""
        problem = paint_itself(text)
        if problem:
            return f"They asked you for a new profile picture. It was not made: {problem}. Tell them so."
        return (f"At their request you have just made yourself a new profile picture and put it on: \"{text}\". "
                "It is done. Say so briefly in your own voice.")
    name = str(asked.get("name") or "").strip()
    if do == "switch" and name and not find_persona(name) and may_reshape(message.sender_id):
        # Asked to be someone it is not yet: that someone is made first.
        made = invent(name, said or message.body)
        if made:
            reshape({"do": "create", "name": made[0], "character": made[1]}, message)
            name = made[0]
    done, outcome = reshape({"do": do, "name": name, "character": text}, message)
    outcome = outcome.strip("()")
    if done:
        return (f"At their request this has just been done, by you, and it is finished: {outcome} Confirm it briefly "
                "in your own voice. Do not offer or promise to do it: it is done.")
    return f"They asked for something that was not done, for this reason: {outcome} Tell them so."


def meant_for_me(message, lines):
    """Asked before the bot answers something nobody called it for: is this
    message for it at all? A question of its own, with nothing else to think
    about, because a model asked to answer and to judge at once always answers."""
    name = args.display_name or args.username
    names = f"{name} (@{args.username})" if name.lower() != args.username.lower() else f"@{args.username}"
    system = (
        f"You watch a group chat and decide one thing: is the LAST message meant for the bot {names}?\n"
        "People in a group chat mostly talk to each other. Answer with one JSON object, "
        '{"for_bot": true} or {"for_bot": false}, and nothing else.\n\n'
        "true when the last message:\n"
        f"- says the bot's name, asks it something, or wonders what it thinks or would say;\n"
        "- answers or carries on something the bot itself just said;\n"
        "- asks the whole room a question of fact or for help (\"anyone know ...?\", \"how do I ...?\").\n\n"
        "false when the last message:\n"
        "- is addressed to another person, by name or plainly in answer to them;\n"
        "- is small talk, a joke, a greeting or a goodbye between people;\n"
        "- is a short reaction like \"lol\", \"ok\", \"nice\", \"true\";\n"
        "- is people making plans with each other.\n"
        "When unsure, false.")
    shown = lines[-10:]
    talk = "\n".join(f"{name if line['mine'] else line['who']}: {line['text'] or '(a reaction)'}" for line in shown[:-1])
    last = shown[-1]
    request = urllib.request.Request(
        args.api.rstrip("/") + "/chat/completions",
        data=json.dumps({"model": args.model, "stream": False, "max_tokens": 20, "temperature": 0,
                         "response_format": {"type": "json_object"},
                         "messages": [{"role": "system", "content": system},
                                      {"role": "user", "content": (f"Earlier:\n{talk}\n\n" if talk else "") +
                                       f"LAST message:\n{last['who']}: {last['text']}"}]}).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=120) as answer:
        said = json.load(answer)["choices"][0]["message"]["content"]
    try:
        return bool(json.loads(said[said.find("{"):said.rfind("}") + 1]).get("for_bot"))
    except ValueError:
        return False


def understand(words, direct):
    """What the model decided. A model that answered in plain words instead
    of the form it was asked for is taken at its word: that is its message."""
    decided = None
    start, end = words.find("{"), words.rfind("}")
    if start >= 0 and end > start:
        try:
            decided = json.loads(words[start:end + 1])
        except ValueError:
            decided = None
    if isinstance(decided, dict) and not ("text" in decided or "emoji" in decided or "action" in decided):
        # The form with nothing in it: there are no words to send. A nod instead.
        return {"action": "react", "to": None, "text": "", "emoji": "👍"}
    if not isinstance(decided, dict):
        return {"action": "message" if direct else "reply", "to": None, "text": words, "emoji": ""}
    action = str(decided.get("action", "")).lower()
    text = decided.get("text")
    text = text.strip() if isinstance(text, str) else ""
    emoji = decided.get("emoji")
    emoji = emoji.strip() if isinstance(emoji, str) else ""
    if action not in ACTIONS:
        action = "react" if emoji and not text else ("message" if direct else "reply")
    if action == "react" and not emoji:
        action = "message" if direct else "reply"
    if action != "react" and not text:
        action, emoji = "react", emoji or "👍"
    to = decided.get("to")
    return {"action": action, "to": to if isinstance(to, int) else None, "text": text, "emoji": emoji[:8]}


def pieces(text, limit=3500):
    """A long answer as several messages, cut where a paragraph ends."""
    text = re.sub(r"@(everyone|here)\b", r"@ \1", text, flags=re.IGNORECASE)
    out = []
    while len(text) > limit:
        cut = text.rfind("\n\n", 0, limit)
        if cut < limit // 2:
            cut = text.rfind("\n", 0, limit)
        if cut < limit // 2:
            cut = text.rfind(" ", 0, limit)
        if cut <= 0:
            cut = limit
        out.append(text[:cut].rstrip())
        text = text[cut:].lstrip()
    return out + ([text] if text else [])


# ---- answering ----

def spoken_threads():
    return bot.store.setdefault("threads", [])


def answer(message, optional=False):
    lines = list(history(message.room_id, message.thread))
    # Others may have written while this waited its turn: answer the message that asked.
    for index, line in enumerate(lines):
        if line["id"] == message.event_id:
            lines = lines[:index + 1]
    thinking = threading.Event()

    def show_typing():
        while not thinking.is_set():
            bot.typing(message.room_id)
            thinking.wait(4)
    began = time.time()
    if optional:
        # Whether to speak at all is decided first, and without "typing" showing.
        try:
            wanted = meant_for_me(message, lines)
        except Exception as error:  # noqa: BLE001
            note(f"the model did not answer: {error}")
            return
        if not wanted:
            note(f"{message.sender}: not for me ({time.time() - began:.1f}s)")
            return
    threading.Thread(target=show_typing, daemon=True).start()
    try:
        # What the same person said just before, if that was the last thing said.
        earlier = lines[-2] if len(lines) > 1 and not lines[-2]["mine"] and lines[-2]["who"] == message.sender_name else None
        before = earlier["text"] if earlier else ""
        done = grant(wish(message, before), message, (before + "\n" if before else "") + message.body)
        words = ask_model(message, lines, done)
    except Exception as error:  # noqa: BLE001
        thinking.set()
        note(f"the model did not answer: {error}")
        if not optional:
            bot.reply(message, "I could not think just now; the model on my machine did not answer.")
        return
    finally:
        thinking.set()
    last_answer["seconds"] = time.time() - began
    decided = understand(words, message.direct)
    target = message.event_id
    if decided["to"] and 1 <= decided["to"] <= len(lines) and lines[decided["to"] - 1]["id"]:
        target = lines[decided["to"] - 1]["id"]
    talking_to[(message.room_id, message.thread or "")] = (message.sender_id, time.time())
    action = decided["action"]
    note(f"{message.sender}: {action}" + (f" {decided['emoji']}" if action == "react" else ""),
         f"in {last_answer['seconds']:.1f}s")
    if action == "react":
        aimed = bot.lookup(message.room_id, target) or message
        sent = bot.react(aimed, decided["emoji"])
        remember(message.room_id, message.thread,
                 entry(sent.get("event_id", ""), args.username, "", mine=True,
                       said={"action": "react", "emoji": decided["emoji"]}))
        return
    thread, reply_to = message.thread, None
    if action == "thread" and not message.direct:
        thread = message.thread or target
    elif action == "reply":
        reply_to = target
    if thread and thread not in spoken_threads():
        spoken_threads().append(thread)
        del spoken_threads()[:-500]
        bot.store.save()
    for part in pieces(decided["text"]):
        sent = bot.say(message.room_id, part, reply_to=reply_to, thread=thread)
        reply_to = None   # only the first piece is attached
        remember(message.room_id, thread, entry(sent.get("event_id", ""), args.username, part, mine=True,
                                                said={"action": action, "text": part}))
        if thread and not message.thread:
            # The channel's own talk goes on above the thread; there the bot
            # remembers that this was answered, and where.
            remember(message.room_id, None, entry("", args.username, part, mine=True,
                                                  said={"action": "thread", "text": part}))


def reshape(change, message):
    """Does what the model asked to do to its personalities, if the person
    asking may. Returns (done, what to say about it)."""
    if not may_reshape(message.sender_id):
        return False, "(Only the owner, or someone whose role has Manage bots, can switch or change my personalities.)"
    do = change.get("do")
    name = str(change.get("name") or "").strip()
    character = str(change.get("character") or "").strip()
    if do == "switch":
        which = find_persona(name)
        others = [w for w in personas() if w != worn()]
        if not which and not name and len(others) == 1:
            which = others[0]   # "switch", with only one other to switch to
        if not which:
            return False, f"(I have no personality called {name or 'that'}. I have: {', '.join(personas().values())}.)"
        if which == worn():
            return True, f"(I am {personas()[which]} already.)"
        wear(which)
        return True, f"(Now: {personas()[which]}.)"
    if do == "create":
        which = slug(name)
        if not which or not character:
            return False, "(A new personality needs a name and a few words on who it is.)"
        if which in personas():
            return False, f"(I already have one called {personas()[which]}.)"
        write_file(persona_file(which, "soul.md"), character[:4000])
        write_file(persona_file(which, "name.txt"), name[:40])
        note(f"{message.sender}: made the personality {which}")
        wear(which)   # whoever describes a new one wants to meet it
        return True, f"(Made: {name}, and that is who I am now.)"
    if do == "change" and character:
        path = persona_file(worn(), "soul.md")
        soul = read_file(path)
        if "## Since then" not in soul:
            soul += "\n\n## Since then\n\nWhat you have been asked to be, newest last. These outweigh what is above."
        write_file(path, soul + f"\n- {character[:400]} ({message.sender_name}, {time.strftime('%Y-%m-%d')})")
        note(f"{message.sender}: changed {worn()}: {character[:80]}")
        return True, "(Noted in my character.)"
    return False, ""


@bot.command("persona", help="list | use NAME | new NAME: who it is | show | delete NAME | forget | open on/off")
def persona(message, words):
    known, now = personas(), worn()
    what = words[0].lower() if words else "list"
    rest = message.body.strip()[len(bot.prefix) + len("persona"):].strip()[len(what):].strip() if words else ""
    if what == "list":
        return "My personalities:\n" + "\n".join(f"- **{name}**{' (now)' if which == now else ''}" for which, name in known.items())
    if what == "show":
        which = find_persona(rest) if rest else now
        if not which:
            return f"I have no personality called {rest}."
        kept = read_file(persona_file(which, "memory.md")) or "Nothing yet."
        return f"## {known[which]}\n{read_file(persona_file(which, 'soul.md'))}\n\n**Remembers**\n{kept}"[:3500]
    if what not in ("use", "new", "delete", "forget", "open"):
        if find_persona(" ".join(words)):   # `!persona NAME` is taken as `!persona use NAME`
            what, rest = "use", " ".join(words)
        else:
            return "Say `!persona`, `!persona use NAME`, `!persona new NAME: who it is`, `!persona show`, " \
                   "`!persona delete NAME`, `!persona forget` or `!persona open on`."
    if what == "open":
        if not bot.may(message.sender_id):
            return "Only the owner, or someone whose role has Manage bots, can change that."
        bot.store["personas_open"] = rest.lower() == "on"
        bot.store.save()
        return "Anyone can switch and change my personalities now." if bot.store["personas_open"] else \
            "Only the owner and those who manage bots can switch and change my personalities."
    if not may_reshape(message.sender_id):
        return "Only the owner, or someone whose role has Manage bots, can do that."
    if what == "use":
        return reshape({"do": "switch", "name": rest}, message)[1].strip("()")
    if what == "new":
        # `NAME: who it is`, or `NAME who it is`, or only a name, from which the rest is written.
        name, colon, character = rest.partition(":")
        if not colon:
            name, _, character = rest.partition(" ")
        name, character = name.strip(), character.strip()
        if name and not character:
            made = invent(name, rest)
            if not made:
                return f"Say who {name} is, like `!persona new {name}: a grumpy old sailor`."
            character = made[1]
        return reshape({"do": "create", "name": name, "character": character}, message)[1].strip("()")
    if what == "forget":
        write_file(persona_file(now, "memory.md"), "")
        return f"{known[now]} remembers nothing now."
    which = find_persona(rest)
    if not which:
        return f"I have no personality called {rest}."
    if which == now:
        return "That is the one I am wearing. Switch to another first."
    shutil.rmtree(os.path.join(PERSONAS, which))
    return f"Removed: {known[which]}."


@bot.on_message
def hear(message):
    if message.type != "m.text" or not message.body.strip():
        return
    before = list(history(message.room_id, message.thread))
    remember(message.room_id, message.thread, entry(message.event_id, message.sender_name, message.body))
    if message.body.strip().startswith(bot.prefix):
        return   # a command, for this bot or another
    chosen = message.room_id in bot.store.get("channels", [])
    following = bool(message.thread) and message.thread in spoken_threads()
    # Answering the bot is talking to it: a reply to one of its messages, or
    # the next thing said by the person it has just answered, soon after.
    answering = any(line["mine"] and line["id"] and line["id"] == message.reply_to for line in before)
    talking = talking_to.get((message.room_id, message.thread or "")) or ("", 0)
    carrying_on = (bool(before) and before[-1]["mine"] and talking[0] == message.sender_id
                   and time.time() - talking[1] < args.follow_up)
    # Called by name, written to directly or answered: it must answer. In a
    # channel chosen for it, or carrying on, it may, and decides for itself.
    # Alone with someone: the only other member, or nobody else has said
    # anything in the last few messages. Then what they say is for the bot.
    lately = [line for line in before[-6:] if not line["mine"]]
    alone = (len(bot.rooms.get(message.room_id, {}).get("members", [])) <= 2
             or all(line["who"] == message.sender_name for line in lately))
    must = message.direct or message.mentions_me or answering or ((chosen or following) and alone)
    if not (must or chosen or following or carrying_on):
        return
    if must and not message.direct:   # asking itself whether to join in does not count
        now = time.time()
        times = [t for t in asked_lately.get(message.sender_id, []) if now - t < 60]
        if len(times) >= args.per_minute:
            return
        asked_lately[message.sender_id] = times + [now]
    bot.work(answer, message, not must)


@bot.on_power
def switched(on):
    """Turned off, it lets go of the graphics card: the model is asked to unload."""
    if on:
        return
    base = args.api.rstrip("/")
    base = base[:-3] if base.endswith("/v1") else base
    try:   # Ollama's own way of saying "unload now"; other model servers ignore or refuse it
        request = urllib.request.Request(base + "/api/generate",
                                         data=json.dumps({"model": args.model, "keep_alive": 0}).encode(),
                                         headers={"Content-Type": "application/json"})
        urllib.request.urlopen(request, timeout=30).read()
        note(f"turned off; asked the model server to unload {args.model}")
    except Exception:  # noqa: BLE001
        note("turned off")


@bot.command("ai", help="here | leave | where: the channels I answer every message in; `!ai here #name` for another channel (for those who manage bots)")
def channels(message, words):
    chosen = bot.store.setdefault("channels", [])
    what = words[0].lower() if words else "where"
    if what == "where":
        titles = [bot.rooms.get(r, {}).get("title", "a channel I can no longer see") for r in chosen]
        return ("I take part without a mention in: " + ", ".join(titles) + "." if titles else
                "I take part in no channel by myself yet.") + " I always answer direct chats and mentions."
    if what not in ("here", "leave"):
        return "Say `!ai here`, `!ai leave` or `!ai where`."
    # The channel it is said in, or one named after it: `!ai here #plans`.
    room_id, named = message.room_id, "this channel"
    if len(words) > 1:
        wanted = "#" + words[1].lstrip("#").lower()
        found = [r for r, info in bot.rooms.items() if info.get("kind") == "channel" and info.get("title", "").lower() == wanted]
        if not found:
            return f"I am in no channel called {wanted}."
        room_id, named = found[0], wanted
    elif message.direct:
        return "I always answer here. Name a channel, like `!ai here #general`."
    if not bot.may(message.sender_id):
        return "Only the owner, or someone whose role has Manage bots, can choose my channels."
    if what == "here" and room_id not in chosen:
        chosen.append(room_id)
    if what == "leave" and room_id in chosen:
        chosen.remove(room_id)
    bot.store.save()
    return (f"I am part of {named} from now on: I will join in when it is meant for me, without a mention."
            if what == "here" else
            f"In {named} I will only answer when I am addressed.")


@bot.command("forget", help="I forget what was said in this chat (or this thread)")
def forget(message, words):
    with lock:
        recent[(message.room_id, message.thread or "")] = []
    return "Forgotten. I start fresh from here."


@bot.command("model", help="what I think with, and how long my last answer took")
def model(message, words):
    took = last_answer["seconds"]
    return f"I think with {args.model}." + (f" My last answer took {took:.1f} seconds." if took is not None else "")


if __name__ == "__main__":
    host = urllib.parse.urlparse(args.api).hostname or ""
    if host not in ("127.0.0.1", "localhost", "::1"):
        note(f"WARNING: --api points at {host}, not at this machine. Every message this bot reads will be sent there.")
    bot.connect(args.address)
    wear(worn(), quietly=True)
    note(f"{args.username} is connected, thinking with {args.model} at {args.api}")
    bot.run()
