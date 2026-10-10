#!/usr/bin/env python3
"""Asks new members for their birthday and wishes them a happy one.

    python3 birthday_bot.py SERVER:PORT [--channel '#general'] [--at 09:00]
                            [--greet-existing]

When someone joins the server the bot opens a direct chat and asks, once, for
their birthday. They answer there (05-17, 17 May, 2004-05-17) or put it in
their profile; an answer in the chat wins. On the day, at --at by this
machine's clock, the bot wishes them in the direct chat and in the channel.

In the direct chat the bot also understands: when, forget, no (never ask
again), private (no post in the channel), public. In a channel, !birthdays
lists the next few.

Birthdays told to the bot are kept in its folder on this machine (store.json
beside its vault) and are sent nowhere.
"""
import argparse
import datetime
import re
import threading
import time

from corded_bot import Bot

MONTHS = ["january", "february", "march", "april", "may", "june", "july", "august", "september", "october",
          "november", "december"]
HELP = ("Tell me your birthday like `05-17` (month-day), `17 May`, or `2004-05-17` if you want your age shown. "
        "Other words I know: `when`, `forget`, `no` (I won't ask again), `private` (I wish you here only, "
        "not in the channel), `public`.")


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


def falls_on(birthday, today):
    """Whether a stored birthday is to be wished on this day. Someone born on
    29 February is wished on the 28th in years without one."""
    parts = birthday.split("-")
    if len(parts) not in (2, 3) or not all(p.isdigit() for p in parts):
        return False
    month, day = int(parts[-2]), int(parts[-1])
    if (month, day) == (2, 29) and (today.month, today.day) == (2, 28):
        try:
            datetime.date(today.year, 2, 29)
        except ValueError:
            return True
    return (month, day) == (today.month, today.day)


def in_range(birthday):
    """Whether "MM-DD" or "YYYY-MM-DD" names a day that exists."""
    parts = birthday.split("-")
    try:
        datetime.date(2004, int(parts[-2]), int(parts[-1]))
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
    parser.add_argument("--channel", default=None, help="where birthdays are announced (default #general)")
    parser.add_argument("--at", default="09:00", help="when in the day, by this machine's clock")
    parser.add_argument("--greet-existing", action="store_true",
                        help="also ask the people who were here before the bot")
    parser.add_argument("--username", default="birthdays")
    parser.add_argument("--vault", default="./birthdays-vault")
    # For trying it out: pretend it is this day (and past --at), look more often, greet sooner.
    parser.add_argument("--today", default=None, help=argparse.SUPPRESS)
    parser.add_argument("--tick", type=float, default=30, help=argparse.SUPPRESS)
    parser.add_argument("--join-delay", type=float, default=None, help=argparse.SUPPRESS)
    args = parser.parse_args()
    channel = args.channel or args.old_channel or "#general"
    args.at = datetime.datetime.strptime(args.at, "%H:%M").strftime("%H:%M")   # 9:00 is 09:00

    bot = Bot(vault=args.vault, username=args.username,
              about="I remember birthdays. Tell me yours in a direct chat, or put it in your profile.")
    if args.join_delay is not None:
        bot.join_delay = args.join_delay
    people = bot.store.setdefault("people", {})   # user id -> {"birthday", "asked", "no", "private", "wished"}

    wishing = threading.Lock()   # the clock and an answer in a chat may both find a birthday

    def today():
        return datetime.date.fromisoformat(args.today) if args.today else datetime.date.today()

    def birthday_of(user_id):
        """What they told the bot, or else what their profile says."""
        told = people.get(user_id, {}).get("birthday")
        if told:
            return told
        written = bot.profile_of(user_id).get("birthday", "")
        return written if re.fullmatch(r"(\d{4}-)?\d{2}-\d{2}", written) and in_range(written) else ""

    def name_of(member):
        return member.get("display_name") or member.get("username", "")

    def ask(member):
        person = people.setdefault(member["user_id"], {})
        if person.get("asked") or person.get("no") or birthday_of(member["user_id"]):
            return
        bot.dm(member["username"],
               f"Hi {name_of(member)}, I'm the birthday bot here. Tell me your birthday and I'll wish you a happy "
               "one: reply `05-17` (month-day), or `2004-05-17` if you want your age shown. Or put it in your "
               "profile. Reply `no` and I won't ask again.")
        person["asked"] = True
        bot.store.save()

    bot.on_join(ask)

    def wish_one(member, birthday, day):
        person = people.setdefault(member["user_id"], {})
        # Marked first: a wish that fails half way is not said twice.
        person["wished"] = day.year
        bot.store.save()
        age = age_on(birthday, day)
        bot.dm(member["username"], f"🎂 Happy birthday, {name_of(member)}!" + (f" {age} today." if age else ""))
        if not person.get("private"):
            bot.say(channel, f"🎂 Happy birthday, **{name_of(member)}**!" + (f" ({age} today.)" if age else ""))

    def wish():
        now = datetime.datetime.now()
        if not args.today and now.strftime("%H:%M") < args.at:
            return
        day = today()
        with wishing:
            for user_id, member in bot.members().items():
                birthday = birthday_of(user_id)
                if (not member.get("bot") and people.get(user_id, {}).get("wished") != day.year
                        and falls_on(birthday, day)):
                    bot._guard(wish_one, member, birthday, day)

    bot.every(args.tick)(wish)

    @bot.on_message
    def told(message):
        if not message.direct:
            return
        person = people.setdefault(message.sender_id, {})
        word = message.body.strip().lower().strip(".!")
        lost = person.pop("lost", 0)
        if word in ("no", "stop", "no thanks"):
            person["no"] = True
            answer = "All right, I won't ask. If you change your mind, tell me a date here any time."
        elif word == "forget":
            person.pop("birthday", None)
            answer = "Forgotten. A birthday in your profile still counts; clear it there too if you want none."
        elif word == "when":
            birthday = birthday_of(message.sender_id)
            answer = f"I have {in_words(birthday)}." if birthday else "I don't have a birthday for you. " + HELP
        elif word in ("private", "public"):
            person["private"] = word == "private"
            answer = ("I'll wish you here only, not in the channel." if word == "private"
                      else f"I'll wish you in {channel} too.")
        elif word in ("help", "?"):
            answer = HELP
        else:
            birthday = parse_birthday(message.body, today().year)
            if not birthday:
                slashed = re.fullmatch(r"\d{1,4}[/.]\d{1,2}([/.]\d{1,4})?", word)
                answer = ("I can't tell the day from the month there. Write it like `05-17` (month-day) or `17 May`."
                          if slashed else "I didn't understand that. " + HELP)
                # Whatever keeps writing things it cannot read (another program,
                # perhaps) gets three answers and then silence.
                person["lost"] = lost + 1
                if lost >= 3:
                    bot.store.save()
                    return
            else:
                person["birthday"] = birthday
                person.pop("no", None)
                answer = f"Got it: {in_words(birthday)}. I'll wish you a happy birthday then."
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
            return "I don't know any birthdays yet. Tell me yours in a direct chat."
        return "\n".join(f"{name}: {' '.join(in_words(b).split()[:2])}" + (" (today)" if days == 0 else "")
                         for days, name, b in sorted(known)[:5])

    bot.connect(args.address)
    print(f"{args.username} is connected", flush=True)
    if args.greet_existing:
        time.sleep(bot.join_delay)   # the bots already here say that they are bots when someone new arrives
        for member in bot.members().values():
            if not member.get("bot"):
                bot._guard(ask, member)
                time.sleep(1)
    bot.run()


if __name__ == "__main__":
    main()
