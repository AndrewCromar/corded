#!/usr/bin/env python3
"""Fetches the video behind a link and posts the file in a thread under it.

    python3 video_bot.py SERVER:PORT [--yt-dlp /path/to/yt-dlp] [--channels '#clips,#general']

The bot does nothing until it is mentioned. Then it looks for a link: in the
message that mentions it, or else in the message that one replies to, or the
message whose thread it is in. It fetches the video and posts the file in the
thread under the message that holds the link, also when that message is itself
in a thread. The word "audio" beside the mention gets the sound only. In a
direct chat with the bot a link is enough.

All of the work happens on the machine the bot runs on. The fetching is done
by yt-dlp, a program that knows well over a thousand video sites, and ffmpeg,
which joins picture and sound; both run here, and the only other machine
spoken to is the site the video is on. Nothing is sent to any service in
between. yt-dlp is found from --yt-dlp, from YT_DLP, beside this file, or on
PATH; get it from https://github.com/yt-dlp/yt-dlp/releases.

Limits, kept in the bot's files and changed in a direct chat by the server's
owner or anyone with the Manage bots permission (`limit minutes 30`,
`limit size 300`, `limit quality 1080`): 20 minutes, 200 MB and 720p to begin
with. One video for each request: no playlists, no live streams. Five
requests in ten minutes for each person.

Adult sites, and single videos a site marks as 18+, are fetched only in a
channel marked NSFW or in a direct chat with the bot. Those who manage bots
change that with `adult off` (nowhere), `adult anywhere` or `adult nsfw`, and
name more sites with `adult add example.com`.

It only fetches from the public web: an address that leads to this machine or
to the network it is on is refused. What people fetch is theirs to answer for,
and so is whether a site allows it.
"""
import argparse
import collections
import ipaddress
import json
import os
import re
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.parse

from corded_bot import Bot

LINK = re.compile(r"https?://[^\s<>]+", re.IGNORECASE)
DEFAULTS = {"minutes": 20, "size": 200, "quality": 720}
PER_PERSON, WITHIN = 5, 600   # requests, seconds

# Sites that are for adults throughout. yt-dlp also marks what it fetches from
# the adult sites it knows, and single videos elsewhere, as 18+, and that mark
# counts too; this list lets the bot say no before anything is fetched, and
# covers sites yt-dlp only reaches through the video file in the page.
ADULT_SITES = """
4tube.com alphaporno.com beeg.com bongacams.com cam4.com camsoda.com chaturbate.com clips4sale.com daftsex.com
drtuber.com e-hentai.org e621.net efukt.com empflix.com eporner.com erocast.me erome.com eroprofile.com fansly.com
fapello.com fux.com gelbooru.com hanime.tv hclips.com hdzog.com heavy-r.com hellporno.com hqporner.com iwara.tv
lovehomeporn.com manyvids.com motherless.com moviefap.com murrtube.net nhentai.net noodlemagazine.com nubiles-porn.com
nuvid.com onlyfans.com peekvids.com playvids.com pornbox.com pornerbros.com pornflip.com pornhub.com pornhub.org
pornone.com porntop.com porntrex.com porntube.com redgifs.com redtube.com rule34.xxx rule34video.com slutload.com
spankbang.com stripchat.com sunporno.com sxyprn.com thisvid.com tnaflix.com tube8.com tubepornclassic.com txxx.com
upornia.com vjav.com xfreehd.com xhamster.com xnxx.com xvideos.com xxxymovies.com youjizz.com youporn.com zenporn.com
""".split()
ADULT_ENDINGS = (".xxx", ".porn", ".sex", ".adult", ".sexy")
ADULT_WORDS = re.compile(r"porn|hentai|xxx")


def site_of(url):
    """The site of an address, as someone would name it: no "www." and no path."""
    host = urllib.parse.urlsplit(url if "://" in url else "//" + url).hostname or ""
    return host.lower().removeprefix("www.")


def first_link(text):
    """The first web address in a message, without the punctuation that ends a sentence."""
    found = LINK.search(text or "")
    return found.group(0).rstrip(".,;:!?)]}'\"") if found else None


def address_problem(url):
    """Why this address will not be fetched, or None if it may be: only the
    public web, never this machine or the network it is on."""
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in ("http", "https") or not parts.hostname:
        return "That is not a web address I can fetch."
    try:
        found = socket.getaddrinfo(parts.hostname, parts.port or 443, proto=socket.IPPROTO_TCP)
    except OSError:
        return f"I can't find the site `{parts.hostname}`."
    for entry in found:
        address = ipaddress.ip_address(entry[4][0].split("%")[0])
        address = getattr(address, "ipv4_mapped", None) or address
        if not address.is_global:
            return "That address is on a private network. I only fetch from the public web."
    return None


