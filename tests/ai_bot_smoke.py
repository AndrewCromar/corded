#!/usr/bin/env python3
"""Runs the AI bot against a real server with a stand-in for the model, and
checks where and how it answers.

    python3 ai_bot_smoke.py <folder with cordedd and corded-cli>
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

bin_dir = os.path.abspath(sys.argv[1])
repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
bots = os.path.join(repo, "bots")
sys.path.insert(0, bots)
from corded_bot import Bot  # noqa: E402

os.environ["CORDED_CLI"] = os.path.join(bin_dir, "corded-cli")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


asked = []   # every request the "model" got
painted = [] # every picture it asked the picture program for
PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAAAAAA6fptVAAAACklEQVR4nGNoAAAAggCBd81ytgAAAABJRU5ErkJggg=="
gates = []   # every time it was asked whether a message was meant for the bot
unloads = [] # every time it was told to let go of the graphics card


class Model(BaseHTTPRequestHandler):
    """Answers by what the last message says, the way a model would choose."""

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if "prompt" in body and "width" in body:   # the picture program, asked for a profile picture
            painted.append(body)
            raw = json.dumps({"images": [PNG], "info": "{}"}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(raw)
            return
        if "messages" not in body:   # Ollama's "unload this model", sent when the bot is turned off
            unloads.append(body)
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"{}")
            return
        last = [m for m in body["messages"] if m["role"] == "user"][-1]["content"].lower()
        if "for_bot" not in body["messages"][0]["content"] and "decide whether it asks" not in body["messages"][0]["content"]:
            asked.append(body)
        if "decide whether it asks the bot to do one of a few things" in body["messages"][0]["content"]:
            # The question asked first about a message that sounds like a wish.
            if "remember that" in last:
                words = json.dumps({"do": "remember", "text": "alice takes her tea without milk"})
            elif "new personality" in last:
                words = json.dumps({"do": "create", "name": "Captain", "text": "You are Captain, an old sailor. You talk like one."})
            elif "switch to" in last:
                words = json.dumps({"do": "switch", "name": "captain"})
            elif "from now on" in last:
                words = json.dumps({"do": "change", "text": "You end every answer with arr."})
            elif "profile picture" in last:
                words = json.dumps({"do": "picture", "text": "an owl wearing a sailor's cap"})
            elif "which personalities" in last:
                words = json.dumps({"do": "list"})
            else:
                words = json.dumps({"do": "none"})
        elif "for_bot" in body["messages"][0]["content"]:
            # The question asked first when nobody called the bot: is this for it?
            last = last.split("last message:")[-1]
            gates.append(last)
            words = json.dumps({"for_bot": "between us" not in last})
        elif "thanks" in last:
            words = json.dumps({"action": "react", "emoji": "👍", "text": ""})
        elif "explain" in last:
            words = json.dumps({"action": "thread", "text": "It works like this."})
        elif "which one" in last:
            words = json.dumps({"action": "reply", "to": 1, "text": "The first."})
        elif "long" in last:
            words = json.dumps({"action": "message", "text": "\n\n".join(["A paragraph of words. " * 60] * 6)})
        elif "empty" in last:
            words = "{}"
        elif "plain" in last:
            words = "Just words, no form. @everyone"
        else:
            words = json.dumps({"action": "message", "text": "Noted."})
        answer = json.dumps({"choices": [{"message": {"content": words}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(answer)

    def log_message(self, *_):
        pass


def main():
    tmp = tempfile.mkdtemp(prefix="corded-aibot-")
    started = []
    try:
        port, model_port = free_port(), free_port()
        address = f"127.0.0.1:{port}"
        started.append(subprocess.Popen([os.path.join(bin_dir, "cordedd"), "--port", str(port), "--data",
                                         os.path.join(tmp, "server"), "--owner", "alice"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1)
        threading.Thread(target=HTTPServer(("127.0.0.1", model_port), Model).serve_forever, daemon=True).start()

        def person(name):
            heard = []
            someone = Bot(vault=os.path.join(tmp, name), username=name, is_bot=False, hear_bots=True)
            someone.on_message(heard.append)
            someone.connect(address)
            threading.Thread(target=someone.run, daemon=True).start()
            return someone, heard

        def wait_for(what, heard, test, seconds=25):
            end = time.time() + seconds
            while time.time() < end:
                for m in list(heard):
                    if m.sender == "sage" and test(m):
                        return m
                time.sleep(0.2)
            raise AssertionError(f"did not hear {what}; heard {[(m.sender, m.body[:60]) for m in heard]}")

        def start_bot():
            p = subprocess.Popen([sys.executable, "ai_bot.py", address, "--api", f"http://127.0.0.1:{model_port}/v1",
                                  "--username", "sage", "--vault", os.path.join(tmp, "sage"), "--per-minute", "100",
                                  "--pictures", f"http://127.0.0.1:{model_port}"],
                                 cwd=bots, stdout=subprocess.PIPE, text=True)
            started.append(p)
            line = p.stdout.readline()
            assert "connected" in line, line
            threading.Thread(target=lambda: [None for _ in p.stdout], daemon=True).start()
            return p

        alice, alice_heard = person("alice")
        bob, bob_heard = person("bob")
        sage = start_bot()
        time.sleep(3)

        # The standing text: who it is, then the chat's own rules, then the form to answer in.
        alice.say("#general", "nobody asked the bot this")
        alice.say("#general", "@sage hello there")
        wait_for("an answer to a mention", alice_heard, lambda m: m.body == "Noted.")
        assert len(asked) == 1, "it answered something it was not asked"
        system = asked[0]["messages"][0]["content"]
        assert "a bot in a Corded chat" in system and '"action"' in system and "the channel #general" in system
        assert "`!ai here`" in system and "Channels chosen for you right now: none" in system, "it is not told about itself"
        assert asked[0]["messages"][-1]["content"].endswith("alice: @sage hello there")
        print("ok  mention: answered, with the chat's rules and the form to answer in sent along")

        # Carrying on needs no second mention; someone else cutting in does.
        requests = len(asked)
        alice.say("#general", "and one more thing")
        for _ in range(60):
            if len(asked) > requests:
                break
            time.sleep(0.2)
        assert len(asked) == requests + 1, "the next message of the person just answered was not taken as said to it"
        time.sleep(2)
        bob.say("#general", "unrelated chatter from someone else")
        time.sleep(3)
        assert len(asked) == requests + 1, "it answered someone who was not talking to it"
        print("ok  follow-up: the person just answered can carry on without a mention; others are not answered")

        # A direct chat is always answered, with a plain message.
        direct = bob.dm("sage", "hi, anyone home?")
        said = wait_for("an answer in the direct chat", bob_heard, lambda m: m.room_id == direct and m.body == "Noted.")
        assert not said.reply_to and not said.thread
        assert "a direct chat" in asked[-1]["messages"][0]["content"]
        print("ok  direct chat: answered without a mention, as a plain message")

        # A reaction instead of words.
        before = len(alice_heard)
        thanked = alice.say("#general", "@sage thanks")
        time.sleep(4)
        events = alice.request({"cmd": "fetch_timeline", "room_id": alice.room_id("#general"), "limit": 30})["events"]
        reactions = [e for e in events if e.get("type") == "m.reaction"
                     and (e.get("relation") or {}).get("target") == thanked["event_id"]]
        assert len(reactions) == 1 and reactions[0]["relation"]["key"] == "👍", reactions
        assert not [m for m in alice_heard[before:] if m.sender == "sage"], "it also spoke"
        print("ok  react: a thank-you gets an emoji and no words")

        # A thread under the question; a follow-up there needs no mention.
        question = alice.say("#general", "@sage explain how it works")
        first = wait_for("an answer in a thread", alice_heard, lambda m: m.body == "It works like this.")
        assert first.thread == question["event_id"], first.raw
        requests = len(asked)
        alice.say("#general", "and then what?", thread=question["event_id"])
        follow = wait_for("a follow-up answer", alice_heard, lambda m: m.body == "Noted." and m.thread == question["event_id"])
        assert follow and len(asked) == requests + 1
        assert "a thread in the channel" in asked[-1]["messages"][0]["content"]
        said_in_thread = [m["content"] for m in asked[-1]["messages"][1:]]
        assert any("explain how it works" in c for c in said_in_thread) and not any("hello there" in c for c in said_in_thread), \
            "a thread's talk is kept apart from the channel's"
        print("ok  thread: answered under the question; the thread is followed without a mention, with its own history")

        # Channels are chosen by those who manage bots.
        bob.say("#general", "!ai here")
        wait_for("a refusal", alice_heard, lambda m: "Only the owner" in m.body)
        alice.say("#general", "!ai here")
        wait_for("the channel being taken", alice_heard, lambda m: "I am part of this channel" in m.body)
        requests = len(asked)
        bob.say("#general", "tea or coffee")
        bob.say("#general", "which one would you pick")
        picked = wait_for("a reply", alice_heard, lambda m: m.body == "The first.")
        assert picked.reply_to, "the model asked for a reply to a message"
        assert len(asked) == requests + 2
        assert "Channels chosen for you right now: #general" in asked[-1]["messages"][0]["content"]
        alice.say("#general", "!ai leave #general")
        wait_for("leaving by name", alice_heard, lambda m: "In #general I will only answer when I am addressed" in m.body)
        alice.say("#general", "!ai here #nowhere")
        wait_for("an unknown channel", alice_heard, lambda m: "no channel called #nowhere" in m.body)
        alice.say("#general", "!ai here #general")
        wait_for("choosing by name", alice_heard, lambda m: "I am part of #general" in m.body)
        alice.say("#general", "!ai here always")
        wait_for("answering everything", alice_heard, lambda m: "answer every message in this channel" in m.body)
        requests, judged = len(asked), len(gates)
        bob.say("#general", "alice, between us, one more thing")
        for _ in range(60):
            if len(asked) > requests:
                break
            time.sleep(0.2)
        assert len(asked) == requests + 1 and len(gates) == judged, "in a channel that is its own it answers without judging"
        alice.say("#general", "!ai here")
        wait_for("judging again", alice_heard[-1:] + alice_heard, lambda m: "I am part of this channel" in m.body)
        time.sleep(1)
        alice.say("#general", "!ai where")
        wait_for("the list", alice_heard, lambda m: "I take part without a mention in: #general" in m.body)
        # In its channel it reads everything and may stay out of it; called by name it may not.
        requests, judged, heard = len(asked), len(gates), len(alice_heard)
        bob.say("#general", "alice, between us, did you see the match")
        for _ in range(60):
            if len(gates) > judged:
                break
            time.sleep(0.2)
        time.sleep(2)
        assert len(gates) == judged + 1 and "did you see the match" in gates[-1]
        spoke = [m.body for m in alice_heard[heard:] if m.sender == "sage"]
        assert len(asked) == requests and not spoke, f"it spoke where it judged it was not meant: {spoke}"
        bob.say("#general", "@sage between us, what do you think")
        for _ in range(60):
            if len(asked) > requests:
                break
            time.sleep(0.2)
        assert len(asked) == requests + 1 and len(gates) == judged + 1, "called by name, it does not ask itself first"
        print("ok  channels: only a manager can choose one; there it reads everything, may stay out, and must answer its name")

        # A model that ignores the form is taken at its word; no @everyone gets out.
        alice.say("#general", "say it plain")
        plain = wait_for("plain words", alice_heard, lambda m: m.body.startswith("Just words"))
        assert "@everyone" not in plain.body and plain.reply_to
        # A model that returns the form with nothing in it is not quoted: a nod instead.
        heard = len(alice_heard)
        alice.say("#general", "@sage an empty one")
        time.sleep(4)
        assert not [m.body for m in alice_heard[heard:] if m.sender == "sage"], "it sent an empty form as its words"
        # A long answer goes out in pieces.
        before = len(alice_heard)
        alice.say("#general", "a long one please")
        wait_for("a long answer", alice_heard[before:] or alice_heard, lambda m: m.body.startswith("A paragraph"))
        time.sleep(3)
        parts = [m for m in alice_heard[before:] if m.sender == "sage" and m.body.startswith("A paragraph")]
        assert len(parts) >= 2 and all(len(m.body) <= 3500 for m in parts), [len(m.body) for m in parts]
        print("ok  plain words are sent as they are, without @everyone; a long answer is cut at paragraph ends")

        # Personalities: remembering, making one by describing it, switching, changing; each with its own memories.
        def shown_name():
            for m in alice.request({"cmd": "member_list"})["members"]:
                if m["username"] == "sage":
                    return m["display_name"]
        system = lambda: asked[-1]["messages"][0]["content"]   # noqa: E731
        alice.say("#general", "@sage remember that I take my tea without milk")
        time.sleep(3)
        assert "have now stored it for good" in system(), "the model is told what was done, to say so itself"
        alice.say("#general", "@sage what do you know about me")
        wait_for("an answer", alice_heard[-1:] + alice_heard, lambda m: m.body == "Noted.")
        time.sleep(1)
        assert "alice takes her tea without milk" in system() and "# What you remember" in system()
        bob.say("#general", "@sage here is a new personality for you: a sailor")
        time.sleep(3)
        assert "was not done" in system() and "can switch or change my personalities" in system()
        alice.say("#general", "@sage here is a new personality for you: a sailor called Captain")
        time.sleep(3)
        assert "Made: Captain, and that is who I am now" in system() and "You are Captain, an old sailor" in system(), \
            "a new one is put on at once and confirms itself"
        alice.say("#general", "@sage which personalities do you have")
        time.sleep(3)
        assert "exactly these and no others: Captain (the one you are now), sage" in system()
        for _ in range(50):
            if shown_name() == "Captain":
                break
            time.sleep(0.2)
        assert shown_name() == "Captain", shown_name()
        alice.say("#general", "@sage who are you now")
        time.sleep(3)
        assert "You are Captain, an old sailor" in system() and "tea without milk" not in system(), \
            "the new personality has its own character and none of the other's memories"
        assert "- Captain (the one you are now)" in system() and "- sage:" in system()
        alice.say("#general", "@sage from now on end every answer with arr")
        time.sleep(3)
        assert "Noted in my character" in system()
        alice.say("#general", "@sage and again")
        time.sleep(3)
        assert "You end every answer with arr." in system()
        if shutil.which("ffmpeg"):
            alice.say("#general", "@sage make yourself a new profile picture, an owl in a sailor's cap")
            time.sleep(5)
            assert painted and "an owl wearing a sailor's cap" in painted[-1]["prompt"], "it asks the picture program itself"
            assert os.path.exists(os.path.join(tmp, "sage", "personas", "captain", "picture.jpg")), "kept with the personality"
            assert "made yourself a new profile picture and put it on" in system()
        alice.say("#general", "!persona")
        wait_for("the list", alice_heard, lambda m: "**Captain** (now)" in m.body and "**sage**" in m.body)
        alice.say("#general", "!persona new Owl: You are Owl. You speak rarely and wisely.")
        wait_for("one made by command", alice_heard, lambda m: m.body.startswith("Made: Owl"))
        alice.say("#general", "!persona sage")
        wait_for("switching back, by its name alone", alice_heard, lambda m: m.body == "Now: sage.")
        alice.say("#general", "!persona delete owl")
        wait_for("one removed", alice_heard, lambda m: m.body == "Removed: Owl.")
        alice.say("#general", "@sage back to you")
        time.sleep(3)
        assert "tea without milk" in system() and "end every answer with arr" not in system(), "back as the first, with its memories and without the other's changes"
        print("ok  personalities: remembers when told; made, switched and changed by talking or by command; each has its own memories; only a manager reshapes")

        # Off and on, by those who manage bots; off, it answers nothing.
        bob.say("#general", "!sage off")
        wait_for("a refusal to be turned off", alice_heard, lambda m: "can turn me off" in m.body)
        alice.say("#general", "!sage off")
        wait_for("going quiet", alice_heard, lambda m: "Going quiet" in m.body)
        time.sleep(1)
        requests = len(asked)
        alice.say("#general", "@sage are you there")
        bob.say("#general", "!sage on")
        time.sleep(4)
        assert len(asked) == requests, "it answered while turned off"
        assert unloads and unloads[-1].get("keep_alive") == 0, "turned off, it did not ask for its model to be unloaded"
        alice.say("#general", "!sage status")
        wait_for("its state", alice_heard, lambda m: "I am off" in m.body)
        alice.say("#general", "!sage on")
        wait_for("coming back", alice_heard, lambda m: m.body == "I am back.")
        alice.say("#general", "@sage and now")
        for _ in range(60):
            if len(asked) > requests:
                break
            time.sleep(0.2)
        assert len(asked) == requests + 1, [m["messages"][-1]["content"][:60] for m in asked[requests:]]
        print("ok  off and on: only a manager can; turned off it answers nothing but the command that wakes it")

        # After a restart it still knows what was said, and where it answers.
        sage.terminate()
        sage.wait(timeout=10)
        sage = start_bot()
        time.sleep(3)
        requests = len(asked)
        bob.say("#general", "still with us?")
        wait_for("an answer after the restart", alice_heard[-1:] + bob_heard[-3:] + alice_heard, lambda m: m.body == "Noted.")
        for _ in range(50):
            if len(asked) > requests:
                break
            time.sleep(0.2)
        earlier = [m["content"] for m in asked[-1]["messages"][1:]]
        assert any("@sage and now" in c for c in earlier), "the chat before the restart was forgotten"
        assert any(m["role"] == "assistant" for m in asked[-1]["messages"]), "its own earlier words were forgotten"
        alice.say("#general", "!forget")
        wait_for("forgetting", alice_heard, lambda m: "Forgotten" in m.body)
        requests = len(asked)
        bob.say("#general", "a fresh start")
        for _ in range(100):
            if len(asked) > requests:
                break
            time.sleep(0.2)
        assert len(asked[-1]["messages"]) <= 4, [m["content"][:40] for m in asked[-1]["messages"]]
        print("ok  restart: the chat and its chosen channel are remembered; !forget starts fresh")
        print("all AI bot checks passed")
    finally:
        for p in started:
            p.terminate()
        for p in started:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    main()
    sys.stdout.flush()
    os._exit(0)
