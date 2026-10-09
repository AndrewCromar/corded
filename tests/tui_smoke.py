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

        alice = start("alice")
        bob = start("bob")
        print("ok  both clients created vaults and went live")

        alice.type("/chat bob\r")
        alice.expect("Say hello")
        alice.type("hello from alice\r")
        bob.expect("hello from alice")
        print("ok  alice -> bob")

        bob.type("hi back from bob\r")
        alice.expect("hi back from bob")
        print("ok  bob -> alice")

        alice.type("/reply replying to you\r")
        bob.expect("replying to you")
        bob.expect("> bob: hi back from bob")
        print("ok  reply shows the quoted message")

        bob.type("/react +1\r")
        alice.expect("[+1]")
        print("ok  reaction shows on the other side")

        bob.type("/thread threaded answer\r")
        alice.expect("   | ")
        alice.expect("threaded answer")
        alice.type("/thread and another\r")
        bob.expect("and another")
        print("ok  thread replies are drawn under the message that started them")

        bob.type("this has a mistaek\r")
        alice.expect("this has a mistaek")
        bob.type("/edit this has no mistake\r")
        alice.expect("this has no mistake (edited)")
        bob.type("/delete\r")
        alice.expect("[deleted]")
        print("ok  edit and delete show on the other side")

        alice.type("/verify\r")
        alice.expect("Safety numbers")
        alice.type("/verified bob\r")
        alice.expect("(checked)")
        print("ok  safety numbers shown and a contact marked as checked")

        carol = start("carol")
        alice.type("/group bob carol : Weekend plans\r")
        alice.expect("Weekend plans")
        alice.type("hi group\r")
        carol.expect("Weekend plans")
        carol.expect("hi group")
        bob.expect("Weekend plans (")  # shown as unread in bob's chat list
        carol.type("carol is here\r")
        alice.expect("carol is here")
        print("ok  group chat with three people, named room, unread marker")

        carol.type("/leave\r")
        carol.expect("No conversations yet")
        alice.expect("left the chat")
        print("ok  leaving a group removes it for the leaver and tells the others")

        # Restart alice: unlock the existing vault and see the history again.
        alice.type("/quit\r")
        os.waitpid(alice.pid, 0)
        clients.remove(alice)
        bob.type("sent while alice was away\r")
        alice = Tui(f"{bindir}/corded-tui", ["--vault", f"{tmp}/alice"])
        clients.append(alice)
        alice.expect("Unlock your vault")
        alice.type("wrong passphrase\r")
        alice.expect("wrong passphrase", timeout=30)
        alice.type("a long passphrase\r")
        alice.expect("Weekend plans", timeout=30)
        # The missed message shows in the open chat, or as an unread count if
        # the other chat happens to be the open one.
        alice.expect_any(["sent while alice was away", "bob (1)"], timeout=30)
        print("ok  restart: wrong passphrase refused, history restored, missed message delivered")
        print("PASS")
    finally:
        try:
            for c in clients:
                c.close()
        finally:
            server.terminate()
            server.wait()
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
