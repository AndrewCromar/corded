# Bots

A Corded bot is an ordinary member of a server. It has a username, its own keys
and its own encrypted vault, and it reads and writes end-to-end encrypted
messages exactly as a person's client does. The server cannot tell it from a
person; its profile says "bot", so people see a BOT tag beside its name.

Everything here is plain Python 3 with no packages to install. The bots drive
`corded-cli`, the headless client that ships with Corded, so they contain no
encryption code of their own.

## Try one

From this folder, with a server running on this machine:

```sh
python3 command_bot.py 127.0.0.1:7443
```

It joins as `helper`. In any channel it can see, type `!ping`, `!roll 2d6`,
`!choose pizza | tacos` or `!help`.

The first run makes a folder (`helper-vault`) holding the bot's keys and a
passphrase file that only your user can read. Keep the folder: it *is* the
bot's account. On an invite-only server pass an invite link instead of the
address.

## The bots

| File | What it does | Checked |
|---|---|---|
| `command_bot.py` | Answers `!` commands. The place to start your own. | By the test suite |
| `webhook_bot.py` | Posts what other programs send it: any text to `/say`, GitHub events to `/github`. Listens on this machine only. | By the test suite |
| `ai_bot.py` | A member run by a language model on your own machine (Ollama, llama.cpp, LM Studio). Its character comes from a "soul" text file; see `soul.example.txt`. Answers when mentioned or in a direct chat. | By the test suite with a stand-in model; not yet with a real one |
| `birthday_bot.py` | Wishes people a happy birthday from the birthday in their profile. | Not yet run for real |

```sh
python3 webhook_bot.py 127.0.0.1:7443 '#development' --secret some-long-word
curl -X POST localhost:8765/say -H 'X-Corded-Secret: some-long-word' -d '{"text":"the build passed"}'

python3 ai_bot.py 127.0.0.1:7443 --soul soul.example.txt --model llama3.2

python3 birthday_bot.py 127.0.0.1:7443 '#general' --at 09:00
```

## Write your own

```python
from corded_bot import Bot

bot = Bot(vault="./my-vault", username="mybot", about="What I am for.")

@bot.command("hello", help="say hello")
def hello(message, words):          # words: what came after !hello
    return f"Hello, {message.sender_name}."     # returned text is sent as a reply

@bot.on_message
def every_message(message):          # every message from someone else
    if "thanks" in message.body.lower():
        bot.react(message, "👍")

@bot.every(3600)
def hourly():
    bot.say("#general", "Another hour gone.")

bot.connect("127.0.0.1:7443")
bot.run()
```

A message has `body`, `sender` (the username), `sender_name` (what people
see), `room_id`, `event_id` and `thread`. Besides `say`, `reply` and `react`
there are `send_file(room, path, caption)`, `profile_of(user_id)`, the `rooms`
the bot is in, and `request({...})`, which sends any command the core
understands and returns its answer.

## What a bot can read

Whatever a member in its place could: every message in the channels and chats
it is in, from the moment it joined. Give a bot a role that sees only the
channels it needs. The AI bot sends the recent messages of a chat to the model
you point it at; with the default address that is a program on your own
machine.
