#!/usr/bin/env python3
"""Asks new members for their birthday and wishes them a happy one.

    python3 birthday_bot.py SERVER:PORT [--channel '#birthdays'] [--at 09:00]
                            [--greet-existing]

The channel it announces in and the time of day are kept in its files:
#general and 09:00 to begin with, --channel and --at to choose others at the
start, and afterwards whatever those who manage bots tell it with
`channel #name` and `time 08:30`.

When someone joins the server the bot opens a direct chat and asks, once, for
their birthday. They answer there (05-17, 17 May, 2004-05-17) or put it in
their profile; an answer in the chat wins. On the day, at --at by this
machine's clock, the bot wishes them in the direct chat and in the channel.

In the direct chat the bot also understands: help, when, forget, no (never
ask again), private (no post in the channel), public. In a channel, !birthdays
lists the next few and !help says what the bot is for.

The server's owner, and anyone with a role that has the Manage bots
permission, can also send it, in a direct chat: list, channel #name, time
08:30, reset wishes, reset asked, reset @name, set @name 05-17, ask @name, ask
everyone.

Its messages use the marks the apps draw as styling: **bold**, `code`, lines
starting with "- " as a list and "## " as a heading.

Birthdays told to the bot are kept in its folder on this machine (store.json
beside its vault) and are sent nowhere.
"""
import argparse
import datetime
import os
import re
import threading
import time

from corded_bot import Bot

MONTHS = ["january", "february", "march", "april", "may", "june", "july", "august", "september", "october",
          "november", "december"]
DATES = ("- `05-17` (month-day) or `17 May`\n"
         "- `2004-05-17` to have your age shown")


def parse_birthday(text, this_year):
    """What someone wrote, as "MM-DD" or "YYYY-MM-DD"; None if it is not a
    date. Day and month as two bare numbers are read month first, as in
    2004-05-17; a month's name removes the doubt."""
    text = text.strip().lower().replace(",", " ")
    year = month = day = None
    numbers = re.fullmatch(r"(?:(\d{4})-)?(\d{1,2})-(\d{1,2})", text)
    if numbers:
        year, month, day = numbers.group(1), int(numbers.group(2)), int(numbers.group(3))
    else:
        words = [w for w in re.sub(r"(\d)(st|nd|rd|th)\b", r"\1", text).split() if w != "of"]
        named = [w for w in words if len(w) >= 3 and any(m.startswith(w) for m in MONTHS)]
        digits = [w for w in words if w.isdigit()]
        if len(named) != 1 or len(digits) not in (1, 2) or len(words) != 1 + len(digits):
            return None
        month = next(i for i, m in enumerate(MONTHS, 1) if m.startswith(named[0]))
        day = int(next(w for w in digits if len(w) <= 2)) if any(len(w) <= 2 for w in digits) else None
        year = next((w for w in digits if len(w) == 4), None)
        if day is None or (len(digits) == 2 and year is None):
            return None
    try:
        datetime.date(int(year) if year else 2004, month, day)   # 2004 had a 29 February
    except ValueError:
        return None
    if year and not 1900 < int(year) <= this_year:
        return None
    return (f"{year}-" if year else "") + f"{month:02d}-{day:02d}"


def parse_time(text):
    """A time of day as someone wrote it (9:00, 09:00, 9am, 9:30 pm), as
    "HH:MM"; None if it is not one."""
    text = text.strip().lower().replace(" ", "").replace(".", "")
    for form in ("%H:%M", "%I:%M%p", "%I%p"):
        try:
            return datetime.datetime.strptime(text, form).strftime("%H:%M")
        except ValueError:
            pass
    return None


def falls_on(birthday, today):
    """Whether a stored birthday is to be wished on this day. Someone born on
    29 February is wished on the 28th in years without one."""
    if not well_formed(birthday):
        return False
    parts = birthday.split("-")
    month, day = int(parts[-2]), int(parts[-1])
    if (month, day) == (2, 29) and (today.month, today.day) == (2, 28):
        try:
            datetime.date(today.year, 2, 29)
        except ValueError:
            return True
    return (month, day) == (today.month, today.day)


