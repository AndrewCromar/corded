#!/usr/bin/env python3
"""Runs the AI bot against a real server with a stand-in for the model, and
checks where and how it answers.

    python3 ai_bot_smoke.py <folder with cordedd and corded-cli>
"""
import json
import os
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


class Model(BaseHTTPRequestHandler):
    """Answers by what the last message says, the way a model would choose."""

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        asked.append(body)
        last = [m for m in body["messages"] if m["role"] == "user"][-1]["content"].lower()
        if "thanks" in last:
            words = json.dumps({"action": "react", "emoji": "👍", "text": ""})
        elif "explain" in last:
            words = json.dumps({"action": "thread", "text": "It works like this."})
        elif "which one" in last:
            words = json.dumps({"action": "reply", "to": 1, "text": "The first."})
        elif "long" in last:
            words = json.dumps({"action": "message", "text": "\n\n".join(["A paragraph of words. " * 60] * 6)})
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
                                  "--username", "sage", "--vault", os.path.join(tmp, "sage")],
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
        wait_for("the channel being taken", alice_heard, lambda m: "every message in this channel" in m.body)
        requests = len(asked)
        bob.say("#general", "tea or coffee")
        bob.say("#general", "which one would you pick")
        picked = wait_for("a reply", alice_heard, lambda m: m.body == "The first.")
        assert picked.reply_to, "the model asked for a reply to a message"
        assert len(asked) == requests + 2
        alice.say("#general", "!ai where")
        wait_for("the list", alice_heard, lambda m: "#general" in m.body and "every message in" in m.body)
        print("ok  channels: only a manager can choose one; there every message is answered, a reply attached to its message")

        # A model that ignores the form is taken at its word; no @everyone gets out.
        alice.say("#general", "say it plain")
        plain = wait_for("plain words", alice_heard, lambda m: m.body.startswith("Just words"))
        assert "@everyone" not in plain.body and plain.reply_to
        # A long answer goes out in pieces.
        before = len(alice_heard)
        alice.say("#general", "a long one please")
        wait_for("a long answer", alice_heard[before:] or alice_heard, lambda m: m.body.startswith("A paragraph"))
        time.sleep(3)
        parts = [m for m in alice_heard[before:] if m.sender == "sage" and m.body.startswith("A paragraph")]
        assert len(parts) >= 2 and all(len(m.body) <= 3500 for m in parts), [len(m.body) for m in parts]
        print("ok  plain words are sent as they are, without @everyone; a long answer is cut at paragraph ends")

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
        assert any("tea or coffee" in c for c in earlier), "the chat before the restart was forgotten"
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
