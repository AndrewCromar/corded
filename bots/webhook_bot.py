#!/usr/bin/env python3
"""Posts what other programs tell it into a channel.

    python3 webhook_bot.py SERVER:PORT '#channel' [--port 8765] [--secret WORD]

It listens on this computer only (127.0.0.1). Two addresses:

  POST /say      a JSON body {"text": "..."} is posted as it is. With --secret,
                 the request must carry the header  X-Corded-Secret: WORD.
  POST /github   a GitHub webhook. Pushes, issues, pull requests and releases
                 are put into a sentence. With --secret, GitHub's signature
                 (X-Hub-Signature-256, made with the same word) is checked.

To let GitHub reach it, put it behind something that forwards to this port
(a tunnel, or a web server you already run); do not open the port itself.
"""
import argparse
import hashlib
import hmac
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from corded_bot import Bot

parser = argparse.ArgumentParser()
parser.add_argument("address")
parser.add_argument("channel")
parser.add_argument("--port", type=int, default=8765)
parser.add_argument("--secret", default="")
parser.add_argument("--username", default="webhooks")
parser.add_argument("--vault", default="./webhooks-vault")
args = parser.parse_args()

bot = Bot(vault=args.vault, username=args.username, about="I post what other programs send me.")


def github_sentence(kind, body):
    """What happened, in a line; None for events not worth a message."""
    repo = (body.get("repository") or {}).get("full_name", "a repository")
    who = (body.get("sender") or {}).get("login", "someone")
    if kind == "push":
        commits = body.get("commits") or []
        branch = body.get("ref", "").rsplit("/", 1)[-1]
        if not commits:
            return None
        lines = [f"- {c.get('message', '').splitlines()[0][:100]}" for c in commits[:5]]
        more = f"\n- and {len(commits) - 5} more" if len(commits) > 5 else ""
        return f"**{who}** pushed {len(commits)} commit{'s' if len(commits) != 1 else ''} to {repo} ({branch})\n" + "\n".join(lines) + more
    if kind == "issues" and body.get("action") in ("opened", "closed", "reopened"):
        issue = body.get("issue") or {}
        return f"**{who}** {body['action']} issue #{issue.get('number')} in {repo}: {issue.get('title', '')}\n{issue.get('html_url', '')}"
    if kind == "pull_request" and body.get("action") in ("opened", "closed", "reopened"):
        pr = body.get("pull_request") or {}
        did = "merged" if body["action"] == "closed" and pr.get("merged") else body["action"]
        return f"**{who}** {did} pull request #{pr.get('number')} in {repo}: {pr.get('title', '')}\n{pr.get('html_url', '')}"
    if kind == "release" and body.get("action") == "published":
        release = body.get("release") or {}
        return f"**{repo}** released {release.get('tag_name', '')}\n{release.get('html_url', '')}"
    if kind == "ping":
        return f"GitHub can reach me now, for {repo}."
    return None


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        raw = self.rfile.read(min(int(self.headers.get("Content-Length") or 0), 1024 * 1024))
        try:
            body = json.loads(raw or b"{}")
        except ValueError:
            return self.answer(400, "not JSON")
        if self.path == "/say":
            if args.secret and not hmac.compare_digest(self.headers.get("X-Corded-Secret", ""), args.secret):
                return self.answer(403, "wrong secret")
            text = str(body.get("text", "")).strip()
        elif self.path == "/github":
            if args.secret:
                expected = "sha256=" + hmac.new(args.secret.encode(), raw, hashlib.sha256).hexdigest()
                if not hmac.compare_digest(self.headers.get("X-Hub-Signature-256", ""), expected):
                    return self.answer(403, "bad signature")
            text = github_sentence(self.headers.get("X-GitHub-Event", ""), body) or ""
        else:
            return self.answer(404, "unknown address")
        if text:
            try:
                bot.say(args.channel, text[:4000])
            except Exception as error:  # noqa: BLE001
                return self.answer(502, f"could not post: {error}")
        self.answer(200, "ok")

    def answer(self, code, text):
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(text.encode())

    def log_message(self, *_):
        pass


if __name__ == "__main__":
    bot.connect(args.address)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"{args.username} is connected; listening on http://127.0.0.1:{args.port}", flush=True)
    bot.run()
