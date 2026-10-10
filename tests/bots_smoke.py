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
        wait_for("the date read back", lambda m: m.sender == "birthdays" and "Got it: 17 May 2004" in m.body,
                 among=carol_heard)
        assert "That's today!" in carol_heard[-1].body or any("That's today!" in m.body for m in carol_heard)
        wait_for("a wish in the direct chat", lambda m: m.direct and m.body == "🎂 Happy birthday, carol! 22 today.",
                 among=carol_heard)
        wait_for("a wish in the channel", lambda item: item == ("birthdays", "🎂 Happy birthday, @carol! (22 today.)"))
        its = next(m for m in carol.members().values() if m["username"] == "birthdays")
        profile = carol.profile_of(its["user_id"])
        assert its["display_name"] == "Birthday Bot" and its["bot"] and "birthdays" in profile["bio"]
        assert base64.b64decode(profile["picture"]) == open(os.path.join(bots, "birthday_bot.png"), "rb").read()
        print("ok  birthday bot: asks a new member, reads the date, wishes in the direct chat and in #general; "
              "has a name, a line about itself and a picture")

        # A birthday in the profile, without a year; and a bot that joins is not asked.
        dave, dave_heard, _ = person("dave")
        dave.request({"cmd": "set_profile", "birthday": "05-17"})
        wait_for("a wish from the profile", lambda item: item == ("birthdays", "🎂 Happy birthday, @dave!"))
        bot_process("command_bot.py", address, "latecomer", os.path.join(tmp, "latecomer"))
        alice.say("#general", "!birthdays")
        wait_for("the list", lambda item: item[0] == "birthdays" and "carol: 17 May (today)" in item[1]
                 and "dave: 17 May (today)" in item[1])
        assert not [item for item in heard if "birthday bot" in item[1]], "someone here before the bot was asked"
        wait_for("the bot to say what it saw in the profile", lambda m: m.sender == "birthdays" and
                 "I see from your profile that your birthday is 17 May" in m.body and "Tell me" not in m.body,
                 among=dave_heard)
        print("ok  birthday bot: a birthday from the profile, seen and not asked for; the list of those coming")
        # The other words it knows in a direct chat.
        with_bot = dave.dm("birthdays")
        for said, answer in (("when", "I have 17 May, from your profile."),
                             ("17/05", "I can't tell the day from the month"),
                             ("private", "I'll wish you here only"),
                             ("what?", "I didn't understand that. I have your birthday as 17 May, from your profile."),
                             ("forget", "Your profile says 17 May, and I go by that")):
            dave.say(with_bot, said)
            wait_for(f"the answer to {said}", lambda m: m.sender == "birthdays" and answer in m.body, among=dave_heard)
        del heard[:]
        alice.say("#general", "!birthdays")
        listed = wait_for("the list again", lambda item: item[0] == "birthdays" and "carol: 17 May" in item[1])
        assert "dave" not in listed[1], "someone who asked to be wished privately was listed"
        # Someone who is asked first and fills in the profile afterwards is told that it was seen.
        erin, erin_heard, _ = person("erin")
        wait_for("erin to be asked", lambda m: "Tell me your birthday" in m.body, among=erin_heard)
        erin.request({"cmd": "set_profile", "birthday": "1999-12-25"})
        wait_for("the bot to notice", lambda m: m.direct and
                 "I see your birthday in your profile now: 25 December 1999" in m.body, among=erin_heard)
        erin.say(erin.dm("birthdays"), "24 dec")
        wait_for("the two dates to be weighed", lambda m: "Got it: 24 December. I'll wish you a happy birthday then. "
                 "Your profile says 25 December 1999; I'll go by what you told me here." in m.body, among=erin_heard)
        print("ok  birthday bot: when, private, forget, a date it cannot read, a profile filled in later, "
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
