#!/usr/bin/env python3
"""Makes pictures from a description, with a model on your own machine.

    python3 image_bot.py SERVER:PORT [--api http://127.0.0.1:7860] [--username pictures]

    !imagine a lighthouse in a storm, oil painting
    !imagine a red fox in the snow --wide --seed 1234 --no blurry, text
    @pictures a teapot shaped like a snail

The bot puts an hourglass under the request, shows as typing while the
picture is made, and replies with the picture; its caption gives the words
and the seed, so the same picture can be made again. In a direct chat with
the bot, whatever you write is a description.

Options, at the end of a description: `--wide`, `--tall` or `--square`;
`--seed 1234`; `--steps 30`; `--no things to keep out`.

`!again` makes the last description of the chat once more with a new seed.
`!queue` shows who is waiting; `!cancel` takes your own request out of the
line. One picture is made at a time. Each person gets five in ten minutes,
unless those who manage bots say otherwise: `!images limit 20`, or
`!images limit 0` for no limit.

Where it works: everywhere it is, until channels are chosen for it. The
owner, or anyone whose role has Manage bots, says `!images here` in a channel
(or `!images here #name`); from then on it makes pictures only in chosen
channels and in direct chats. `!images leave` undoes one, `!images where`
lists them.

Adult pictures: refused everywhere to begin with. `!images adult nsfw` (by
those who manage bots) allows them in channels marked NSFW and nowhere else;
`!images adult off` goes back. The refusal goes by the words asked for; a
picture model can still surprise, which is one more reason to choose its
channels.

The pictures are made by a program on this machine that the bot asks over
HTTP: `image_server.py` beside this file, or any program that speaks the
AUTOMATIC1111 web API (Forge, SD.Next). Descriptions go there and nowhere
else. If a chat model is loaded in Ollama on the same graphics card, the bot
asks Ollama to set it down first (`--ollama ''` to leave it alone).
"""
import argparse
import base64
import json
import os
import re
import tempfile
import threading
import time
import urllib.parse
import urllib.request

from corded_bot import Bot

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = {"square": (1024, 1024), "wide": (1216, 832), "tall": (832, 1216)}
PER_PERSON, WITHIN, LINE = 5, 600, 10
AVOID = "blurry, low quality, jpeg artifacts, watermark, signature, text, deformed, extra fingers"
ADULT_WORDS = re.compile(
    r"\b(nsfw|nude|nudes|naked|topless|porn\w*|hentai|sex|sexual|sexy|erotic\w*|explicit|genital\w*|nipple\w*|"
    r"penis|vagina|breasts?|boobs?|lingerie|fetish|bdsm|cum|orgasm\w*)\b", re.IGNORECASE)

parser = argparse.ArgumentParser()
parser.add_argument("address")
parser.add_argument("--api", default="http://127.0.0.1:7860", help="the picture program on this machine")
parser.add_argument("--ollama", default="http://127.0.0.1:11434",
                    help="an Ollama on the same graphics card, asked to unload before a picture ('' for none)")
parser.add_argument("--username", default="pictures")
parser.add_argument("--display-name", default="Pictures")
parser.add_argument("--picture", default=os.path.join(HERE, "image_bot.png"))
parser.add_argument("--vault", default=None)
parser.add_argument("--steps", type=int, default=25)
parser.add_argument("--avoid", default=None, help="a text file of things kept out of every picture")
args = parser.parse_args()

bot = Bot(vault=args.vault or f"./{args.username}-vault", username=args.username, display_name=args.display_name,
          picture=args.picture if args.picture and os.path.exists(args.picture) else None,
          about="I make pictures from a description, on this server's own machine. "
                "`!imagine a lighthouse in a storm`, or write to me directly. `!help` for the rest.")
lock = threading.Lock()
line = []           # requests waiting or being made, oldest first
asked = {}          # user id -> when they asked, for the limit
last_words = {}     # room id -> the last description made there


def note(*words):
    print(*words, flush=True)


def parse(text):
    """A description and its options: (what to draw, settings) or (None, why not)."""
    settings = {"size": "square", "seed": -1, "steps": args.steps, "no": ""}
    parts = re.split(r"\s--(?=[a-z])", " " + text.strip())
    prompt = parts[0].strip()
    for part in parts[1:]:
        name, _, value = part.partition(" ")
        name, value = name.lower(), value.strip()
        if name in SIZES:
            settings["size"] = name
            prompt_extra = value   # words after a bare option belong to nothing
            if prompt_extra:
                return None, f"`--{name}` takes no words after it."
        elif name == "seed" and value.isdigit():
            settings["seed"] = int(value)
        elif name == "steps" and value.isdigit():
            settings["steps"] = max(5, min(40, int(value)))
        elif name == "no" and value:
            settings["no"] = value
        else:
            return None, f"I do not know `--{part.strip()}`. The options are `--wide`, `--tall`, `--square`, " \
                         "`--seed 1234`, `--steps 30` and `--no things to keep out`."
    if not prompt:
        return None, "Say what to draw, like `!imagine a lighthouse in a storm, oil painting`."
    return prompt[:1000], settings


