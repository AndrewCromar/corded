# Bots

A Corded bot is an ordinary member of a server. It has a username, its own keys
and its own encrypted vault, and it reads and writes end-to-end encrypted
messages exactly as a person's client does. The server cannot tell it from a
person; its profile says "bot", so people see a BOT tag beside its name.

Everything here is plain Python 3 with no packages to install (the video bot
also needs the programs yt-dlp and ffmpeg on the machine). The bots drive
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
| `video_bot.py` | Mention it with a link to a video and it posts the file in a thread under the link. Fetched by the machine it runs on. | By the test suite with a stand-in for the downloader, and with real links on a test server |
| `birthday_bot.py` | Asks each new member for their birthday in a direct chat, and on the day wishes them there and in a channel. | By the test suite, and tried on a real server |

```sh
python3 webhook_bot.py 127.0.0.1:7443 '#development' --secret some-long-word
curl -X POST localhost:8765/say -H 'X-Corded-Secret: some-long-word' -d '{"text":"the build passed"}'

python3 ai_bot.py 127.0.0.1:7443 --soul soul.example.txt --model llama3.2

python3 birthday_bot.py 127.0.0.1:7443

python3 video_bot.py 127.0.0.1:7443 --yt-dlp /path/to/yt-dlp
```

### The birthday bot

When someone joins, the bot asks them once, in a direct chat, for their
birthday. If their profile already has one it does not ask: it says which day
it found and that it will wish them then, and it says so too when someone it
asked fills in their profile later. They answer there (`05-17`, `17 May`, `2004-05-17` to have their age
shown) or put it in their profile; an answer in the chat wins. On the day, at
`--at` by the clock of the machine the bot runs on, it wishes them in the
direct chat and in a channel, where it mentions them by `@name` so that they
are notified.

The channel is kept in the bot's files. It is `#general` to begin with;
`--channel '#birthdays'` chooses another when the bot is started, and from
then on those who manage bots change it with `channel #name` in a direct chat
(below). Starting the bot again with the same command does not undo that.
The time of day works the same way: `09:00` to begin with, `--at` at the
start, `time 08:30` from a manager afterwards.

In the direct chat it also understands `help` (what it has for you and
everything it knows), `when`, `forget`, `no` (never ask again), `private` (no
post in the channel) and `public`. In a channel, `!birthdays` lists the next
few and `!help` says what the bot is for. Its messages use bold, lists and
headings, which the apps draw as styling.

The server's owner, and anyone with a role that has the **Manage bots**
permission, can tell it more in a direct chat. Everyone else is refused.

| | |
|---|---|
| `list` | Every birthday it knows (day and month), where each came from, and who it has asked. |
| `channel #name` | Announce birthdays in another channel. `channel` alone says which it is now. |
| `time 08:30` | Wish people at another time of day. `time` alone says when, and what the bot's clock reads. |
| `reset wishes` | Let this year's wishes go out again; anyone whose birthday is today is wished at once. `reset wishes @name` for one person. |
| `reset asked` | Let it ask people again. `reset asked @name` for one person. |
| `reset @name` | Forget everything about one person. |
| `set @name 05-17` | Set someone's birthday. |
| `ask @name`, `ask everyone` | Send its question now. It leaves out those who said no and those who told it a date. |

People who were on the server before the bot's first run are not asked, so
that starting it does not send everyone a message at once; `--greet-existing`
asks them too. Other bots are never asked. Someone born on 29 February is
wished on the 28th in other years. If the bot was off at the set time it
wishes when it is next started that day, and nobody is wished twice in a year.

The birthdays people tell it are kept in `store.json` in the bot's folder, on
the machine it runs on, and are sent nowhere.

It shows as "Birthday Bot" with a cake for a picture (`birthday_bot.png`,
drawn in `birthday_bot.svg`); `--display-name` and `--picture` change them.

### The video bot

Someone posts a link to a video. The bot does nothing until it is mentioned:

- `@clips https://…`, with the link in the same message;
- `@clips` in a reply to a message that has a link, or in the thread under it;
- `@clips audio https://…` for the sound only.

It then posts the video file in the thread under the message that holds the
link, with its title, length and size. In a direct chat with the bot a link
alone is enough. If it cannot, it says why in one line, in the same thread.

