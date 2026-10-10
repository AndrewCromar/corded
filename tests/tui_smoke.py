#!/usr/bin/env python3
"""Smoke test for corded-tui: runs a real server and two TUI clients in
pseudo-terminals, types into them, and checks what appears on screen.

    python3 tests/tui_smoke.py build/dev/bin
"""
import fcntl
import os
import pty
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[()][0-9A-B]|\x1b[=>]")


class Tui:
    def __init__(self, binary, args):
        self.args = args
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["TERM"] = "xterm-256color"
            os.execv(binary, [binary] + args)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
        self.screen = ""

    def pump(self, seconds=0.2):
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if not ready:
                continue
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                return
            self.screen += ANSI.sub("", data.decode("utf-8", "replace"))

    def type(self, text):
        os.write(self.fd, text.encode())
        self.pump(0.3)

    def expect(self, needle, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.screen:
                return
            self.pump(0.2)
        tail = self.screen[-1500:]
        raise AssertionError(f"did not see {needle!r} on screen. Last output:\n{tail}")

    def expect_any(self, needles, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            if any(n in self.screen for n in needles):
                return
            self.pump(0.2)
        raise AssertionError(f"did not see any of {needles!r}. Last output:\n{self.screen[-1500:]}")

    def clear(self):
        self.screen = ""

    def close(self):
        try:
            os.kill(self.pid, signal.SIGTERM)
            for _ in range(50):
                if os.waitpid(self.pid, os.WNOHANG)[0] != 0:
                    return
                # Keep reading: a client blocks if nobody drains its terminal.
                self.pump(0.1)
            info = []
            for th in os.listdir(f"/proc/{self.pid}/task"):
                st = open(f"/proc/{self.pid}/task/{th}/stat").read().split()
                sc = open(f"/proc/{self.pid}/task/{th}/syscall").read().split()[0]
                info.append(f"{th}:{st[2]}:syscall{sc}")
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
            raise AssertionError(f"client {self.args} did not exit on SIGTERM; threads {info}; "
                                 f"screen tail {self.screen[-300:]!r}")
        except OSError:
            pass


def main():
    bindir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/dev/bin")
    tmp = tempfile.mkdtemp(prefix="corded-tui-")
    with socket.socket() as probe:  # ask the kernel for a free port
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    server = subprocess.Popen(
        [f"{bindir}/cordedd", "--host", "127.0.0.1", "--port", str(port), "--data", f"{tmp}/server"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    clients = []
    extra = []  # further server processes started along the way
    DOWN = "\x1b[B"
    try:
        def start(name):
            t = Tui(f"{bindir}/corded-tui",
                    ["--vault", f"{tmp}/{name}", "--server", f"127.0.0.1:{port}", "--name", name])
            clients.append(t)
            t.expect("Create your identity")
            # With --name given, focus starts on the passphrase field.
            t.type("a long passphrase")
            t.type(DOWN)
            t.type("a long passphrase")
            t.type("\r")
            t.expect("live", timeout=30)
            return t

        # The first person to register owns the server.
        alice = start("alice")
        bob = start("bob")
        alice.expect("#general")
        alice.expect("owner")
        bob.expect("#general")
        print("ok  both clients joined the server and see #general; the first one owns it")

        alice.type("hi everyone\r")
        bob.expect("hi everyone")
        print("ok  channel message reaches the other member")

        alice.type("/chat bob\r")
        alice.expect("Say hello")
        alice.type("hello from alice\r")
        bob.expect("alice (1)")
        bob.type("/open alice\r")
        bob.expect("hello from alice")
        print("ok  direct message: unread marker, then /open shows it")

        bob.type("hi back from bob\r")
        alice.expect("hi back from bob")
        alice.type("/reply replying to you\r")
        bob.expect("replying to you")
        bob.expect("> bob: hi back from bob")
        bob.type("/react +1\r")
        alice.expect("[+1]")
        print("ok  reply and reaction")

        # Acting on a chosen message by its number, not just the latest.
        bob.type("/react 1 star\r")
        alice.expect("[star]")
        bob.type("/reply 1 answering the very first message\r")
        alice.expect("answering the very first message")
        alice.expect("> alice: hello from alice")
        bob.type("/edit 1 not mine\r")
        bob.expect("you can only edit your own messages")
        print("ok  commands take a message number")

        # Search looks through what this device holds.
        bob.type("/search very first\r")
        bob.expect("Found for")
        bob.expect("answering the very first message")
        bob.type("\r")
        print("ok  search")

        # /notify is remembered and says what it does.
        bob.type("/notify bell\r")
        bob.expect("ring the terminal's bell")
        bob.type("/notify\r")
        bob.expect("now: bell")
        bob.type("/notify off\r")
        print("ok  notification setting")

        # Pins: in a direct chat either person pins; both see the mark and the list.
        alice.type("/pin 1\r")
        bob.expect("(pinned)", timeout=20)
        bob.type("/pins\r")
        bob.expect("Pinned messages")
        bob.type("\r")   # closes the box
        print("ok  pinned messages")

        # The same reaction again takes it back.
        bob.type("/react 1 star\r")
        alice.pump(2.0)
        alice.clear()
        alice.type("/help\r")   # forces a redraw of the whole screen
        alice.type("/help\r")
        alice.pump(1.5)
        assert "[+1]" in alice.screen, "the other reaction should still be there"
        assert "[star]" not in alice.screen, "a reaction was not taken back"
        print("ok  reacting again takes the reaction back")

        bob.type("/thread threaded answer\r")
        alice.expect("   | ")
        alice.expect("threaded answer")
        print("ok  thread replies are drawn under the message that started them")

        bob.type("this has a mistaek\r")
        alice.expect("this has a mistaek")
        bob.type("/edit this has no mistake\r")
        alice.expect("this has no mistake (edited)")
        bob.type("/delete\r")
        alice.expect("[deleted]")
        print("ok  edit and delete show on the other side")

        alice.type("/once 2s this one vanishes\r")
        bob.expect("this one vanishes (disappears)")
        time.sleep(3.5)
        bob.clear()
        bob.type("/help\r")
        bob.pump(1.0)
        assert "hi back from bob" in bob.screen, "bob's screen did not redraw"
        assert "this one vanishes" not in bob.screen, "a disappearing message is still on screen"
        bob.type("/help\r")
        alice.type("/disappear 1h\r")
        bob.expect("messages disappear after 1h")
        alice.type("/disappear off\r")
        bob.expect("turned off disappearing messages")
        print("ok  disappearing messages: one message, and a whole chat")

        alice.type("/verify\r")
        alice.expect("Safety numbers")
        alice.type("/verified bob\r")
        alice.expect("(checked)")
        print("ok  safety numbers shown and a contact marked as checked")

        carol = start("carol")
        alice.type("/group bob carol : Weekend plans\r")
        alice.expect("Weekend plans")
        alice.type("hi group\r")
        carol.expect("Weekend plans (")
        carol.type("/open weekend\r")
        carol.expect("hi group")
        carol.type("carol is here\r")
        alice.expect("carol is here")
        carol.type("/leave\r")
        alice.expect("left the chat")
        print("ok  group chat: named, unread marker, and leaving")

        # Running the community.
        alice.type("/channel new dev\r")
        bob.expect("#dev")
        carol.expect("#dev")
        alice.type("/open dev\r")
        alice.type("first post in dev\r")
        bob.type("/open dev\r")
        bob.expect("first post in dev")
        # NSFW: the owner marks the channel; others must choose to see it.
        alice.type("/channel nsfw on\r")
        bob.expect("#dev [NSFW]", timeout=20)
        bob.type("/open general\r")
        bob.type("/open dev\r")
        bob.expect("is marked NSFW")
        bob.type("/show\r")
        bob.expect("first post in dev")
        alice.type("/channel nsfw off\r")
        alice.type("/open dev\r")
        print("ok  NSFW channel: tagged, covered until opened on purpose")
        alice.type("/role new mods kick_members manage_messages\r")
        alice.type("/role give bob mods\r")
        alice.type("/members\r")
        alice.expect("[mods]")
        print("ok  owner creates a channel and a role, and gives the role to a member")

        # The Members page, and display names.
        bob.type("/nick Bobby Tables\r")
        alice.type("\t")                 # focus the chat list
        for _ in range(12):
            alice.type("\x1b[B")         # down to the last row, the Members page
        alice.expect("Members of ")
        alice.expect("Bobby Tables")
        alice.expect("(bob)")
        alice.expect("/remove-account <user>")
        alice.type("\t")                 # back to typing
        # A person's own name wins; the owner's choice shows once they have none.
        bob.type("/profile bio likes tables\r")
        bob.type("/nick\r")
        alice.type("/setnick bob Robert\r")
        alice.expect("Robert")
        alice.type("/profile bob\r")
        alice.expect("likes tables")
        alice.type("\r")
        alice.type("/setnick bob\r")
        alice.type("/open dev\r")
        alice.expect("first post in dev")
        # Presence shows on the members page.
        bob.type("/presence dnd\r")
        alice.type("/members\r")
        alice.expect("do not disturb", timeout=20)
        bob.type("/presence auto\r")
        print("ok  members page lists everyone; display names can be set and changed by the owner; presence shows")

        # Running the server from the client.
        alice.type("/settings\r")
        alice.expect("Server settings")
        alice.expect("retention_days = ")
        alice.type("/set retention_days 14\r")
        alice.expect("retention_days is now 14")
        alice.expect("status: version")
        bob.type("/set registration closed\r")
        bob.expect("do not have permission")
        print("ok  settings and status from the owner's client; refused for others")

        # An invite link, used by a fourth person to join.
        alice.type("/invite 1\r")
        alice.expect("Invite link")
        link = open(f"{tmp}/alice/last-invite.txt").read().strip()
        assert link.startswith("corded://127.0.0.1:"), link
        dave = Tui(f"{bindir}/corded-tui", ["--vault", f"{tmp}/dave", "--name", "dave", "--join", link])
        clients.append(dave)
        dave.expect("Create your identity")
        dave.type("a long passphrase")
        dave.type(DOWN)
        dave.type("a long passphrase")
        dave.type("\r")
        dave.expect("live", timeout=30)
        dave.expect("#general")
        print("ok  invite link made in the client lets a new person join")

        # Bob sets up a second device with his recovery key and is the same person there.
        bob.type("/recovery-key\r")
        bob.expect("Your recovery key")
        import re as _re
        key = _re.search(r"((?:[0-9A-Z]{5}-){10}[0-9A-Z]{5})", bob.screen).group(1)
        bob.type("/help\r")
        bob.type("/help\r")
        bob2 = Tui(f"{bindir}/corded-tui", ["--vault", f"{tmp}/bob-second", "--name", "bob",
                                             "--server", f"127.0.0.1:{port}", "--recovery-key", key])
        clients.append(bob2)
        bob2.expect("Setting up this device with your recovery key")
        bob2.type("another passphrase")
        bob2.type(DOWN)
        bob2.type("another passphrase")
        bob2.type("\r")
        bob2.expect("live", timeout=30)
        bob2.expect("#dev")
        bob2.expect("[mods]" if False else "#general")
        alice.type("/open general\r")
        alice.type("to both of bob's devices\r")
        bob2.type("/open general\r")
        bob2.expect("to both of bob's devices")
        bob.type("/open general\r")
        bob.expect("to both of bob's devices")
        bob.type("/open dev\r")
        print("ok  a second device set up with a recovery key is the same person")

        # A second server: alice joins it from inside the client and switches between the two.
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port2 = probe.getsockname()[1]
        server2 = subprocess.Popen(
            [f"{bindir}/cordedd", "--host", "127.0.0.1", "--port", str(port2), "--data", f"{tmp}/server2",
             "--name", "SecondServer"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        extra.append(server2)
        time.sleep(0.5)
        alice.type(f"/server join 127.0.0.1:{port2}\r")
        alice.expect("SecondServer", timeout=30)
        alice.type("hello second server\r")
        alice.type("/servers\r")
        alice.expect("Servers")
        alice.expect("SecondServer")
        alice.type("/server switch 1\r")
        alice.clear()
        alice.type("/help\r")
        alice.pump(1.0)
        if "#dev" not in alice.screen:
            print("DEBUG alice screen:\n" + alice.screen[-2500:])
        assert "#dev" in alice.screen, "switching back did not show the first server's channels"
        assert "hello second server" not in alice.screen, "a message from the other server is showing"
        alice.type("/help\r")
        alice.type("/open dev\r")
        print("ok  one client in two servers: join, list and switch")

        alice.type("/channel private mods\r")
        time.sleep(1.5)
        carol.pump(0.5)
        carol.clear()
        carol.type("/help\r")   # forces a redraw of the whole screen
        carol.pump(1.0)
        assert "#general" in carol.screen, "carol's screen did not redraw"
        assert "#dev" not in carol.screen, "a private channel is still visible to a non-member"
        bob.clear()
        bob.type("/help\r")
        bob.pump(1.0)
        assert "#dev" in bob.screen, "the role holder lost the private channel"
        carol.type("/channel new nope\r")
        carol.expect("do not have permission")
        print("ok  private channel hidden from others; ordinary members cannot manage")

        # Restart alice: unlock the existing vault and see everything again.
        alice.type("/quit\r")
        os.waitpid(alice.pid, 0)
        clients.remove(alice)
        bob.type("/open alice\r")
        bob.type("sent while alice was away\r")
        alice = Tui(f"{bindir}/corded-tui", ["--vault", f"{tmp}/alice"])
        clients.append(alice)
        alice.expect("Unlock your vault")
        alice.type("wrong passphrase\r")
        alice.expect("wrong passphrase", timeout=30)
        alice.type("a long passphrase\r")
        alice.expect("#general", timeout=30)
        alice.expect("owner", timeout=30)
        alice.expect_any(["sent while alice was away", "bob (1)"], timeout=30)
        print("ok  restart: wrong passphrase refused, channels and ownership restored, missed message delivered")

        # Read markers and typing notices between the two of them.
        bob.type("/help\r")   # close the help left open above
        alice.type("/open bob\r")
        bob.expect("read by alice", timeout=30)
        alice.type("hel")
        bob.expect("alice is typing", timeout=15)
        alice.type("lo back\r")
        bob.expect("hello back", timeout=15)
        print("ok  read marker under the last message read, typing notice while composing")

        # The settings page: everyone sees their own, the owner also sees the server's.
        bob.type("/settings\r")
        bob.expect("tell others what you have read", timeout=15)
        alice.type("/settings\r")
        alice.expect("Server settings", timeout=15)
        alice.expect("status: version", timeout=15)
        print("ok  settings page: personal settings for all, server settings and status for the owner")
        print("PASS")
    finally:
        try:
            for c in clients:
                c.close()
        finally:
            for proc in extra + [server]:
                proc.terminate()
                proc.wait()
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
