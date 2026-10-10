#!/usr/bin/env python3
"""A chat member run by a language model on your own machine.

    python3 ai_bot.py SERVER:PORT --soul soul.txt [--model llama3.2]
                      [--api http://127.0.0.1:11434/v1] [--username sage]

The "soul" is a text file that says who the bot is: its name, how it talks,
what it cares about, what it will not do. It is given to the model as its
standing instructions before every answer; edit the file and the next answer
follows the new text.

The model is reached over the chat API that Ollama, llama.cpp's server, LM
Studio and others all offer (the "OpenAI-compatible" one). Nothing leaves
your machine unless --api points somewhere else. The bot reads every message
in the chats it is in, as any member does; it answers when it is mentioned
(@name), when a message replies in a thread it is part of, or in a direct chat.
"""
import argparse
import json
import urllib.request

from corded_bot import Bot

parser = argparse.ArgumentParser()
parser.add_argument("address")
parser.add_argument("--soul", required=True)
parser.add_argument("--model", default="llama3.2")
parser.add_argument("--api", default="http://127.0.0.1:11434/v1")
parser.add_argument("--username", default="sage")
parser.add_argument("--vault", default=None)
parser.add_argument("--memory", type=int, default=20, help="how many recent messages of a chat it keeps in mind")
args = parser.parse_args()

bot = Bot(vault=args.vault or f"./{args.username}-vault", username=args.username,
          about="I am a language model running on my owner's machine. Mention me to talk.")
recent = {}   # room id -> the last few (speaker, words)


def ask_model(room_id):
    soul = open(args.soul, encoding="utf-8").read().strip()
    messages = [{"role": "system", "content": soul + f"\n\nYou are in a group chat under the name {args.username}. "
                 "Answer the latest message briefly, as one chat message, in plain text."}]
    for speaker, words in recent.get(room_id, []):
        if speaker == args.username:
            messages.append({"role": "assistant", "content": words})
        else:
            messages.append({"role": "user", "content": f"{speaker}: {words}"})
    request = urllib.request.Request(
        args.api.rstrip("/") + "/chat/completions",
        data=json.dumps({"model": args.model, "messages": messages, "stream": False}).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=180) as answer:
        return json.load(answer)["choices"][0]["message"]["content"].strip()


@bot.on_message
def hear(message):
    history = recent.setdefault(message.room_id, [])
    history.append((message.sender_name, message.body))
    del history[:-args.memory]
    room = bot.rooms.get(message.room_id, {})
    spoken_to = f"@{args.username}".lower() in message.body.lower() or room.get("kind") == "direct"
    if not spoken_to:
        return
    try:
        words = ask_model(message.room_id)
    except Exception as error:  # noqa: BLE001
        bot.reply(message, f"I could not think just now ({error}).")
        return
    if words:
        history.append((args.username, words))
        bot.reply(message, words[:4000])


if __name__ == "__main__":
    bot.connect(args.address)
    print(f"{args.username} is connected, thinking with {args.model} at {args.api}", flush=True)
    bot.run()