def well_formed(birthday):
    """Whether this is "MM-DD" or "YYYY-MM-DD" and names a day that exists."""
    if not re.fullmatch(r"(\d{4}-)?\d{2}-\d{2}", birthday or ""):
        return False
    parts = birthday.split("-")
    try:
        datetime.date(2004, int(parts[-2]), int(parts[-1]))   # 2004 had a 29 February
    except ValueError:
        return False
    return True


def age_on(birthday, today):
    parts = birthday.split("-")
    age = today.year - int(parts[0]) if len(parts) == 3 else None
    return age if age and 0 < age < 130 else None


def in_words(birthday):
    parts = birthday.split("-")
    return f"{int(parts[-1])} {MONTHS[int(parts[-2]) - 1].capitalize()}" + (f" {parts[0]}" if len(parts) == 3 else "")


def days_until(birthday, today):
    parts = birthday.split("-")
    for year in (today.year, today.year + 1):
        try:
            day = datetime.date(year, int(parts[-2]), int(parts[-1]))
        except ValueError:      # 29 February in a year without one
            day = datetime.date(year, 2, 28)
        if day >= today:
            return (day - today).days
    return 366


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("address")
    parser.add_argument("old_channel", nargs="?", help=argparse.SUPPRESS)   # the channel used to be given here
    parser.add_argument("--channel", default=None,
                        help="where birthdays are announced: #general if never chosen; kept in the bot's files, "
                             "and changed later with `channel #name` by those who manage bots")
    parser.add_argument("--at", default=None,
                        help="when in the day, by this machine's clock: 09:00 if never chosen; kept in the bot's "
                             "files, and changed later with `time 08:30` by those who manage bots")
    parser.add_argument("--greet-existing", action="store_true",
                        help="also ask the people who were here before the bot")
    parser.add_argument("--username", default="birthdays")
    parser.add_argument("--display-name", default="Birthday Bot", help="the name people see")
    parser.add_argument("--picture", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                          "birthday_bot.png"),
                        help="its profile picture, a small square image ('' for none)")
    parser.add_argument("--vault", default="./birthdays-vault")
    # For trying it out: pretend it is this day (and past --at), look more often, greet sooner.
    parser.add_argument("--today", default=None, help=argparse.SUPPRESS)
    parser.add_argument("--tick", type=float, default=30, help=argparse.SUPPRESS)
    parser.add_argument("--join-delay", type=float, default=None, help=argparse.SUPPRESS)
    args = parser.parse_args()
    given = args.channel or args.old_channel
    if args.at and not parse_time(args.at):
        parser.error(f"--at {args.at}: not a time of day; write it like 09:00")

    bot = Bot(vault=args.vault, username=args.username, display_name=args.display_name, picture=args.picture,
              about="I remember birthdays. Tell me yours in a direct chat (like 05-17), or put it in your "
                    "profile, and I'll wish you a happy one on the day.")
    if args.join_delay is not None:
        bot.join_delay = args.join_delay
    # What it keeps about each person, by user id: "birthday" (the one they
    # told it), "asked", "seen" (the profile birthday it has spoken of), "no",
    # "private", "wished" (the year), "lost" (unreadable messages in a row).
    people = bot.store.setdefault("people", {})

    # Where birthdays are announced is kept in the bot's files. --channel
    # counts when it is new or has changed, so that what a manager chose
    # since is not undone by starting the bot again with the same command.
    if given and given != bot.store.get("channel_given"):
        bot.store.update(channel="#" + given.lstrip("#"), channel_given=given)
        bot.store.pop("channel_id", None)
        bot.store.save()
    channel = bot.store.get("channel") or "#general"
    # The time of day, in the same way.
    if args.at and args.at != bot.store.get("at_given"):
        bot.store.update(at=parse_time(args.at), at_given=args.at)
        bot.store.save()
    at = bot.store.get("at") or "09:00"

    def follow():
        """Keeps to the channel when it is given another name."""
        nonlocal channel
        room = bot.rooms.get(bot.store.get("channel_id", ""))
        if room and room.get("title") != channel:
            channel = bot.store["channel"] = room["title"]
            bot.store.save()
        elif not room:
            try:
                bot.store["channel_id"] = bot.room_id(channel)
                bot.store.save()
            except KeyError:
                pass   # not a channel the bot can see; saying so is left to the wish that fails

    wishing = threading.Lock()   # the clock and an answer in a chat may both find a birthday

    def today():
        return datetime.date.fromisoformat(args.today) if args.today else datetime.date.today()

    def birthday_of(user_id):
        """What they told the bot, or else what their profile says."""
        told = people.get(user_id, {}).get("birthday")
        if told:
            return told
        written = bot.profile_of(user_id).get("birthday", "")
        return written if well_formed(written) else ""

    def in_profile(user_id):
        """The birthday in their profile, if the bot goes by it: they have told it none."""
        return "" if people.get(user_id, {}).get("birthday") else birthday_of(user_id)

    def bold(birthday):
        return f"**{in_words(birthday)}**"

    def source_of(user_id):
        return "from your profile" if in_profile(user_id) else "as you told me"

    def help_for(user_id):
        """Everything the bot does, for one person: what it has for them
        comes first, and it does not ask for a birthday it already has."""
        birthday = birthday_of(user_id)
        person = people.get(user_id, {})
        lines = [
            "## 🎂 Birthday Bot",
            f"I wish people a happy birthday on the day, in a direct chat and in {channel}.",
            "",
            f"**Your birthday:** {in_words(birthday)}, {source_of(user_id)}" if birthday
            else "**Your birthday:** not set yet",
            f"**Wished in {channel}:** " + ("no, here only" if person.get("private") else "yes"),
            "",
            "**To change it, send me a date**" if birthday else "**To set it, send me a date**",
            DATES,
            "",
            "**Other things you can send me**",
            "- `when`: what I have for you",
            "- `forget`: drop the date you told me",
            f"- `private`: wish me here only, not in {channel}",
            f"- `public`: wish me in {channel} too",
            "- `no`: don't ask me again",
            "- `help`: this message",
            "",
            f"In a channel, `{bot.prefix}birthdays` lists the next few.",
        ]
        if bot.may(user_id):
            lines += [
                "",
                "**Because you manage bots here**",
                "- `list`: every birthday I know, and who I have asked",
                f"- `channel #name`: announce birthdays somewhere else (now {channel})",
                f"- `time 08:30`: wish people at another time of day (now {at})",
                "- `reset wishes`: let this year's wishes go out again (`reset wishes @name` for one person)",
                "- `reset asked`: let me ask people again (`reset asked @name` for one person)",
                "- `reset @name`: forget everything about one person",
                "- `set @name 05-17`: set someone's birthday",
                "- `ask @name` or `ask everyone`: send my question now",
            ]
        return "\n".join(lines)

    def hint_for(user_id):
        """The short version, after something it could not read."""
        birthday = birthday_of(user_id)
        if birthday:
            return (f"I have your birthday as {bold(birthday)}, {source_of(user_id)}. Send another date to change "
                    "it, or `help` to see everything I know.")
        return "Send me your birthday like `05-17` or `17 May`, or `help` to see everything I know."

    def name_of(member):
        return member.get("display_name") or member.get("username", "")

    def find(name):
        """A member by @username, or by the name people see."""
        name = name.lstrip("@").lower()
        members = [m for m in bot.members().values() if not m.get("bot")]
        return next((m for m in members if m.get("username", "").lower() == name),
                    next((m for m in members if name_of(m).lower() == name), None))

    def listing():
        known, without = [], []
        for user_id, member in sorted(bot.members().items(), key=lambda item: name_of(item[1]).lower()):
            if member.get("bot"):
                continue
            person = people.get(user_id, {})
            birthday = birthday_of(user_id)
            notes = [note for note, on in (("from the profile", birthday and not person.get("birthday")),
                                           ("told me", person.get("birthday")),
                                           (f"wished in {person.get('wished')}", person.get("wished")),
                                           ("private", person.get("private")),
                                           ("said no", person.get("no")),
                                           ("asked", person.get("asked") and not birthday)) if on]
            # The day and the month only: the year is theirs to give out.
            if birthday:
                known.append(f"- **{name_of(member)}** (`{member['username']}`): "
                             f"{' '.join(in_words(birthday).split()[:2])}; {', '.join(notes)}")
            else:
                without.append(name_of(member) + (f" ({', '.join(notes)})" if notes else ""))
        return "\n".join([f"**Birthdays I know ({len(known)})**"] + (known or ["- none yet"]) +
                         (["", "**No birthday yet:** " + ", ".join(without)] if without else []))

    def manage(message):
        """What the owner and those with the Manage bots permission may tell it."""
        nonlocal channel, at
        if not bot.may(message.sender_id):
            return "Only the server's owner and people with the **Manage bots** permission can do that."
        words = message.body.split()
        verb, rest = words[0].lower(), words[1:]
        unknown = "I don't know anyone called **{}** here."
        if verb == "list":
            return listing()
        if verb == "time":
            clock = datetime.datetime.now().strftime("%H:%M")
            if not rest:
                return (f"I wish people at **{at}**, by the clock of the machine I run on (it is {clock} there "
                        "now). To change that: `time 08:30`.")
            chosen = parse_time(" ".join(rest))
            if not chosen:
                return "I can't read that as a time of day. Write it like `time 08:30` or `time 9am`."
            at = chosen
            bot.store["at"] = chosen
            bot.store.save()
            return (f"Done: I wish people at **{chosen}** from now on, by the clock of the machine I run on (it is "
                    f"{clock} there now). I'll remember that when I'm restarted." +
                    (" That time has passed today, so today's birthdays are wished now." if chosen <= clock else ""))
        if verb == "channel":
            follow()
            if not rest:
                return f"Birthdays are announced in **{channel}**. To change that: `channel #name`."
            name = "#" + rest[0].lstrip("#")
            try:
                room = bot.rooms[bot.room_id(name)]
            except KeyError:
                seen = sorted(r.get("title", "") for r in bot.rooms.values() if r.get("kind") == "channel")
                return (f"I can't see a channel called **{name}**. The ones I can see: " +
                        ", ".join(f"`{title}`" for title in seen) + ".")
            if room.get("can_send") is False:
                return f"I'm not allowed to write in **{name}**. Let me write there, or choose another."
            channel = name
            bot.store.update(channel=name, channel_id=room["room_id"])
            bot.store.save()
            return f"Done: birthdays are announced in **{name}** from now on. I'll remember that when I'm restarted."
        if verb == "reset" and rest and rest[0].lower() in ("wishes", "asked"):
            what = rest[0].lower()
            keys = ("wished",) if what == "wishes" else ("asked", "seen")
            after = (" Anyone whose birthday is today is wished again now." if what == "wishes"
                     else " I ask each of them when they next join, or now with `ask @name` or `ask everyone`.")
            if len(rest) > 1:
                member = find(rest[1])
                if not member:
                    return unknown.format(rest[1].lstrip("@"))
                for key in keys:
                    people.get(member["user_id"], {}).pop(key, None)
                return (f"Done: **{name_of(member)}** can be wished again this year." if what == "wishes"
                        else f"Done: I no longer count **{name_of(member)}** as asked.")
            changed = sum(1 for person in people.values() if any(key in person for key in keys))
            for person in people.values():
                for key in keys:
                    person.pop(key, None)
            return f"Done: {changed} {'person' if changed == 1 else 'people'} reset." + after
        if verb == "reset" and rest:
            member = find(rest[0])
            if not member:
                return unknown.format(rest[0].lstrip("@"))
            people.pop(member["user_id"], None)
            return (f"Done: I have forgotten everything about **{name_of(member)}**: the date they told me, "
                    "that I asked, this year's wish and their choices. A birthday in their profile still counts.")
        if verb == "set" and len(rest) >= 2:
            member = find(rest[0])
            if not member:
                return unknown.format(rest[0].lstrip("@"))
            birthday = parse_birthday(" ".join(rest[1:]), today().year)
            if not birthday:
                return "I can't read that date. Write it like `set @name 05-17` or `set @name 17 May`."
            people.setdefault(member["user_id"], {})["birthday"] = birthday
            return f"Done: **{name_of(member)}**'s birthday is {bold(birthday)}."
        if verb == "ask" and rest:
            if rest[0].lower() in ("everyone", "all"):
                waiting = [m for m in bot.members().values() if not m.get("bot")
                           and not people.get(m["user_id"], {}).get("asked")
                           and not people.get(m["user_id"], {}).get("no")
                           and not people.get(m["user_id"], {}).get("birthday")]
                for member in waiting:
                    bot.attempt(ask, member)
                    time.sleep(1)
                return (f"Done: I wrote to {len(waiting)} {'person' if len(waiting) == 1 else 'people'}. "
                        "I leave out those I have asked before, those who said no, and those who told me a date.")
            member = find(rest[0])
            if not member:
                return unknown.format(rest[0].lstrip("@"))
            person = people.setdefault(member["user_id"], {})
            if person.get("no"):
                return f"**{name_of(member)}** told me not to ask, so I won't."
            if person.get("birthday"):
                return f"**{name_of(member)}** has told me a birthday already."
            person.pop("asked", None)
            ask(member)
            return f"Done: I wrote to **{name_of(member)}**."
        return ("I didn't follow that. What I take from those who manage bots: `list`, `channel #name`, "
                "`time 08:30`, `reset wishes`, `reset asked`, `reset @name`, `set @name 05-17`, `ask @name`, "
                "`ask everyone`.")

    def ask(member):
        person = people.setdefault(member["user_id"], {})
        if person.get("asked") or person.get("no") or person.get("birthday"):
            return
        seen = in_profile(member["user_id"])
        if seen:
            # Nothing to ask: say what it found and what it will do with it.
            words = "\n".join([
                f"Hi **{name_of(member)}**, I'm the birthday bot here. 🎂",
                "",
                f"I see from your profile that your birthday is {bold(seen)}, so I'll wish you a happy one then, "
                f"here and in {channel}.",
                "",
                "**If you'd like something else, reply with**",
                f"- `private`: wish me here only, not in {channel}",
                "- another date, if that one is wrong",
                "- `help`: everything I know",
            ])
        else:
            words = "\n".join([
                f"Hi **{name_of(member)}**, I'm the birthday bot here. 🎂",
                "",
                "Tell me your birthday and I'll wish you a happy one on the day.",
                "",
                "**Reply with**",
                DATES,
                "- `no`: I won't ask again",
                "- `help`: everything I know",
                "",
                "Or put it in your profile, and I'll find it there.",
            ])
        # Marked first, as with a wish: a greeting that fails half way is not repeated.
        person["asked"] = True
        person["seen"] = seen
        bot.store.save()
        bot.dm(member["username"], words)

    bot.on_join(ask)

    @bot.on_profile
    def noticed(user_id, profile):
        """Someone it has asked puts a birthday in their profile, or changes
        it: the bot says that it has seen it, once for each date."""
        person = people.get(user_id, {})
        member = bot.members().get(user_id)
        seen = in_profile(user_id)
        if not member or not person.get("asked") or person.get("no") or not seen or seen == person.get("seen"):
            return
        person["seen"] = seen
        bot.store.save()
        if falls_on(seen, today()):
            wish()   # the wish says it all
            return
        bot.dm(member["username"], f"I see your birthday in your profile now: {bold(seen)}. "
                                   "I'll wish you a happy one then.")

    def wish_one(member, birthday, day):
        person = people.setdefault(member["user_id"], {})
        # Marked first: a wish that fails half way is not said twice.
        person["wished"] = day.year
        bot.store.save()
        age = age_on(birthday, day)
        bot.dm(member["username"], f"🎂 **Happy birthday, {name_of(member)}!**" + (f" {age} today." if age else ""))
        if not person.get("private"):
            follow()
            # By username with an @, so they are called to it as by any mention.
            bot.say(channel, f"🎂 **Happy birthday**, @{member['username']}!" + (f" ({age} today.)" if age else ""))

    def wish():
        now = datetime.datetime.now()
        if not args.today and now.strftime("%H:%M") < at:
            return
        day = today()
        with wishing:
            for user_id, member in bot.members().items():
                birthday = birthday_of(user_id)
                if (not member.get("bot") and people.get(user_id, {}).get("wished") != day.year
                        and falls_on(birthday, day)):
                    bot.attempt(wish_one, member, birthday, day)

    bot.every(args.tick)(wish)

    @bot.on_message
    def told(message):
        if not message.direct or message.body.strip().startswith(bot.prefix):
            return   # elsewhere, or a command, which is answered below
        person = people.setdefault(message.sender_id, {})
        word = message.body.strip().lower().strip(".!")
        lost = person.pop("lost", 0)
        if word.split()[:1] and word.split()[0] in ("list", "channel", "time", "reset", "set", "ask"):
            answer = manage(message)
            person = people.setdefault(message.sender_id, {})   # they may have reset themselves
        elif word in ("no", "stop", "no thanks"):
            person["no"] = True
            answer = "All right, I won't ask. If you change your mind, tell me a date here any time."
        elif word == "forget":
            told_before = person.pop("birthday", None)
            written = in_profile(message.sender_id)
            if written:
                answer = (("Forgotten what you told me. " if told_before else "") +
                          f"Your profile says {bold(written)}, and I go by that; clear it there if you "
                          "want no wish.")
            else:
                answer = "Forgotten. I have no birthday for you now." if told_before else "I had none to forget."
        elif word == "when":
            birthday = birthday_of(message.sender_id)
            answer = (f"I have {bold(birthday)}, {source_of(message.sender_id)}." if birthday
                      else "I don't have a birthday for you yet. " + hint_for(message.sender_id))
        elif word in ("private", "public"):
            person["private"] = word == "private"
            answer = ("I'll wish you here only, not in the channel." if word == "private"
                      else f"I'll wish you in {channel} too.")
        elif word in ("help", "?", "commands", "hi", "hello", "hey"):
            answer = help_for(message.sender_id)
        else:
            birthday = parse_birthday(message.body, today().year)
            if not birthday:
                slashed = re.fullmatch(r"\d{1,4}[/.]\d{1,2}([/.]\d{1,4})?", word)
                answer = ("I can't tell the day from the month there. Write it like `05-17` (month-day) or `17 May`."
                          if slashed else "I didn't understand that. " + hint_for(message.sender_id))
                # Whatever keeps writing things it cannot read (another program,
                # perhaps) gets three answers and then silence.
                person["lost"] = lost + 1
                if lost >= 3:
                    bot.store.save()
                    return
            else:
                written = in_profile(message.sender_id)
                person["birthday"] = birthday
                person.pop("no", None)
                answer = f"Got it: {bold(birthday)}. " + (
                    "That's today!" if falls_on(birthday, today()) and person.get("wished") != today().year
                    else "I'll wish you a happy birthday then.")
                if written and written[-5:] != birthday[-5:]:
                    answer += f" Your profile says {bold(written)}; I'll go by what you told me here."
        bot.store.save()
        bot.reply(message, answer)
        wish()   # it may be today

    @bot.command("birthdays", help="the next few birthdays")
    def coming(message, words):
        day = today()
        known = []
        for user_id, member in bot.members().items():
            birthday = birthday_of(user_id)
            if birthday and not member.get("bot") and not people.get(user_id, {}).get("private"):
                known.append((days_until(birthday, day), name_of(member), birthday))
        if not known:
            return "I don't know any birthdays yet. Tell me yours in a direct chat: send me `help` there."
        lines = ["**Next birthdays**"]
        for days, name, birthday in sorted(known)[:5]:
            when = "today 🎂" if days == 0 else "tomorrow" if days == 1 else f"in {days} days"
            lines.append(f"- **{name}**: {' '.join(in_words(birthday).split()[:2])} ({when})")
        return "\n".join(lines)

    @bot.command("help", help="what I do")
    def help_(message, words):
        if message.direct:
            return help_for(message.sender_id)
        return "\n".join([
            "## 🎂 Birthday Bot",
            f"I wish people a happy birthday on the day, in a direct chat and in {channel}.",
            "",
            f"- **Tell me yours:** open a direct chat with me (`@{bot.username}`) and send a date like `05-17`, "
            "or put it in your profile.",
            "- **More:** send me `help` in that chat: changing the date, being wished privately, and the rest.",
            f"- `{bot.prefix}birthdays`: the next few birthdays",
        ])

    bot.connect(args.address)
    follow()
    print(f"{args.username} is connected", flush=True)
    if args.greet_existing:
        time.sleep(bot.join_delay)   # the bots already here say that they are bots when someone new arrives
        for member in bot.members().values():
            if not member.get("bot"):
                bot.attempt(ask, member)
                time.sleep(1)
    bot.run()


if __name__ == "__main__":
    main()
