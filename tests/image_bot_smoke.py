#!/usr/bin/env python3
"""Runs the image bot against a real server with a stand-in for the picture
program, and checks what it asks for and what it sends back.

    python3 image_bot_smoke.py <folder with cordedd and corded-cli>
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
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

bin_dir = os.path.abspath(sys.argv[1])
repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
bots = os.path.join(repo, "bots")
sys.path.insert(0, bots)
from corded_bot import Bot  # noqa: E402

os.environ["CORDED_CLI"] = os.path.join(bin_dir, "corded-cli")
# The smallest PNG there is: one grey pixel.
PNG = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAAAAAA6fptVAAAACklEQVR4nGNoAAAAggCBd81ytgAAAABJRU5ErkJggg==")
asked = []
slow = {"seconds": 0.0}


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Pictures(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        asked.append(body)
        time.sleep(slow["seconds"])
        seed = body["seed"] if body["seed"] >= 0 else 4242
        answer = json.dumps({"images": [base64.b64encode(PNG).decode()], "info": json.dumps({"seed": seed})}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(answer)

    def log_message(self, *_):
        pass


def main():
    tmp = tempfile.mkdtemp(prefix="corded-imgbot-")
    started = []
    try:
        port, api_port = free_port(), free_port()
        address = f"127.0.0.1:{port}"
        started.append(subprocess.Popen([os.path.join(bin_dir, "cordedd"), "--port", str(port), "--data",
                                         os.path.join(tmp, "server"), "--owner", "alice"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1)
        threading.Thread(target=ThreadingHTTPServer(("127.0.0.1", api_port), Pictures).serve_forever, daemon=True).start()

        def person(name):
            texts, files = [], []
            someone = Bot(vault=os.path.join(tmp, name), username=name, is_bot=False, hear_bots=True)
            someone.on_message(texts.append)
            someone.on_file(files.append)
            someone.connect(address)
            threading.Thread(target=someone.run, daemon=True).start()
            return someone, texts, files

        def wait_for(what, heard, test, seconds=25):
            end = time.time() + seconds
            while time.time() < end:
                for m in list(heard):
                    if m.sender == "pictures" and test(m):
                        return m
                time.sleep(0.2)
            raise AssertionError(f"did not get {what}; got {[(m.sender, m.body[:70]) for m in heard]}")

        alice, alice_texts, alice_files = person("alice")
        bob, bob_texts, bob_files = person("bob")
        p = subprocess.Popen([sys.executable, "image_bot.py", address, "--api", f"http://127.0.0.1:{api_port}",
                              "--ollama", "", "--vault", os.path.join(tmp, "pictures")],
                             cwd=bots, stdout=subprocess.PIPE, text=True)
        started.append(p)
        assert "connected" in p.stdout.readline()
        threading.Thread(target=lambda: [None for _ in p.stdout], daemon=True).start()
        time.sleep(3)

        # A description with options: the right request goes out, the picture comes back as a reply.
        asking = alice.say("#general", "!imagine a red fox in the snow --wide --seed 7 --no fog, people")
        got = wait_for("the picture", alice_files, lambda m: True)
        assert got.file["mime"] == "image/png" and got.reply_to == asking["event_id"], got.raw
        content = got.raw.get("content") or {}
        if shutil.which("ffmpeg"):   # with it, the apps show the picture in the chat itself
            assert content.get("thumbnail") and content.get("width") == 1 and content.get("height") == 1, content.keys()
        caption = content.get("caption", "") or got.body
        assert "a red fox in the snow" in caption and "--seed 7" in caption and "--wide" in caption, caption
        sent = asked[-1]
        assert sent["prompt"] == "a red fox in the snow" and (sent["width"], sent["height"]) == (1216, 832)
        assert sent["seed"] == 7 and sent["negative_prompt"].startswith("fog, people") and "nsfw" in sent["negative_prompt"]
        print("ok  !imagine: the description and its options reach the picture program; the picture comes back as a reply")

        # Mistakes are explained; adult words are refused; nothing is asked of the picture program.
        requests = len(asked)
        alice.say("#general", "!imagine a cat --huge")
        wait_for("an unknown option", alice_texts, lambda m: "I do not know `--huge`" in m.body)
        alice.say("#general", "!imagine")
        wait_for("an empty description", alice_texts, lambda m: "Say what to draw" in m.body)
        alice.say("#general", "!imagine a nude statue")
        wait_for("a refusal", alice_texts, lambda m: "do not make adult pictures" in m.body)
        assert len(asked) == requests
        print("ok  an unknown option, an empty description and an adult one are answered in words, with no picture made")

        # A mention, and a direct chat, take plain words.
        before = len(alice_files)
        alice.say("#general", "@pictures a teapot shaped like a snail --tall")
        for _ in range(100):
            if len(alice_files) > before:
                break
            time.sleep(0.2)
        assert asked[-1]["prompt"] == "a teapot shaped like a snail" and asked[-1]["height"] == 1216
        direct = bob.dm("pictures", "a paper boat on a puddle")
        wait_for("a picture in the direct chat", bob_files, lambda m: m.room_id == direct)
        assert asked[-1]["prompt"] == "a paper boat on a puddle"
        print("ok  a mention and a direct chat take a description without the command")

        # One at a time, in order; the line can be seen and left.
        slow["seconds"] = 3.0
        before = len(alice_files)
        alice.say("#general", "!imagine first in line")
        time.sleep(0.6)
        waiting = bob.say("#general", "!imagine second in line")
        wait_for("its place in the line", alice_texts, lambda m: m.body == "1 ahead of you.")
        alice.say("#general", "!queue")
        wait_for("the line", alice_texts, lambda m: "now: alice: first in line" in m.body and "bob: second in line" in m.body)
        bob.say("#general", "!cancel")
        wait_for("leaving the line", alice_texts, lambda m: "Taken out of the line: 1" in m.body)
        time.sleep(5)
        slow["seconds"] = 0.0
        assert "second in line" not in [a["prompt"] for a in asked], "a cancelled request was still made"
        assert len(alice_files) == before + 1 and waiting
        alice.say("#general", "!again")
        for _ in range(100):
            if len(alice_files) > before + 1:
                break
            time.sleep(0.2)
        assert asked[-1]["prompt"] == "first in line" and asked[-1]["seed"] == -1
        print("ok  the line: the second waits and is told so, can leave it, and !again repeats the last with a new seed")

        # Five in ten minutes for each person.
        alice.say("#general", "!imagine one more")     # alice's fifth
        alice.say("#general", "!imagine one too many")
        wait_for("the limit", alice_texts, lambda m: "5 pictures in ten minutes" in m.body)
        alice.say("#general", "!images limit 0")
        wait_for("the limit lifted", alice_texts, lambda m: m.body == "No limit on pictures now.")
        requests = len(asked)
        alice.say("#general", "!imagine one too many after all")
        for _ in range(100):
            if len(asked) > requests:
                break
            time.sleep(0.2)
        assert asked[-1]["prompt"] == "one too many after all"
        print("ok  the sixth request in ten minutes is refused; a manager can change the limit, and 0 lifts it")

        # Channels chosen by those who manage bots; off and on.
        bob.say("#general", "!images here")
        wait_for("a refusal", alice_texts, lambda m: "Only the owner" in m.body)
        alice.request({"cmd": "create_channel", "name": "art"})
        time.sleep(2)
        alice.say("#general", "!images here #art")
        wait_for("the channel being chosen", alice_texts, lambda m: "I make pictures in #art" in m.body)
        requests = len(asked)
        bob.say("#general", "!imagine not here any more")
        time.sleep(3)
        assert len(asked) == requests, "it made a picture outside its chosen channels"
        alice.say("#general", "!pictures off")
        wait_for("going quiet", alice_texts, lambda m: "Going quiet" in m.body)
        bob.say("#art", "!imagine while it is off")
        time.sleep(3)
        assert len(asked) == requests, "it made a picture while turned off"
        alice.say("#general", "!pictures on")
        wait_for("coming back", alice_texts, lambda m: m.body == "I am back.")
        bob.say("#art", "!imagine back in the right place")
        for _ in range(100):
            if len(asked) > requests:
                break
            time.sleep(0.2)
        assert asked[-1]["prompt"] == "back in the right place"
        print("ok  chosen channels: only a manager chooses; elsewhere it stays silent; off and on")
        print("all image bot checks passed")
    finally:
        for proc in started:
            proc.terminate()
        for proc in started:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    main()
    sys.stdout.flush()
    os._exit(0)