def adult_allowed(message):
    return bot.store.get("adult", "off") == "nsfw" and bool(bot.rooms.get(message.room_id, {}).get("nsfw"))


def works_here(message):
    chosen = bot.store.get("channels", [])
    return message.direct or not chosen or message.room_id in chosen


def free_the_card():
    """Asks Ollama to set down whatever it holds on the graphics card."""
    if not args.ollama:
        return
    try:
        with urllib.request.urlopen(args.ollama.rstrip("/") + "/api/ps", timeout=5) as answer:
            loaded = [m.get("name") for m in json.load(answer).get("models", [])]
        for name in loaded:
            request = urllib.request.Request(args.ollama.rstrip("/") + "/api/generate",
                                             data=json.dumps({"model": name, "keep_alive": 0}).encode(),
                                             headers={"Content-Type": "application/json"})
            urllib.request.urlopen(request, timeout=30).read()
            note(f"asked Ollama to unload {name}")
    except Exception:  # noqa: BLE001  (no Ollama here: nothing to free)
        pass


def make(prompt, settings, adult):
    width, height = SIZES[settings["size"]]
    avoid = AVOID
    if args.avoid and os.path.exists(args.avoid):
        avoid = open(args.avoid, encoding="utf-8").read().strip() or AVOID
    avoid = ", ".join(x for x in (settings["no"], avoid, "" if adult else "nsfw, nude, naked, explicit") if x)
    request = urllib.request.Request(
        args.api.rstrip("/") + "/sdapi/v1/txt2img",
        data=json.dumps({"prompt": prompt, "negative_prompt": avoid, "width": width, "height": height,
                         "steps": settings["steps"], "seed": settings["seed"], "cfg_scale": 7}).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=900) as answer:
        result = json.load(answer)
    info = result.get("info") or {}
    info = json.loads(info) if isinstance(info, str) else info
    return base64.b64decode(result["images"][0]), info.get("seed", settings["seed"])


def job(entry):
    message, prompt, settings = entry["message"], entry["prompt"], entry["settings"]
    with lock:
        if entry["cancelled"]:
            line.remove(entry)
            return
        entry["making"] = True
    working = threading.Event()

    def show_typing():
        while not working.is_set():
            bot.typing(message.room_id)
            working.wait(4)
    threading.Thread(target=show_typing, daemon=True).start()
    began = time.time()
    path = None
    try:
        free_the_card()
        picture, seed = make(prompt, settings, entry["adult"])
        took = time.time() - began
        handle, path = tempfile.mkstemp(prefix="corded-picture-", suffix=".png")
        with os.fdopen(handle, "wb") as f:
            f.write(picture)
        extras = "".join(f" --{settings['size']}" if settings["size"] != "square" else "")
        caption = f"**{prompt}**\n`--seed {seed}{extras}` · {took:.0f} s"
        bot.send_file(message.room_id, path, caption, reply_to=message.event_id, thread=message.thread)
        last_words[message.room_id] = (prompt, settings)
        # Nothing of a picture stays behind: the file is removed below, the
        # picture program keeps none, and the bot's vault holds only the
        # message that carried it.
        note(f"{message.sender}: a picture in {took:.0f}s: {prompt[:80]}")
    except Exception as error:  # noqa: BLE001
        note(f"no picture: {error}")
        bot.attempt(bot.reply, message, "I could not make that picture: the picture program on my machine "
                                        "did not answer. Whoever runs me can see why in my log.")
    finally:
        working.set()
        if path and os.path.exists(path):
            os.remove(path)
        bot.unreact(message, entry["waiting"])
        with lock:
            if entry in line:
                line.remove(entry)


