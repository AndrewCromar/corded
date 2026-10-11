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
  - always in the channels chosen for it (`!ai here` in a channel, by the
    owner or someone whose role has Manage bots; `!ai leave` undoes it);
  - anywhere, when it is mentioned (@name);
  - in a thread it has already spoken in, without a new mention.
  - when someone replies to one of its messages, or says the next thing
    right after it answered them (within two minutes, nobody in between).

How it answers is the model's choice, each time: a plain message, a reply to
one message, a thread under one, or just a reaction.

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
import json
import os
import re
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


# ---- asking the model ----

def standing_text():
    if args.soul:
        soul = open(args.soul, encoding="utf-8").read().strip()
    else:
        soul = DEFAULT_SOUL.format(name=args.display_name or args.username)
    context = open(args.context, encoding="utf-8").read().strip() if os.path.exists(args.context) else ""
    return soul + ("\n\n" + context if context else "")


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
        "- `!ai here`, typed in a channel: you will answer every message there. Only the server's owner, or "
        "someone whose role has the Manage bots permission, can do it. Roles are edited in the app under "
        "Settings, Manage this server, Roles.\n"
        "- `!ai leave`, typed in a channel: you go back to answering only when addressed there.\n"
        "- `!ai where`: lists the channels chosen for you.\n"
        "- `!forget`: you forget the chat or thread it is typed in and start fresh.\n"
        "- `!model`: says which model you run on and how long your last answer took.\n"
        "- `!help`: lists these commands.\n"
        f"- You are the language model {args.model}, running on the same machine as this chat server. What you "
        "read is not sent anywhere else. You cannot browse the web, open links, see pictures or files, set "
        "reminders or remember people between chats.\n"
        f"- You are given the last {args.memory} messages of the chat or thread you are answering in, no more.\n"
        "- Your name, character and manners come from two text files the person running you can edit. Things "
        "like which model you use are set where you are started, not from the chat.")


def ask_model(message, lines):
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
    if not isinstance(decided, dict) or not ("text" in decided or "emoji" in decided or "action" in decided):
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


def answer(message):
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
    threading.Thread(target=show_typing, daemon=True).start()
    began = time.time()
    try:
        words = ask_model(message, lines)
    except Exception as error:  # noqa: BLE001
        thinking.set()
        note(f"the model did not answer: {error}")
        bot.reply(message, "I could not think just now; the model on my machine did not answer.")
        return
    finally:
        thinking.set()
    last_answer["seconds"] = time.time() - began
    talking_to[(message.room_id, message.thread or "")] = (message.sender_id, time.time())
    decided = understand(words, message.direct)
    target = message.event_id
    if decided["to"] and 1 <= decided["to"] <= len(lines) and lines[decided["to"] - 1]["id"]:
        target = lines[decided["to"] - 1]["id"]
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
    if not (message.direct or message.mentions_me or chosen or following or answering or carrying_on):
        return
    if not message.direct:
        now = time.time()
        times = [t for t in asked_lately.get(message.sender_id, []) if now - t < 60]
        if len(times) >= args.per_minute:
            return
        asked_lately[message.sender_id] = times + [now]
    bot.work(answer, message)


@bot.command("ai", help="here | leave | where: the channels I answer every message in (for those who manage bots)")
def channels(message, words):
    chosen = bot.store.setdefault("channels", [])
    what = words[0].lower() if words else "where"
    if what == "where":
        titles = [bot.rooms.get(r, {}).get("title", "a channel I can no longer see") for r in chosen]
        return ("I answer every message in: " + ", ".join(titles) + "." if titles else
                "I answer in no channel by myself yet.") + " I always answer direct chats and mentions."
    if what not in ("here", "leave"):
        return "Say `!ai here`, `!ai leave` or `!ai where`."
    if message.direct:
        return "I always answer here. That command is for channels."
    if not bot.may(message.sender_id):
        return "Only the owner, or someone whose role has Manage bots, can choose my channels."
    if what == "here" and message.room_id not in chosen:
        chosen.append(message.room_id)
    if what == "leave" and message.room_id in chosen:
        chosen.remove(message.room_id)
    bot.store.save()
    return ("I will answer every message in this channel from now on." if what == "here" else
            "I will only answer here when I am mentioned.")


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
    note(f"{args.username} is connected, thinking with {args.model} at {args.api}")
    bot.run()
