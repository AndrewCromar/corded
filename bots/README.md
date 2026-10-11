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
| `ai_bot.py` | A member run by a language model on your own machine (Ollama, llama.cpp, LM Studio). Answers direct chats, mentions and the channels chosen for it; picks between a message, a reply, a thread and a reaction. | By its own test with a stand-in model, with three real models on a test server, and tried on a real server |
| `image_bot.py` | Makes a picture from a description with Stable Diffusion on your own machine (`image_server.py` beside it draws). | By its own test with a stand-in picture program, with real pictures on one machine, and running on a real server |
| `video_bot.py` | Mention it with a link to a video and it posts the file in a thread under the link. Fetched by the machine it runs on. | By the test suite with a stand-in for the downloader, with real links on a test server, and tried on a real server |
| `birthday_bot.py` | Asks each new member for their birthday in a direct chat, and on the day wishes them there and in a channel. | By the test suite, and tried on a real server |

```sh
python3 webhook_bot.py 127.0.0.1:7443 '#development' --secret some-long-word
curl -X POST localhost:8765/say -H 'X-Corded-Secret: some-long-word' -d '{"text":"the build passed"}'

python3 ai_bot.py 127.0.0.1:7443 --soul soul.example.txt --model qwen2.5:7b

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

### The AI bot

A member that a language model speaks through. The model runs on the same
machine as the bot, behind the chat API that Ollama, llama.cpp's server and LM
Studio all offer; nothing is sent anywhere else unless `--api` says so, and
the bot prints a warning at start if it does.

**Setting it up.** The model program is not part of this repository. With
Ollama (https://ollama.com), on the machine the bot will run on:

```sh
# 1. Ollama itself. Either the system install from their site, or, with no
#    install at all, their Linux archive unpacked into a folder of your own:
mkdir -p ~/ollama && cd ~/ollama
curl -fL https://ollama.com/download/ollama-linux-amd64.tar.zst | tar --zstd -x
#    (older releases are .tgz: curl -fL …/ollama-linux-amd64.tgz | tar -xz)

# 2. Run it, on this machine only, and fetch a model (about 4.7 GB):
OLLAMA_HOST=127.0.0.1:11434 ~/ollama/bin/ollama serve &
~/ollama/bin/ollama pull qwen2.5:7b

# 3. The bot:
python3 ai_bot.py 127.0.0.1:7443 --soul soul.example.txt --username sage
```

`OLLAMA_MODELS=/some/folder` puts the models somewhere other than
`~/.ollama`. Any other program that offers the same chat API (llama.cpp's
server, LM Studio) works with `--api`. The host (`../host`) can start Ollama
and the bot together.

**What it will and will not say** is decided by two things: the soul file,
which is yours to write, and the model, which was trained with habits of its
own that no file fully overrides. `--model` takes any model your Ollama has;
people who want a model without those habits pick one made that way.

**When it answers.** It must answer a direct chat, a mention (`@sage`) and a
reply to one of its messages. It may answer, and decides for itself, in a
thread it has already spoken in, right after it answered someone (their next
message, within `--follow-up` seconds, 120), and in the channels chosen for
it. There the model is first asked one plain question, is this message meant
for the bot, and only a yes lets it speak: it joins in when it is asked or
talked about, and stays out when people are talking to each other. Alone with
one person in a channel, it takes everything as meant for it.

The owner, or anyone whose role has **Manage bots**, chooses a channel with
`!ai here` in it (or `!ai here #name` from anywhere); `!ai leave` undoes
that and `!ai where` lists them. One person gets at most `--per-minute`
answers a minute to mentions outside a direct chat (8 unless changed).

**How it answers.** The model decides each time, and the bot does what it
decided: a plain message, a reply attached to one message, a thread under
one, or only an emoji under it. Answers are short by instruction; a long one
goes into a thread and is cut at paragraph ends if it would not fit one
message. It shows as typing while the model works, and questions are answered
one at a time, so two people asking at once do not load the graphics card
twice.

It shows with a sprig of sage for a picture (`ai_bot.png`, drawn in
`ai_bot.svg`); `--display-name` and `--picture` change how it appears.

**Two files shape it**, both read again before every answer, so an edit shows
in the next one:

- `--soul FILE`: who it is. Its name, how it talks, what it will not do.
  `soul.example.txt` is a start. Without one it is a plain, friendly member.
- `--context FILE`: where it is and how the place works. `chat.md` beside the
  bot is used unless you give another: that this is a group chat, that
  answers are short, and when a reply, a thread or a reaction is the right
  form. Change the bot's manners there.

**What it keeps in mind.** The last `--memory` messages (30) of each chat, and
of each thread apart from its channel. Nothing extra is stored: after a
restart it reads them back from its own vault. `!forget` makes it start fresh
in the chat or thread it is said in. `!model` says which model it thinks with
and how long the last answer took.

**Which model.** Tried on one machine (RTX 4060, 8 GB) with the same scripted
chat, 2026-10-10:

| Model | Size on the card | An answer takes | How it did |
|---|---|---|---|
| `qwen2.5:7b` | about 5.4 GB | 0.3 to 1.5 s (the first after loading: 10 to 60 s) | Right answers, short, and the right form nearly every time: a reaction for thanks and good night, a reply to the older question it was asked about, a thread for the long one. The default |
| `llama3.2:3b` | about 3 GB | 0.2 to 1.4 s | Forms mostly right, but said it did not know the capital of Australia |
| `gemma3:4b` | about 4 GB | 0.3 to 1 s | Forms right (and a sleepy face for good night), but refused or fumbled easy questions |

A model this size gets facts wrong with a straight face now and then; the
context file tells it to say when it does not know, which helps and does not
cure it.

### The image bot

`!imagine a lighthouse in a storm, oil painting` and it replies with the
picture. So does a mention with a description after it, and anything written
to it in a direct chat. The caption gives the words and the seed, so the same
picture can be asked for again.

**Setting it up.** The picture program needs an NVIDIA graphics card with
8 GB or more, its driver, and a Python environment of its own; none of that
is part of this repository. With `uv` (https://docs.astral.sh/uv), which
fetches the right Python by itself:

```sh
# 1. Once: the environment and the libraries (about 6 GB).
uv venv --python 3.12 sd-env
uv pip install --python sd-env/bin/python torch --index-url https://download.pytorch.org/whl/cu124
uv pip install --python sd-env/bin/python diffusers transformers accelerate safetensors

# 2. The picture program. The first picture downloads the model (about 7 GB)
#    into ~/.cache/huggingface, or into the folder HF_HOME names.
PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True sd-env/bin/python image_server.py &

# 3. The bot. With ffmpeg installed, pictures show in the chat itself;
#    without it they arrive as files to open.
python3 image_bot.py 127.0.0.1:7443
```

Once the model is there, `HF_HUB_OFFLINE=1` in front of step 2 keeps the
program from asking the internet anything at start. `--model` takes another
SDXL model by its Hugging Face name or a folder. Nothing a picture is made
from or into is kept: the program holds it in memory only long enough to
hand it over, and the bot's one temporary file is deleted once it is sent.

- Options, at the end of a description: `--wide`, `--tall`, `--square`;
  `--seed 1234`; `--steps 30`; `--no things to keep out`.
- `!again` makes the last description of the chat once more with a new seed.
  `!queue` shows the line; `!cancel` takes your own request out of it. One
  picture is made at a time; each person gets five in ten minutes, which
  those who manage bots change with `!images limit 20` (`0` for no limit).
- `!images here` (or `!images here #name`), from the owner or anyone with
  **Manage bots**, chooses a channel; once one is chosen it works only in
  chosen channels and direct chats. `!images leave` and `!images where` go
  with it.
- Adult pictures are refused everywhere until `!images adult nsfw`, which
  allows them in channels marked NSFW only; `!images adult off` goes back.
  Nothing looks at the pictures: the refusal is a check of the words asked
  for, and with adult off a few words are added to what is kept out. The
  model itself has no filter. It can still produce what nobody asked for,
  which is one more reason to choose its channels.

**The picture program.** `image_server.py` is the one file here that needs
more than the standard library: PyTorch and `diffusers`. It listens on this
machine only and answers the request the AUTOMATIC1111 web UI made common
(`/sdapi/v1/txt2img`), so `image_bot.py --api` can as well point at Forge or
SD.Next. The default model is Stable Diffusion XL.

**One graphics card, two models.** On an 8 GB card the picture model and the
chatbot's model do not fit together. The picture program asks Ollama to set
its model down before it loads, and unloads itself 45 seconds after the last
picture (`--idle`). So the chatbot's first answer right after pictures is
slow, and the first picture after a pause takes about ten seconds longer.

Measured on an RTX 4060 with 8 GB, 2026-10-10: about 16 seconds a picture at
1024 pixels and 25 steps; 7.5 GB of the card while drawing; about 4 GB of
ordinary memory. A first way of loading the model, which kept it in ordinary
memory between pictures, ran a 16 GB machine out of memory on the second
picture; the program now keeps the large part on the graphics card only.

### Turning a bot off and on

Every bot made with the kit answers `!name off` and `!name on`, where name is
its username (`!sage off`, `!pictures on`), from the owner or anyone with
Manage bots. Off, it stays connected but shows as offline, hears nothing
except the command that wakes it, and its timers rest; the AI bot also has
its model unloaded from the graphics card. `!name status` says which it is.
The choice is kept across restarts. In your own bot, `@bot.on_power` is
called with True or False when it is switched.

### The video bot

Someone posts a link to a video. The bot does nothing until it is mentioned:

- `@clips https://…`, with the link in the same message;
- `@clips` in a reply to a message that has a link, or in the thread under it;
- `@clips audio https://…` for the sound only.

It then posts the video file in the thread under the message that holds the
link, with its title, length and size. A link that is itself in a thread gets
a thread of its own under it. In a direct chat with the bot a link
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