def ask(message, text, again=False):
    if not works_here(message):
        return None   # not its place: silence, as for any other chatter
    parsed, settings = parse(text)
    if parsed is None:
        return settings
    adult = adult_allowed(message)
    if not adult and ADULT_WORDS.search(parsed):
        if bot.store.get("adult", "off") == "nsfw":
            return "I only make adult pictures in channels marked NSFW."
        return "I do not make adult pictures here."
    now = time.time()
    times = [t for t in asked.get(message.sender_id, []) if now - t < WITHIN]
    limit = bot.store.get("limit", PER_PERSON)
    if limit and len(times) >= limit:
        wait = int((WITHIN - (now - times[0])) // 60) + 1
        return f"That is {limit} pictures in ten minutes; ask again in about {wait} minute{'s' if wait != 1 else ''}."
    with lock:
        if len(line) >= LINE:
            return "The line is full; try again in a few minutes."
        asked[message.sender_id] = times + [now]
        entry = {"message": message, "prompt": parsed, "settings": settings, "adult": adult, "cancelled": False,
                 "making": False, "waiting": None, "who": message.sender_name}
        line.append(entry)
        ahead = len(line) - 1
    entry["waiting"] = bot.attempt(bot.react, message, "⏳")
    bot.work(job, entry)
    if ahead:
        return f"{ahead} ahead of you."
    return None


@bot.command("imagine", help="a description: I make the picture. Options: --wide --tall --seed N --steps N --no things")
def imagine(message, words):
    return ask(message, message.body.strip()[len(bot.prefix) + len("imagine"):])


@bot.command("again", help="the last description of this chat once more, with a new seed")
def again(message, words):
    if message.room_id not in last_words:
        return "Nothing has been made here yet." if works_here(message) else None
    prompt, settings = last_words[message.room_id]
    options = f" --{settings['size']} --steps {settings['steps']}" + (f" --no {settings['no']}" if settings["no"] else "")
    return ask(message, prompt + options)


@bot.command("queue", help="who is waiting for a picture")
def queue_(message, words):
    with lock:
        waiting = [("now: " if e["making"] else f"{i}. ") + f"{e['who']}: {e['prompt'][:60]}"
                   for i, e in enumerate(line) if not e["cancelled"]]
    return "\n".join(waiting) if waiting else "Nobody is waiting."


@bot.command("cancel", help="takes your own request out of the line")
def cancel(message, words):
    with lock:
        mine = [e for e in line if e["message"].sender_id == message.sender_id and not e["making"] and not e["cancelled"]]
        for e in mine:
            e["cancelled"] = True
    for e in mine:
        bot.unreact(e["message"], e["waiting"])
    if mine:
        return f"Taken out of the line: {len(mine)}."
    return "You have nothing waiting. A picture that is already being made cannot be stopped."


@bot.command("images", help="here | leave | where | adult off | adult nsfw | limit N, 0 for none (for those who manage bots)")
def images(message, words):
    chosen = bot.store.setdefault("channels", [])
    what = words[0].lower() if words else "where"
    if what == "where":
        titles = [bot.rooms.get(r, {}).get("title", "a channel I can no longer see") for r in chosen]
        where = "I make pictures in: " + ", ".join(titles) + ", and in direct chats." if titles else \
                "I make pictures wherever I am; no channels have been chosen."
        adult = {"off": "Adult pictures: nowhere.", "nsfw": "Adult pictures: only in channels marked NSFW."}
        limit = bot.store.get("limit", PER_PERSON)
        return (where + " " + adult[bot.store.get("adult", "off")] + " " +
                (f"Each person: {limit} in ten minutes." if limit else "No limit on how many."))
    if what not in ("here", "leave", "adult", "limit"):
        return ("Say `!images here`, `!images leave`, `!images where`, `!images adult off`, `!images adult nsfw` "
                "or `!images limit 5`.")
    if not bot.may(message.sender_id):
        return "Only the owner, or someone whose role has Manage bots, can change that."
    if what == "limit":
        if len(words) < 2 or not words[1].isdigit():
            return "Say `!images limit 5`: pictures for each person in ten minutes. `!images limit 0` is no limit."
        bot.store["limit"] = int(words[1])
        bot.store.save()
        return ("No limit on pictures now." if bot.store["limit"] == 0 else
                f"Each person gets {bot.store['limit']} pictures in ten minutes.")
    if what == "adult":
        if len(words) < 2 or words[1].lower() not in ("off", "nsfw"):
            return "Say `!images adult off` or `!images adult nsfw`."
        bot.store["adult"] = words[1].lower()
        bot.store.save()
        return "Adult pictures: only in channels marked NSFW." if bot.store["adult"] == "nsfw" else "Adult pictures: nowhere."
    room_id, named = message.room_id, "this channel"
    if len(words) > 1:
        wanted = "#" + words[1].lstrip("#").lower()
        found = [r for r, info in bot.rooms.items() if info.get("kind") == "channel" and info.get("title", "").lower() == wanted]
        if not found:
            return f"I am in no channel called {wanted}."
        room_id, named = found[0], wanted
    elif message.direct:
        return "I always work here. Name a channel, like `!images here #art`."
    if what == "here" and room_id not in chosen:
        chosen.append(room_id)
    if what == "leave" and room_id in chosen:
        chosen.remove(room_id)
    bot.store.save()
    if what == "here":
        return f"I make pictures in {named}. From now on only in chosen channels and direct chats."
    return f"No more pictures in {named}." + ("" if chosen else " No channel is chosen now, so I work wherever I am.")


@bot.on_message
def hear(message):
    """A description without the command: after a mention, or in a direct chat."""
    body = message.body.strip()
    if message.type != "m.text" or not body or body.startswith(bot.prefix):
        return
    if message.mentions_me:
        body = re.sub(rf"@{re.escape(args.username)}\b[,:]?", "", body, flags=re.IGNORECASE).strip()
    elif not message.direct:
        return
    if body.lower() in ("help", "hi", "hello", "?"):
        bot.reply(message, "Tell me what to draw, like `a lighthouse in a storm, oil painting`. "
                           "`!help` lists the rest.")
        return
    answer = ask(message, body)
    if answer:
        bot.reply(message, answer)


if __name__ == "__main__":
    host = urllib.parse.urlparse(args.api).hostname or ""
    if host not in ("127.0.0.1", "localhost", "::1"):
        note(f"WARNING: --api points at {host}, not at this machine. Every description will be sent there.")
    bot.connect(args.address)
    note(f"{args.username} is connected, making pictures with {args.api}")
    bot.run()
