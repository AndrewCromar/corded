#!/usr/bin/env python3
"""Runs the example bots against a throwaway server and talks to them.

    python3 bots_smoke.py <folder with cordedd and corded-cli>
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
import urllib.request
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
        alice = Bot(vault=os.path.join(tmp, "alice"), username="alice")
        alice.on_message(lambda m: heard.append((m.sender, m.body)))
        alice.connect(address)
        threading.Thread(target=alice.run, daemon=True).start()

        def wait_for(what, test, seconds=25):
            end = time.time() + seconds
            while time.time() < end:
                for item in heard:
                    if test(item):
                        return item
                time.sleep(0.2)
            raise AssertionError(f"did not hear {what}; heard {heard}")

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
        print("PASS")
    finally:
        for p in started:
            p.terminate()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
