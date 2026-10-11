#!/usr/bin/env python3
"""Runs the example bots against a throwaway server and talks to them.

    python3 bots_smoke.py <folder with cordedd and corded-cli>
"""
import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

bin_dir = os.path.abspath(sys.argv[1])
repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
bots = os.path.join(repo, "bots")
sys.path.insert(0, bots)
from corded_bot import Bot, Message, mentioned_names  # noqa: E402

os.environ["CORDED_CLI"] = os.path.join(bin_dir, "corded-cli")


# A stand-in for yt-dlp: it fetches nothing and answers by what the address says.
FAKE_YT_DLP = r'''#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
url = args[-1]
if "--dump-single-json" in args:
    if "fail" in url:
        sys.stderr.write("WARNING: something minor\nERROR: [generic] Unsupported URL: " + url + "\n")
        sys.exit(1)
    print(json.dumps({"title": "A Test Clip", "duration": 9000 if "long" in url else 75,
                      "webpage_url_domain": "videos.example", "is_live": "live" in url,
                      "age_limit": 18 if "adult" in url else 0,
                      "_type": "playlist" if "list" in url else "video"}))
    sys.exit(0)
folder = args[args.index("--paths") + 1]
audio = "--extract-audio" in args
tall = not audio and "res:720" in args[args.index("--format-sort") + 1]
path = os.path.join(folder, "A_Test_Clip [abc]." + ("mp3" if audio else "mp4"))
with open(path, "wb") as f:
    f.write(b"0" * (3000000 if "big" in url and tall else 5000))
print(path)
'''


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    tmp = tempfile.mkdtemp(prefix="corded-bots-")
    started = []
    try:
        port, hook_port, model_port = free_port(), free_port(), free_port()
        address = f"127.0.0.1:{port}"
        started.append(subprocess.Popen([os.path.join(bin_dir, "cordedd"), "--port", str(port), "--data",
                                         os.path.join(tmp, "server"), "--owner", "alice"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1)

        # A person, written with the same kit, who records what she is told.
        heard = []
        alice = Bot(vault=os.path.join(tmp, "alice"), username="alice", is_bot=False, hear_bots=True)
        alice.on_message(lambda m: heard.append((m.sender, m.body)))
        heard_where = []
        alice.on_message(lambda m: heard_where.append((m.room_id, m.body)))
        alice.connect(address)
        threading.Thread(target=alice.run, daemon=True).start()

        def wait_for(what, test, seconds=25, among=heard):
            end = time.time() + seconds
            while time.time() < end:
                for item in list(among):
                    if test(item):
                        return item
                time.sleep(0.2)
            raise AssertionError(f"did not hear {what}; heard {among}")

        def person(name):
            """Someone else, who keeps the messages and files that reach them."""
            texts, files = [], []
            someone = Bot(vault=os.path.join(tmp, name), username=name, is_bot=False, hear_bots=True)
            someone.on_message(texts.append)
            someone.on_file(files.append)
            someone.connect(address)
            threading.Thread(target=someone.run, daemon=True).start()
            return someone, texts, files

        def bot_process(*words):
            p = subprocess.Popen([sys.executable, *words], cwd=bots, stdout=subprocess.PIPE, text=True)
            started.append(p)
            line = p.stdout.readline()   # each bot prints one line when it is connected
            assert "connected" in line, line
            return p

        bot_process("command_bot.py", address, "helper", os.path.join(tmp, "helper"))
        time.sleep(2)   # the room's member list reaches alice
        alice.say("#general", "!ping")
        wait_for("pong", lambda item: item == ("helper", "pong"))
        alice.say("#general", "!roll 3d6")
        sender, body = wait_for("a roll", lambda item: item[0] == "helper" and "=" in item[1])
        assert 3 <= int(body.rsplit("=", 1)[1]) <= 18, body
        alice.say("#general", "!help")
        wait_for("the list of commands", lambda item: item[0] == "helper" and "!roll" in item[1] and "!ping" in item[1])
        print("ok  command bot: ping, dice and help")

        bot_process("webhook_bot.py", address, "#general", "--port", str(hook_port), "--secret", "s3cret",
                    "--vault", os.path.join(tmp, "hooks"))
        time.sleep(2)

        def post(path, body, headers):
            request = urllib.request.Request(f"http://127.0.0.1:{hook_port}{path}", data=json.dumps(body).encode(),
                                             headers={"Content-Type": "application/json", **headers})
            try:
                return urllib.request.urlopen(request, timeout=20).status
            except urllib.error.HTTPError as error:
                return error.code
        assert post("/say", {"text": "no secret"}, {}) == 403
        assert post("/say", {"text": "the build passed"}, {"X-Corded-Secret": "s3cret"}) == 200
        wait_for("the posted line", lambda item: item == ("webhooks", "the build passed"))
        import hashlib
        import hmac
        push = {"ref": "refs/heads/main", "repository": {"full_name": "a/b"}, "sender": {"login": "drew"},
                "commits": [{"message": "Fix the thing\n\nlong"}, {"message": "Add a test"}]}
        raw = json.dumps(push).encode()
        signed = urllib.request.Request(
            f"http://127.0.0.1:{hook_port}/github", data=raw,
            headers={"X-GitHub-Event": "push",
                     "X-Hub-Signature-256": "sha256=" + hmac.new(b"s3cret", raw, hashlib.sha256).hexdigest()})
        assert urllib.request.urlopen(signed, timeout=20).status == 200
        wait_for("the push", lambda item: item[0] == "webhooks" and "pushed 2 commits to a/b (main)" in item[1]
                 and "- Fix the thing" in item[1])
        assert post("/github", push, {"X-GitHub-Event": "push", "X-Hub-Signature-256": "sha256=00"}) == 403
        print("ok  webhook bot: a plain post, a signed GitHub push, and refusals without the secret")

        # The AI bot, with a stand-in for the model that says what it was asked.
        asked = []

        class Model(BaseHTTPRequestHandler):
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                asked.append(body)
                answer = json.dumps({"choices": [{"message": {"content": "Forty-two, as ever."}}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(answer)

            def log_message(self, *_):
                pass
        model = HTTPServer(("127.0.0.1", model_port), Model)
        threading.Thread(target=model.serve_forever, daemon=True).start()
        soul = os.path.join(tmp, "soul.txt")
        open(soul, "w").write("You are Sage. You answer briefly.")
        bot_process("ai_bot.py", address, "--soul", soul, "--api", f"http://127.0.0.1:{model_port}/v1",
                    "--username", "sage", "--vault", os.path.join(tmp, "sage"))
        time.sleep(2)
        alice.say("#general", "nobody asked the bot this")
        alice.say("#general", "@sage what is the answer?")
        wait_for("the model's words", lambda item: item == ("sage", "Forty-two, as ever."))
        assert len(asked) == 1, "it answered something it was not asked"
        sent = asked[0]["messages"]
        assert sent[0]["role"] == "system" and "You are Sage" in sent[0]["content"]
        assert sent[-1]["content"].endswith("@sage what is the answer?")
        print("ok  AI bot: answers only when mentioned, with its soul file as standing instructions")

        # The birthday bot. It is told that today is 17 May 2026.
        def birthdays(*more):
            return bot_process("birthday_bot.py", address, "--today", "2026-05-17", "--tick", "2",
                               "--join-delay", "2", "--vault", os.path.join(tmp, "birthdays"), *more)
        running = birthdays()
        carol, carol_heard, carol_files = person("carol")
        asked = wait_for("to be asked for a birthday", lambda m: m.sender == "birthdays" and "birthday bot" in m.body,
                         among=carol_heard)
        assert asked.direct, "the question was not in a direct chat"
        # Carol's first words in a new chat make her client send her profile
        # there by itself; the answers to what she asks next must still be hers.
        carol.say(asked.room_id, "17th of May 2004")
        assert "members" in carol.request({"cmd": "member_list"})
        assert isinstance(carol.profile_of(alice.me), dict)
        assert carol.dm("birthdays") == asked.room_id
        wait_for("the date read back", lambda m: m.sender == "birthdays" and "Got it: **17 May 2004**" in m.body,
                 among=carol_heard)
        assert any("That's today!" in m.body for m in carol_heard)
        wait_for("a wish in the direct chat", lambda m: m.direct and m.body == "🎂 **Happy birthday, carol!** 22 today.",
                 among=carol_heard)
        wait_for("a wish in the channel",
                 lambda item: item == ("birthdays", "🎂 **Happy birthday**, @carol! (22 today.)"))
        its = next(m for m in carol.members().values() if m["username"] == "birthdays")
        profile = carol.profile_of(its["user_id"])
        assert its["display_name"] == "Birthday Bot" and its["bot"] and "birthdays" in profile["bio"]
        assert base64.b64decode(profile["picture"]) == open(os.path.join(bots, "birthday_bot.png"), "rb").read()
        print("ok  birthday bot: asks a new member, reads the date, wishes in the direct chat and in #general; "
              "has a name, a line about itself and a picture")

        # A birthday in the profile, without a year; and a bot that joins is not asked.
        dave, dave_heard, dave_files = person("dave")
        dave.request({"cmd": "set_profile", "birthday": "05-17"})
        wait_for("a wish from the profile", lambda item: item == ("birthdays", "🎂 **Happy birthday**, @dave!"))
        bot_process("command_bot.py", address, "latecomer", os.path.join(tmp, "latecomer"))
        alice.say("#general", "!birthdays")
        wait_for("the list", lambda item: item[0] == "birthdays" and "- **carol**: 17 May (today 🎂)" in item[1]
                 and "- **dave**: 17 May (today 🎂)" in item[1])
        assert not [item for item in heard if "birthday bot" in item[1]], "someone here before the bot was asked"
        wait_for("the bot to say what it saw in the profile", lambda m: m.sender == "birthdays" and
                 "I see from your profile that your birthday is **17 May**" in m.body and "Tell me" not in m.body,
                 among=dave_heard)
        print("ok  birthday bot: a birthday from the profile, seen and not asked for; the list of those coming")
        # The other words it knows in a direct chat.
        with_bot = dave.dm("birthdays")
        for said, answer in (("when", "I have **17 May**, from your profile."),
                             ("17/05", "I can't tell the day from the month"),
                             ("private", "I'll wish you here only"),
                             ("what?", "I didn't understand that. I have your birthday as **17 May**, "
                                       "from your profile."),
                             ("forget", "Your profile says **17 May**, and I go by that"),
                             ("help", "## 🎂 Birthday Bot\n"
                                      "I wish people a happy birthday on the day, in a direct chat and in #general.\n\n"
                                      "**Your birthday:** 17 May, from your profile\n"
                                      "**Wished in #general:** no, here only\n\n"
                                      "**To change it, send me a date**\n- `05-17`"),
                             ("!help", "**Your birthday:** 17 May, from your profile")):
            dave.say(with_bot, said)
            wait_for(f"the answer to {said}", lambda m: m.sender == "birthdays" and answer in m.body, among=dave_heard)
        del heard[:]
        alice.say("#general", "!birthdays")
        listed = wait_for("the list again", lambda item: item[0] == "birthdays" and "**carol**: 17 May" in item[1])
        assert "dave" not in listed[1], "someone who asked to be wished privately was listed"
        # Someone who is asked first and fills in the profile afterwards is told that it was seen.
        erin, erin_heard, _ = person("erin")
        wait_for("erin to be asked", lambda m: "Tell me your birthday" in m.body, among=erin_heard)
        erin.say(erin.dm("birthdays"), "help")
        wait_for("help for someone it knows nothing of", lambda m: "**Your birthday:** not set yet" in m.body
                 and "**To set it, send me a date**" in m.body and "**Wished in #general:** yes" in m.body,
                 among=erin_heard)
        alice.say("#general", "!help")
        wait_for("help in a channel", lambda item: item[0] == "birthdays" and "## 🎂 Birthday Bot" in item[1]
                 and "`!birthdays`" in item[1] and "Your birthday" not in item[1])
        erin.request({"cmd": "set_profile", "birthday": "1999-12-25"})
        wait_for("the bot to notice", lambda m: m.direct and
                 "I see your birthday in your profile now: **25 December 1999**" in m.body, among=erin_heard)
        erin.say(erin.dm("birthdays"), "24 dec")
        wait_for("the two dates to be weighed",
                 lambda m: "Got it: **24 December**. I'll wish you a happy birthday then. Your profile says "
                           "**25 December 1999**; I'll go by what you told me here." in m.body, among=erin_heard)
        print("ok  birthday bot: help (in a chat and in a channel), when, private, forget, a date it cannot read, "
              "a profile filled in later, "
              "and the list leaves private people out")

        time.sleep(6)   # past the wait before a newcomer is asked, and a few looks at the clock
        running.terminate()
        running.wait()
        running = birthdays()
        time.sleep(6)
        assert not [item for item in heard if "Happy birthday" in item[1]], "wished again after a restart"
        running.terminate()
        running.wait()
        birthdays("--greet-existing")
        late = wait_for("alice to be asked when told to ask everyone", lambda item: "birthday bot" in item[1])
        alice.say(alice.dm("birthdays"), "No.")
        wait_for("no to be taken", lambda item: item[0] == "birthdays" and "I won't ask" in item[1])
        heard.remove(late)
        time.sleep(4)
        assert len([m for m in carol_heard if "birthday bot" in m.body]) == 1, "carol was asked twice"
        assert not [item for item in heard if "birthday bot" in item[1]], "alice was asked twice"
        people = json.load(open(os.path.join(tmp, "birthdays", "store.json")))["people"]
        asked_ids = {user for user, kept in people.items() if kept.get("asked")}
        names = {user: member["username"] for user, member in alice.members().items()} | {alice.me: "alice"}
        assert asked_ids == {carol.me, alice.me, dave.me, erin.me}, ("a bot was written to",
                                                   sorted(names.get(user, user) for user in asked_ids))
        assert people[alice.me].get("no") and people[dave.me].get("private")
        print("ok  birthday bot: nobody wished or asked twice after a restart; old members asked only with "
              "--greet-existing; bots never")

        # What only the owner and those with the Manage bots permission may tell it.
        to_bot = {who: who.dm("birthdays") for who in (alice, carol, dave)}
        carol.say(to_bot[carol], "list")
        wait_for("a refusal", lambda m: "Only the server's owner and people with the **Manage bots**" in m.body,
                 among=carol_heard)
        alice.say(to_bot[alice], "list")
        wait_for("the owner's list", lambda item: "**Birthdays I know (3)**" in item[1]
                 and "- **carol** (`carol`): 17 May; told me, wished in 2026" in item[1]
                 and "- **dave** (`dave`): 17 May; from the profile, wished in 2026, private" in item[1]
                 and "**No birthday yet:** alice (said no, asked)" in item[1] and "2004" not in item[1])
        del heard[:]
        alice.say(to_bot[alice], "reset wishes @carol")
        wait_for("the reset", lambda item: "Done: **carol** can be wished again this year." in item[1])
        wait_for("carol wished again", lambda item: item == ("birthdays", "🎂 **Happy birthday**, @carol! (22 today.)"))
        assert len([m for m in carol_heard if m.direct and "Happy birthday" in m.body]) == 2

        made = alice.request({"cmd": "create_role", "name": "botkeeper", "permissions": ["manage_bots"]})
        alice.request({"cmd": "grant_role", "username": "carol", "role": "botkeeper"})
        for _ in range(20):   # a role just given takes a moment to be known everywhere
            if carol.may(carol.me):
                break
            time.sleep(0.5)
        assert carol.may(carol.me), f"the role did not arrive: {made}"
        assert alice.may(alice.me) and not dave.may(dave.me)
        carol.say(to_bot[carol], "set @erin 17 may")
        wait_for("a date set by someone with the role", lambda m: "Done: **erin**'s birthday is **17 May**." in m.body,
                 among=carol_heard)
        wait_for("erin wished", lambda item: item == ("birthdays", "🎂 **Happy birthday**, @erin!"))
        carol.say(to_bot[carol], "help")
        wait_for("help with the part for managers", lambda m: "**Because you manage bots here**" in m.body
                 and "`reset wishes`" in m.body, among=carol_heard)
        before = len(dave_heard)
        dave.say(to_bot[dave], "help")
        said = wait_for("help without it", lambda m: dave_heard.index(m) >= before and "## 🎂 Birthday Bot" in m.body
                        and "Wished in" in m.body, among=dave_heard)
        assert "manage bots" not in said.body

        del heard[:]
        alice.say(to_bot[alice], "reset dave")
        wait_for("everything about dave forgotten", lambda item: "forgotten everything about **dave**" in item[1])
        wait_for("dave wished again, no longer privately",
                 lambda item: item == ("birthdays", "🎂 **Happy birthday**, @dave!"))
        alice.say(to_bot[alice], "ask everyone")
        wait_for("the count", lambda item: "Done: I wrote to 1 person." in item[1])
        assert len([m for m in dave_heard if "I see from your profile" in m.body]) == 2
        alice.say(to_bot[alice], "reset asked")
        wait_for("everyone unasked", lambda item: "people reset. I ask each of them when they next join" in item[1])
        alice.say(to_bot[alice], "ask @alice")
        wait_for("no respected", lambda item: "**alice** told me not to ask, so I won't." in item[1])
        alice.say(to_bot[alice], "reset wishes @nobody")
        wait_for("an unknown name", lambda item: "I don't know anyone called **nobody** here." in item[1])
        print("ok  birthday bot: list, reset, set and ask for the owner and for a role with Manage bots; "
              "refused to everyone else")

        # The channel it announces in: #general until told otherwise, and kept through a restart.
        alice.say(to_bot[alice], "channel")
        wait_for("where it announces", lambda item: "Birthdays are announced in **#general**." in item[1])
        alice.say(to_bot[alice], "channel #nowhere")
        wait_for("a channel it cannot see", lambda item: "I can't see a channel called **#nowhere**" in item[1]
                 and "`#general`" in item[1])
        alice.request({"cmd": "create_channel", "name": "parties"})
        time.sleep(3)
        alice.say(to_bot[alice], "channel parties")
        wait_for("the change", lambda item: "Done: birthdays are announced in **#parties** from now on." in item[1])
        alice.say(to_bot[alice], "time")
        wait_for("when it wishes", lambda item: "I wish people at **09:00**, by the clock" in item[1])
        alice.say(to_bot[alice], "time half past")
        wait_for("a time it cannot read", lambda item: "I can't read that as a time of day." in item[1])
        alice.say(to_bot[alice], "time 7:30 am")
        wait_for("the new time", lambda item: "Done: I wish people at **07:30** from now on" in item[1])
        for running_bot in started[-3:]:
            if "birthday_bot.py" in running_bot.args:
                running_bot.terminate()
                running_bot.wait()
        birthdays()
        alice.say(to_bot[alice], "channel")
        wait_for("the choice kept", lambda item: "Birthdays are announced in **#parties**." in item[1])
        alice.say(to_bot[alice], "time")
        wait_for("the time kept", lambda item: "I wish people at **07:30**, by the clock" in item[1])
        alice.say(to_bot[alice], "reset wishes @carol")
        wait_for("a wish in the new channel", lambda item: item == (alice.room_id("#parties"),
                 "🎂 **Happy birthday**, @carol! (22 today.)"), among=heard_where)
        print("ok  birthday bot: the channel is #general and the time 09:00 until a manager changes them, "
              "and the changes are kept")

        # The video bot, with the stand-in above for yt-dlp and a limit of 1 MB.
        fake = os.path.join(tmp, "fake-yt-dlp")
        open(fake, "w").write(FAKE_YT_DLP)
        os.chmod(fake, 0o755)
        alice_files = []
        alice.on_file(alice_files.append)
        bot_process("video_bot.py", address, "--yt-dlp", fake, "--max-size", "1", "--vault", os.path.join(tmp, "clips"))
        time.sleep(3)
        clip = "http://93.184.216.34/"    # an address on the public web that needs no looking up
        del heard[:]
        alice.say("#general", f"nobody asked for this one: {clip}unasked")
        asked_for = alice.say("#general", f"@clips {clip}watch.")["event_id"]
        got = wait_for("the video", lambda m: m.sender == "clips", among=alice_files)
        assert got.thread == asked_for and got.file["name"].endswith(".mp4") and got.file["size"] == 5000, got.raw
        assert got.body == "**A Test Clip**\n1:15 · 0.0 MB · videos.example", got.body
        time.sleep(2)
        assert len(alice_files) == 1, "it fetched a link nobody mentioned it for"

        second = alice.say("#general", f"another one {clip}second")["event_id"]
        carol.say("#general", "@clips", reply_to=second)
        wait_for("the video asked for in a reply", lambda m: m.thread == second, among=alice_files)
        carol.say("#general", "@clips audio please", thread=second)
        got = wait_for("the sound asked for in the thread", lambda m: m.thread == second
                       and m.file["name"].endswith(".mp3") and "sound only" in m.body, among=alice_files)
        carol._recent.clear()   # a message older than it remembers is read from its vault
        assert carol.lookup(got.room_id, second).body == f"another one {clip}second"
        big = carol.say("#general", f"@clips {clip}big")["event_id"]
        got = wait_for("a smaller copy of one too large", lambda m: m.thread == big, among=alice_files)
        assert got.file["size"] == 5000

        for path, reason in (("long", "That video is 2:30:00 long, and my limit is 20 minutes."),
                             ("list", "That is a list of videos."), ("live", "That is a live stream"),
                             ("fail", f"The site would not give it to me: Unsupported URL: {clip}fail")):
            alice.say("#general", f"@clips {clip}{path}")
            wait_for(f"a refusal for {path}", lambda item: item[0] == "clips" and reason in item[1])
        for private in ("http://127.0.0.1:9/video", "http://192.168.1.1/video", "http://[::1]/video"):
            del heard[:]
            alice.say("#general", f"@clips {private}")
            wait_for(f"a refusal for {private}", lambda item: item == ("clips", "That address is on a private "
                     "network. I only fetch from the public web."))
        alice.say("#general", f"@clips {clip}sixth")
        wait_for("the limit for one person", lambda item: item == ("clips", "That's 5 in 10 minutes. Give me a "
                                                                   "little while."))
        dave.say("#general", "@clips hello")
        wait_for("a hint", lambda item: item[0] == "clips" and "I didn't find a link." in item[1])
        print("ok  video bot: only when mentioned; with the link, in a reply and in a thread; sound only; a smaller "
              "copy when too large; refusals with their reason; never a private address")

        with_clips = dave.dm("clips")
        dave.say(with_clips, "help")
        said = wait_for("its help", lambda m: m.sender == "clips" and "## 🎬 Clips" in m.body, among=dave_heard)
        assert "**20 minutes** and **1 MB**, at **720p**" in said.body and "manage bots" not in said.body
        dave.say(with_clips, "limit minutes 300")
        wait_for("a refusal to change the limits", lambda m: m.sender == "clips" and "**Manage bots**" in m.body,
                 among=dave_heard)
        dave.say(with_clips, f"{clip}straight")
        wait_for("a video in a direct chat, without a mention", lambda m: m.sender == "clips" and m.direct,
                 among=dave_files)
        alice.say(alice.dm("clips"), "limit minutes 300")
        wait_for("the limit changed", lambda item: item[0] == "clips" and "Done." in item[1]
                 and "**300 minutes**" in item[1])
        carol.say("#general", f"@clips {clip}long")
        wait_for("a long video now let through", lambda m: m.sender == "clips" and "2:30:00" in m.body,
                 among=alice_files)
        assert json.load(open(os.path.join(tmp, "clips", "store.json")))["limits"] == {
            "minutes": 300, "size": 1, "quality": 720}
        print("ok  video bot: help, a link alone in a direct chat, limits changed by the owner and by nobody else")

        # Adult sites: only in a channel marked NSFW or in a direct chat, unless a manager says otherwise.
        alice.request({"cmd": "create_channel", "name": "after-dark"})
        time.sleep(3)
        alice.request({"cmd": "set_channel_nsfw", "room_id": alice.room_id("#after-dark"), "nsfw": True})
        time.sleep(3)
        del heard[:]
        dave.say("#general", "@clips https://www.pornhub.com/view_video.php?viewkey=abc")
        wait_for("an adult site refused in an ordinary channel", lambda item: item == ("clips", "That link is to an "
                 "adult site, and I only fetch those in a channel marked NSFW or in a direct chat with me."))
        dave.say("#general", f"@clips {clip}adult")
        wait_for("a video marked 18+ refused in an ordinary channel", lambda item: item == ("clips", "That video is "
                 "marked 18+, and I only fetch those in a channel marked NSFW or in a direct chat with me."))
        before = len(alice_files)
        marked = dave.say("#after-dark", f"@clips {clip}adult")["event_id"]
        wait_for("the same video in a channel marked NSFW", lambda m: m.thread == marked, among=alice_files)
        to_clips = alice.dm("clips")
        for said, answer in (("adult", "adult sites from the start"),
                             ("adult add somewhere.example", "Done: I count `somewhere.example`"),
                             ("adult add https://www.pornhub.com/x", "Done: I count `pornhub.com` as an adult site."),
                             ("adult off", "Done: nothing from adult sites")):
            alice.say(to_clips, said)
            wait_for(f"the answer to {said}", lambda item: item[0] == "clips" and answer in item[1])
        del heard[:]
        carol.say("#after-dark", "@clips https://clips.somewhere.example/1")
        wait_for("a named site refused everywhere once adult is off", lambda item: item == ("clips", "That link is to "
                 "an adult site, and I don't fetch those on this server."))
        kept = json.load(open(os.path.join(tmp, "clips", "store.json")))
        assert kept["adult"] == "off" and kept["adult_sites"] == ["somewhere.example"], kept
        assert len(alice_files) == before + 1
        earlier = len(dave_heard)
        dave.say(dave.dm("clips"), "adult anywhere")
        wait_for("someone who does not manage bots refused", lambda m: m.sender == "clips"
                 and "**Manage bots**" in m.body and dave_heard.index(m) >= earlier, among=dave_heard)
        alice.say(to_clips, "adult anywhere")
        wait_for("adult sites allowed anywhere", lambda item: item == ("clips", "Done: adult sites in any channel. "
                 "I'll remember that when I'm restarted."))
        anywhere = carol.say("#general", f"@clips {clip}adult")["event_id"]
        wait_for("a video marked 18+ in an ordinary channel, now allowed", lambda m: m.thread == anywhere,
                 among=alice_files)
        print("ok  video bot: adult sites and videos marked 18+ only where a channel is marked NSFW; a manager can "
              "turn them off, allow them anywhere, and name more sites")

        # A link inside a thread gets a thread of its own under it, not a place in the thread it is in.
        inner = alice.say("#general", f"and one more, in here: {clip}nested", thread=second)["event_id"]
        dave.say("#general", "@clips", thread=inner)
        wait_for("a video under a link that is itself in a thread", lambda m: m.thread == inner, among=alice_files)
        deeper = alice.say("#general", f"deeper still {clip}deeper", thread=inner)["event_id"]
        dave.say("#general", "@clips", reply_to=deeper, thread=inner)
        wait_for("and one level further down", lambda m: m.thread == deeper, among=alice_files)
        assert len([m for m in alice_files if m.thread == second]) == 2, "it posted into the thread the link was in"
        print("ok  video bot: a link in a thread gets its own thread under it, however deep")

        # The rest of the kit.
        assert mentioned_names("hi @Sage, mail me a@b.c or @sage-2") == {"sage", "sage-2"}
        assert Message({"content": {"body": "hey @SAGE!"}}, me="sage").mentions_me
        assert not Message({"content": {"body": "@sagebrush hello"}}, me="sage").mentions_me
        link = alice.say("#general", "a link: https://example.com/clip")["event_id"]
        alice.say("#general", "an answer", reply_to=link)
        note = os.path.join(tmp, "note.txt")
        open(note, "w").write("a file for the thread")
        alice.send_file("#general", note, "as asked", thread=link)
        answer = wait_for("the answer", lambda m: m.body == "an answer", among=carol_heard)
        assert answer.reply_to == link and carol.recall(link).body == "a link: https://example.com/clip"
        sent = wait_for("the file", lambda m: m.file["name"] == "note.txt", among=carol_files)
        assert sent.thread == link and sent.body == "as asked" and sent.file["size"] == 21
        order = []
        ahead = [alice.work(lambda n=n: (time.sleep(0.3), order.append(n))) for n in range(3)]
        time.sleep(1.5)
        assert ahead == [0, 1, 2] and order == [0, 1, 2], (ahead, order)
        print("ok  bot kit: mentions, what a message answers, a file into a thread, one job at a time")
        print("PASS")
    finally:
        for p in started:
            p.terminate()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
