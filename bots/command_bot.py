#!/usr/bin/env python3
"""A bot that answers commands: !ping, !roll 2d6, !choose a | b | c, !help.

    python3 command_bot.py SERVER:PORT [USERNAME] [VAULT_FOLDER]
"""
import random
import sys

from corded_bot import Bot

address = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:7443"
username = sys.argv[2] if len(sys.argv) > 2 else "helper"
bot = Bot(vault=sys.argv[3] if len(sys.argv) > 3 else f"./{username}-vault", username=username,
          about="I answer commands. Try !help.")


@bot.command("ping", help="check that I am awake")
def ping(message, words):
    return "pong"


@bot.command("roll", help="roll dice: !roll 2d6")
def roll(message, words):
    count, _, sides = (words[0] if words else "1d6").lower().partition("d")
    try:
        count, sides = int(count or 1), int(sides or 6)
    except ValueError:
        return "Say it like 2d6: two dice with six sides."
    if not (1 <= count <= 20 and 2 <= sides <= 1000):
        return "Between 1 and 20 dice, with 2 to 1000 sides."
    rolls = [random.randint(1, sides) for _ in range(count)]
    return f"{' + '.join(map(str, rolls))} = {sum(rolls)}" if count > 1 else str(rolls[0])


@bot.command("choose", help="pick one: !choose pizza | tacos | soup")
def choose(message, words):
    options = [o.strip() for o in " ".join(words).split("|") if o.strip()]
    return random.choice(options) if len(options) > 1 else "Give me at least two things, with | between them."


bot.connect(address)
print(f"{username} is connected to {address}", flush=True)
bot.run()
