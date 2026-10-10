#!/usr/bin/env python3
"""Wishes people a happy birthday, from the birthday in their profile.

    python3 birthday_bot.py SERVER:PORT '#channel' [--at 09:00]

A profile's birthday is 2004-05-17, or 05-17 without the year. Profiles are
shared, encrypted, with the people someone talks to, so the bot only knows the
birthdays of people it shares a chat with, and only once they have said
something there (or changed their profile) since the bot joined.
"""
import argparse
import datetime
import json
import os

from corded_bot import Bot

parser = argparse.ArgumentParser()
parser.add_argument("address")
parser.add_argument("channel")
parser.add_argument("--at", default="09:00")
parser.add_argument("--username", default="birthdays")
parser.add_argument("--vault", default="./birthdays-vault")
args = parser.parse_args()

bot = Bot(vault=args.vault, username=args.username, about="I remember birthdays. Put yours in your profile.")
done_file = os.path.join(args.vault, "wished.json")


def birthdays_today(today):
    """(display name, age or None) for everyone whose birthday is today."""
    found = {}
    for room in bot.rooms.values():
        for member in room.get("members", []):
            if member.get("me") or member.get("user_id") in found:
                continue
            day = bot.profile_of(member["user_id"]).get("birthday", "")
            parts = day.split("-")
            if len(parts) not in (2, 3) or parts[-2:] != [f"{today.month:02d}", f"{today.day:02d}"]:
                continue
            age = today.year - int(parts[0]) if len(parts) == 3 and parts[0].isdigit() else None
            found[member["user_id"]] = (member.get("display_name") or member.get("username"), age)
    return list(found.values())


@bot.every(60)
def check():
    now = datetime.datetime.now()
    if now.strftime("%H:%M") < args.at:
        return
    today = now.date()
    wished = json.load(open(done_file)) if os.path.exists(done_file) else {}
    if wished.get("day") == today.isoformat():
        return
    for name, age in birthdays_today(today):
        bot.say(args.channel, f"Happy birthday, **{name}**!" + (f" {age} today." if age and 0 < age < 130 else ""))
    json.dump({"day": today.isoformat()}, open(done_file, "w"))


if __name__ == "__main__":
    bot.connect(args.address)
    print(f"{args.username} is connected", flush=True)
    bot.run()