**Everything is done by the machine the bot runs on.** The fetching is done by
[yt-dlp](https://github.com/yt-dlp/yt-dlp/releases), a program that knows well
over a thousand video sites, and by ffmpeg, which joins picture and sound.
Both run on that machine, and the only other machine spoken to is the site
the video is on; no service in between is used. Say where yt-dlp is with
`--yt-dlp`, or put it beside `video_bot.py` or on `PATH`. Sites change, so
when one stops working the first thing to try is a newer yt-dlp. Some sites
want an account or forbid fetching, and then the bot says so.

Limits, to begin with: 20 minutes, 200 MB, and 720p or the nearest below. A
video too large at that quality is fetched at a lower one. The server's own
limit for one file counts too, if it is smaller. One video for each request:
no playlists and no live streams. Five requests in ten minutes for each
person. `--channels '#clips,#general'` keeps it to those channels.

The limits are kept in the bot's files. The server's owner, and anyone with a
role that has the **Manage bots** permission, change them in a direct chat:
`limits`, `limit minutes 30`, `limit size 300`, `limit quality 1080`, and
`adult` (below).

Adult sites are fetched only in a channel marked NSFW, or in a direct chat
with the bot. That covers over seventy adult sites the bot knows by name, any
site whose address says so (`.xxx`, "porn" in the name), the sites yt-dlp
marks as adult, and single videos that an ordinary site marks 18+. Those who
manage bots change it: `adult off` (nowhere), `adult anywhere`, `adult nsfw`
(as it begins), and `adult add example.com` to name another site.

It only fetches from the public web: an address that leads to the machine it
runs on, or to the network that machine is on, is refused. This is checked
before fetching; a public page that sends the downloader on to a private
address is not caught, so do not run the bot on a network with things that
trust whoever is inside it. The file is deleted from the bot's machine once it
is sent, and what is sent takes space on the server like any other file.

What people fetch is theirs to answer for, and so is whether a site allows it.

## Write your own

```python
from corded_bot import Bot

bot = Bot(vault="./my-vault", username="mybot", about="What I am for.",
          display_name="My Bot", picture="mybot.png")   # both optional; the picture about 128 pixels square

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
see), `room_id`, `event_id`, `thread` (the message whose thread it is in),
`reply_to` (the message it answers), `mentions_me` (it says `@` and the bot's
name) and `direct` (it was said in a direct chat with the bot).

| | |
|---|---|
| `bot.say(room, text, reply_to=, thread=)` | Send text. A room is its id or its title, like `"#general"`. |
| `bot.reply(message, text)` | Answer where the message was said. |
| `bot.dm(username, text)` | Open the direct chat with someone and say something there. Returns the room id. |
| `bot.react(message, emoji)` | `bot.unreact(message, what_react_returned)` takes it back. |
| `bot.send_file(room, path, caption, reply_to=, thread=)` | `thread=` puts the file in the thread under that message. |
| `bot.typing(room)` | Show "typing" for a few seconds while working. |
| `bot.recall(event_id)` | A recent message by its id: what `reply_to` and `thread` point at. |
| `bot.lookup(room_id, event_id)` | The same for a message of any age: from the bot's vault if it is older than it remembers. |
| `bot.members()`, `bot.profile_of(user_id)`, `bot.rooms` | Who and what the bot can see. |
| `bot.may(user_id)` | Whether someone may give the bot orders: the server's owner, and anyone whose roles have the Manage bots permission. `bot.may(user_id, "kick_members")` asks about another permission. |
| `bot.work(fn, *args)` | Run a job after those already waiting, one at a time (one model, one graphics card). Returns how many are ahead. |
| `bot.store` | A dictionary kept between runs, in `store.json` in the bot's folder. Call `bot.store.save()` after changing it. |
| `bot.attempt(fn, *args)` | Call something that may fail without stopping the rest; the failure is printed. |
| `bot.request({...})` | Any command the core understands; returns its answer. |

More things to hang a function on:

```python
@bot.on_join
def welcome(member):                 # someone joined the server
    bot.dm(member["username"], f"Welcome, {member['display_name']}.")

@bot.on_file
def got(message):                    # someone sent a file
    print(message.file["name"], message.file["size"], message.body)

@bot.on_profile
def changed(user_id, profile):       # someone's profile reached the bot, new or changed
    print(profile.get("birthday"))
```

`on_join` is called once for each person who joins after the bot's first run,
also for those who joined while it was off. The people already there on the
first run are not counted as joining, and other bots never are.

A bot does not hear other bots (two answering each other would never stop);
`Bot(..., hear_bots=True)` changes that.

## Leave one running

`birthday-bot.service.example` is a unit for `systemd --user`, which starts
the bot when the machine does and starts it again if it stops. The steps are
at the top of that file; it serves for any of the bots with the command
changed.

## What a bot can read

Whatever a member in its place could: every message in the channels and chats
it is in, from the moment it joined. Give a bot a role that sees only the
channels it needs. The AI bot sends the recent messages of a chat to the model
you point it at; with the default address that is a program on your own
machine.