def length_in_words(seconds):
    seconds = int(seconds or 0)
    hours, rest = divmod(seconds, 3600)
    return (f"{hours}:{rest // 60:02d}:{rest % 60:02d}" if hours else f"{rest // 60}:{rest % 60:02d}")


def size_in_words(size):
    return f"{size / 1e6:.1f} MB" if size < 10e6 else f"{size / 1e6:.0f} MB"


def find_yt_dlp(given):
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (given, os.environ.get("YT_DLP", ""), os.path.join(here, "yt-dlp")):
        if candidate and os.path.isfile(candidate):
            return os.path.abspath(candidate)
    return shutil.which("yt-dlp")


class Refused(Exception):
    """A request the bot will not or cannot carry out; its words are for the person who asked."""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("address")
    parser.add_argument("--yt-dlp", default=None, help="where yt-dlp is (else YT_DLP, beside this file, or PATH)")
    parser.add_argument("--channels", default=None,
                        help="the channels it works in, separated by commas (default: every one it can see)")
    parser.add_argument("--max-minutes", type=int, default=None, help="20 if never chosen; kept in the bot's files")
    parser.add_argument("--max-size", type=int, default=None, help="in MB; 200 if never chosen")
    parser.add_argument("--max-height", type=int, default=None, help="720 if never chosen")
    parser.add_argument("--username", default="clips")
    parser.add_argument("--display-name", default="Clips", help="the name people see")
    parser.add_argument("--picture", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "video_bot.png"),
                        help="its profile picture, a small square image ('' for none)")
    parser.add_argument("--vault", default=None)
    args = parser.parse_args()

    yt_dlp = find_yt_dlp(args.yt_dlp)
    if not yt_dlp:
        parser.error("yt-dlp was not found; say where it is with --yt-dlp (it is at "
                     "https://github.com/yt-dlp/yt-dlp/releases)")
    channels = {"#" + name.strip().lstrip("#") for name in (args.channels or "").split(",") if name.strip()}
    # Some sites (YouTube among them) hand out their videos through a script
    # that has to be run. yt-dlp looks for deno by itself; node serves if it is here.
    node = None if shutil.which("deno") else shutil.which("node")
    runtime = ["--js-runtimes", f"node:{node}"] if node else []

    bot = Bot(vault=args.vault or f"./{args.username}-vault", username=args.username,
              display_name=args.display_name, picture=args.picture,
              about="Mention me with a link to a video and I'll post the file in a thread under it. "
                    "Everything is fetched by the machine I run on.")

    # The limits are kept in the bot's files. A flag counts when it is new or
    # has changed, so that what a manager chose since is not undone by
    # starting the bot again with the same command.
    limits = bot.store.setdefault("limits", dict(DEFAULTS))
    given = bot.store.setdefault("limits_given", {})
    for name, flag in (("minutes", args.max_minutes), ("size", args.max_size), ("quality", args.max_height)):
        limits.setdefault(name, DEFAULTS[name])
        if flag is not None and flag != given.get(name):
            limits[name] = given[name] = flag
    bot.store.save()

    # Adult sites: "nsfw" (only in channels marked NSFW and in direct chats),
    # "off" (nowhere) or "anywhere"; and the sites a manager has named besides.
    bot.store.setdefault("adult", "nsfw")
    more_adult = bot.store.setdefault("adult_sites", [])

    def adult_site(url):
        site = site_of(url)
        return bool(site) and (any(site == known or site.endswith("." + known) for known in ADULT_SITES + more_adult)
                               or site.endswith(ADULT_ENDINGS) or bool(ADULT_WORDS.search(site)))

    def adult_allowed(message):
        marked = bot.rooms.get(message.room_id, {}).get("nsfw")
        return bot.store["adult"] == "anywhere" or (bot.store["adult"] == "nsfw" and bool(message.direct or marked))

    def adult_refusal(what):
        return what + (", and I don't fetch those on this server." if bot.store["adult"] == "off" else
                       ", and I only fetch those in a channel marked NSFW or in a direct chat with me.")

    def adult_in_words():
        return {"nsfw": "adult sites, and videos a site marks 18+, only in channels marked NSFW or in a direct chat "
                        "with me",
                "off": "nothing from adult sites, and no video a site marks 18+",
                "anywhere": "adult sites in any channel"}[bot.store["adult"]]

    title = f"## 🎬 {args.display_name}"
    asked = collections.defaultdict(collections.deque)   # user id -> when they last asked
    server = {"size": 0}   # the largest file the server takes, in MB, once known; 0 for not known or no limit

    def cap():
        """The largest file to fetch, in MB: the bot's limit, or the server's if that is smaller."""
        return min(limits["size"], server["size"]) if server["size"] else limits["size"]

    def whose_cap():
        return "the most this server takes in one file" if cap() < limits["size"] else "my limit"

    def limits_in_words():
        return (f"- up to **{limits['minutes']} minutes** and **{cap()} MB**, at **{limits['quality']}p** "
                "or the nearest below\n"
                "- one video for each request: no playlists, no live streams\n"
                f"- {PER_PERSON} requests in {WITHIN // 60} minutes for each person\n"
                f"- {adult_in_words()}")

    def help_for(user_id):
        lines = [
            title,
            "Mention me with a link to a video and I'll post the video file in a thread under it.",
            "",
            "**How to ask**",
            f"- `@{bot.username} https://…`: with the link in the same message",
            f"- reply to a message that has a link, or write in its thread, and mention me: `@{bot.username}`",
            f"- add `audio` for the sound only: `@{bot.username} audio https://…`",
            "- here in a direct chat, the link alone is enough",
            "",
            "**Limits**",
            limits_in_words(),
            "",
            "I know most video sites. Some want an account or forbid it, and then I say what they said.",
        ]
        if bot.may(user_id):
            lines += [
                "",
                "**Because you manage bots here**",
                "- `limits`: what the limits are now",
                "- `limit minutes 30`: the longest video I fetch",
                "- `limit size 300`: the largest file, in MB",
                "- `limit quality 1080`: the picture height I aim for (360, 480, 720, 1080, …)",
                "- `adult nsfw`, `adult off`, `adult anywhere`: where I fetch from adult sites",
                "- `adult add example.com`, `adult remove example.com`: sites I count as adult besides those I know",
            ]
        return "\n".join(lines)

    def manage(message, words):
        """What the owner and those with the Manage bots permission may tell it."""
        if not bot.may(message.sender_id):
            return "Only the server's owner and people with the **Manage bots** permission can do that."
        if words[0] == "adult":
            if len(words) > 1 and words[1] in ("nsfw", "off", "anywhere"):
                bot.store["adult"] = words[1]
                bot.store.save()
                return f"Done: {adult_in_words()}. I'll remember that when I'm restarted."
            if len(words) > 2 and words[1] in ("add", "remove"):
                site = site_of(words[2])
                if "." not in site:
                    return "I can't read that as a site. Write it like `adult add example.com`."
                if words[1] == "add" and not adult_site(site):
                    more_adult.append(site)
                elif words[1] == "remove" and site in more_adult:
                    more_adult.remove(site)
                elif words[1] == "remove" and adult_site(site):
                    return f"`{site}` is one of the sites I know as adult from the start; I keep those."
                bot.store.save()
                return (f"Done: I count `{site}` as an adult site." if adult_site(site)
                        else f"Done: I no longer count `{site}` as an adult site.")
            return ("**Adult sites**\n"
                    f"- now: {adult_in_words()}\n"
                    f"- I know {len(ADULT_SITES)} adult sites from the start, and take a site's own 18+ mark too"
                    + (f"\n- named here besides: {', '.join(f'`{site}`' for site in more_adult)}" if more_adult else "")
                    + "\n\nTo change: `adult nsfw`, `adult off`, `adult anywhere`, `adult add example.com`, "
                      "`adult remove example.com`.")
        if words[0] == "limits" or len(words) == 1:
            return ("**Limits**\n" + limits_in_words() +
                    "\n\nTo change one: `limit minutes 30`, `limit size 300`, `limit quality 1080`.")
        name = {"minutes": "minutes", "minute": "minutes", "length": "minutes", "size": "size", "mb": "size",
                "quality": "quality", "height": "quality"}.get(words[1])
        number = words[2].rstrip("pmb") if len(words) > 2 else ""
        if not name or not number.isdigit() or not 0 < int(number) < 100000:
            return "I didn't follow that. Write it like `limit minutes 30`, `limit size 300` or `limit quality 1080`."
        limits[name] = int(number)
        bot.store.save()
        return "Done. I'll remember that when I'm restarted.\n\n**Limits**\n" + limits_in_words()

    def run(arguments, folder, timeout, watch=False):
        """Runs yt-dlp with these arguments in a folder of its own and returns
        (exit code, what it printed, what it complained of). It is stopped if
        it takes too long or, when watched, writes more than the size limit."""
        command = [yt_dlp, "--ignore-config", "--no-plugin-dirs", "--no-cache-dir", "--no-exec", "--no-playlist",
                   "--socket-timeout", "20", "--retries", "2", "--no-progress", "--color", "never", *runtime,
                   *arguments]
        ceiling = cap() * 1e6 * 2.5   # while picture and sound are joined, the parts and the result are all there
        with open(os.path.join(folder, ".out"), "w+") as out, open(os.path.join(folder, ".err"), "w+") as err:
            process = subprocess.Popen(command, cwd=folder, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
            end = time.time() + timeout
            stopped = None
            while process.poll() is None:
                time.sleep(0.5)
                written = sum(entry.stat().st_size for entry in os.scandir(folder) if entry.is_file())
                if time.time() > end:
                    stopped = "It took too long, so I stopped."
                elif watch and written > ceiling:
                    stopped = f"It grew well past {cap()} MB, {whose_cap()}, so I stopped."
                if stopped and process.poll() is None:   # not if it finished meanwhile
                    process.kill()
                    process.wait()
                    raise Refused(stopped)
            out.seek(0)
            err.seek(0)
            return process.returncode, out.read(), err.read()

    def complaint(err):
        """The last thing yt-dlp called an error, short enough for a chat."""
        errors = [line for line in err.splitlines() if line.startswith("ERROR:")]
        words = re.sub(r"^ERROR:\s*(\[[^\]]*\]\s*)?([\w-]+:\s)?", "", errors[-1]) if errors else "it gave no reason"
        if re.search(r"logged[- ]in|log ?in|sign ?in|cookies|credentials|members[- ]only|private video", words, re.I):
            return "it wants an account for this video, and I have none."
        # yt-dlp's advice on its own options is of no use to someone in a chat.
        return re.split(r"\. (?:See|Use|Try|Pass) |; please report", words)[0][:300]

    def fetch(url, audio, folder, adult_ok):
        """Fetches one video into folder and returns (path, what yt-dlp knows about it)."""
        about = None
        heights = [limits["quality"]] + [h for h in (480, 360, 240) if h < limits["quality"]]
        for height in ([0] if audio else heights):
            # The picture and sound most players can show, at this height or the nearest below.
            kind = (["--extract-audio", "--audio-format", "mp3", "--format", "ba/b"] if audio else
                    ["--format-sort", f"vcodec:h264,res:{height},acodec:m4a", "--merge-output-format", "mp4"])
            # First what it would fetch and how large that is, without fetching.
            code, out, err = run(kind + ["--dump-single-json", "--flat-playlist", "--", url], folder, 90)
            try:
                chosen = json.loads(out) if code == 0 else None
            except ValueError:
                chosen = None
            if not chosen:
                raise Refused(f"The site would not give it to me: {complaint(err)}")
            if about is None:
                about = chosen
                if not adult_ok and ((about.get("age_limit") or 0) >= 18
                                     or adult_site(about.get("webpage_url") or "")):
                    raise Refused(adult_refusal("That video is marked 18+"))
                if about.get("_type") == "playlist":
                    raise Refused("That is a list of videos. I fetch one at a time: send me the link of a single "
                                  "video.")
                if about.get("is_live") or about.get("live_status") in ("is_live", "is_upcoming"):
                    raise Refused("That is a live stream, and I only fetch videos that have ended.")
                if (about.get("duration") or 0) > limits["minutes"] * 60:
                    raise Refused(f"That video is {length_in_words(about['duration'])} long, and my limit is "
                                  f"{limits['minutes']} minutes.")
            parts = chosen.get("requested_formats") or [chosen]
            sizes = [part.get("filesize") or part.get("filesize_approx") or 0 for part in parts]
            if all(sizes) and sum(sizes) > cap() * 1e6:
                continue   # too large at this height: try the next one down
            # Then the fetching. A size the site did not tell is watched as the file grows.
            code, out, err = run(kind + ["--restrict-filenames", "--trim-filenames", "80", "--no-mtime",
                                         "--paths", folder, "--output", "%(title).60s [%(id)s].%(ext)s",
                                         "--print", "after_move:filepath", "--", url], folder, 900, watch=True)
            printed = [line.strip() for line in out.splitlines() if line.strip()]
            path = printed[-1] if printed else ""
            if code != 0 or not os.path.isfile(path):
                raise Refused(f"The site would not give it to me: {complaint(err)}")
            if os.path.getsize(path) <= cap() * 1e6:
                return path, about
            for entry in os.scandir(folder):
                if entry.is_file() and not entry.name.startswith("."):
                    os.remove(entry.path)
        raise Refused(f"That video is larger than {cap()} MB, {whose_cap()}, even at the lowest quality I try.")

    def job(message, holder, url, audio, waiting, adult_ok):
        """One request, from the link to the file in the thread. Run one at a time."""
        # Always under the message that holds the link, also when that message
        # is itself in a thread: threads go as deep as they are needed.
        thread = holder.event_id
        folder = tempfile.mkdtemp(prefix="corded-clip-")
        try:
            for attempt in (1, 2):
                bot.typing(message.room_id)
                path, about = fetch(url, audio, folder, adult_ok)
                bot.typing(message.room_id)
                site = about.get("webpage_url_domain") or site_of(url)
                facts = [length_in_words(about.get("duration")) if about.get("duration") else "",
                         size_in_words(os.path.getsize(path)), site, "sound only" if audio else ""]
                caption = f"**{(about.get('title') or 'Untitled')[:200]}**\n" + " · ".join(filter(None, facts))
                try:
                    bot.send_file(message.room_id, path, caption, thread=thread)
                    break
                except RuntimeError as error:
                    # The server has a limit of its own for one file. Once it has
                    # said what that is, the bot fetches a smaller copy.
                    allowed = re.search(r"larger than this server allows \((\d+) MB\)", str(error))
                    if not allowed or attempt == 2:
                        raise
                    server["size"] = int(allowed.group(1))
                    os.remove(path)
        except Refused as refusal:
            bot.say(message.room_id, str(refusal), thread=thread)
        except Exception as error:  # noqa: BLE001 - the person is told, and the next request still runs
            bot.say(message.room_id, f"Something went wrong on my side: {error}", thread=thread)
            raise
        finally:
            shutil.rmtree(folder, ignore_errors=True)
            bot.unreact(holder, waiting)

    @bot.on_message
    def hear(message):
        if message.body.strip().startswith(bot.prefix):
            return   # a command, answered below
        if not message.direct:
            title = bot.rooms.get(message.room_id, {}).get("title", "")
            if not message.mentions_me or (channels and title not in channels):
                return
        words = [w for w in message.body.lower().split() if w != f"@{bot.username.lower()}"]
        if message.direct and words and words[0] in ("limit", "limits", "adult"):
            bot.reply(message, manage(message, words))   # before a link is looked for: `adult add` may name one
            return
        # The link: in this message, or in the one it answers, or the one whose thread this is.
        holder, url = message, first_link(message.body)
        for earlier in (message.reply_to, message.thread):
            if not url and earlier:
                holder = bot.lookup(message.room_id, earlier)
                url = first_link(holder.body) if holder else None
        if not url:
            if message.direct:
                bot.reply(message, help_for(message.sender_id))
            else:
                bot.reply(message, f"I didn't find a link. Mention me with one (`@{bot.username} https://…`), or "
                                   "in a reply to a message that has one.")
            return
        if adult_site(url) and not adult_allowed(message):
            bot.reply(message, adult_refusal("That link is to an adult site"))
            return
        problem = address_problem(url)
        if problem:
            bot.reply(message, problem)
            return
        recent = asked[message.sender_id]
        while recent and recent[0] < time.time() - WITHIN:
            recent.popleft()
        if len(recent) >= PER_PERSON:
            bot.reply(message, f"That's {PER_PERSON} in {WITHIN // 60} minutes. Give me a little while.")
            return
        recent.append(time.time())
        waiting = bot.attempt(bot.react, holder, "⏳")
        ahead = bot.work(job, message, holder, url, "audio" in words, waiting, adult_allowed(message))
        if ahead:
            bot.say(message.room_id, f"In line: {ahead} ahead of you.", thread=holder.event_id)

    @bot.command("help", help="what I do")
    def help_(message, words):
        if message.direct:
            return help_for(message.sender_id)
        return "\n".join([
            title,
            f"Mention me with a link to a video (`@{bot.username} https://…`), or in a reply to a message that has "
            "one, and I'll post the video file in a thread under it.",
            "",
            limits_in_words(),
            "",
            "Send me `help` in a direct chat for the rest.",
        ])

    bot.connect(args.address)
    try:   # the server's limit for one file, if a member may read it; else it is learned the first time it is met
        for entry in bot.request({"cmd": "get_settings"}).get("settings", []):
            if entry.get("key") == "max_file_mb" and str(entry.get("value", "")).isdigit():
                server["size"] = int(entry["value"])
    except RuntimeError:
        pass
    print(f"{args.username} is connected, fetching with {yt_dlp}", flush=True)
    bot.run()


if __name__ == "__main__":
    main()
