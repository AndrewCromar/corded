#!/usr/bin/env python3
"""Runs a Corded server and everything around it with one command, and shows
it all on one page.

    python3 corded_host.py [--config host.json] [--port 7460] [--take-over]

What it runs is listed in host.json (see host.example.json): the server, the
model programs the bots need, and the bots. Each is started in order, kept
running (one that stops by itself is started again), and stopped cleanly on
Ctrl+C, bots first and the server last.

The page is served on this machine only. The address it prints carries a key
made at start; without it nothing is shown and no button works, so no other
program or web page on the machine can press them.

Only the Python standard library is used. Linux only: it reads /proc.
"""
import argparse
import json
import os
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
TICKS = os.sysconf("SC_CLK_TCK")
PAGE = os.sysconf("SC_PAGE_SIZE")


def stamp():
    return time.strftime("%H:%M:%S")


def say(*words):
    print(f"[{stamp()}]", *words, flush=True)


def port_open(port, host="127.0.0.1"):
    with socket.socket() as s:
        s.settimeout(0.3)
        return s.connect_ex((host, int(port))) == 0


def processes():
    """Every process of this user: pid -> (process group, command line, ticks used, pages held)."""
    found = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open(f"/proc/{name}/stat") as f:
                stat = f.read()
            fields = stat[stat.rindex(")") + 2:].split()
            with open(f"/proc/{name}/cmdline", "rb") as f:
                command = f.read().replace(b"\0", b" ").decode(errors="replace").strip()
            with open(f"/proc/{name}/statm") as f:
                pages = int(f.read().split()[1])
            if os.stat(f"/proc/{name}").st_uid != os.getuid():
                continue
            found[int(name)] = (int(fields[2]), command, int(fields[11]) + int(fields[12]), pages)
        except (OSError, ValueError, IndexError):
            continue
    return found


class Unit:
    """One program the host looks after."""

    def __init__(self, spec, log_dir):
        self.name = spec["name"]
        self.kind = spec.get("kind", "other")          # server, model, bot or other
        self.cmd = [os.path.expanduser(str(part)) for part in spec["cmd"]]
        self.cwd = os.path.expanduser(spec["cwd"]) if spec.get("cwd") else None
        self.env = {k: os.path.expanduser(str(v)) for k, v in (spec.get("env") or {}).items()}
        self.autostart = spec.get("autostart", True)
        self.after = spec.get("after", [])
        self.port = spec.get("port")                   # it is ready when this answers
        self.vault = os.path.expanduser(spec["vault"]) if spec.get("vault") else None
        self.memory_max = spec.get("memory_max")       # e.g. "6G": more than that and it alone is stopped
        self.about = spec.get("about", "")
        self.log_path = os.path.join(log_dir, self.name + ".log")
        self.proc = None
        self.wanted = False
        self.state = "stopped"                         # stopped, starting, running, crashed, outside
        self.started = 0.0
        self.restarts = 0
        self.quick = 0                                 # stops in a row soon after starting
        self.last_exit = None
        self.outside_pid = None
        self.tail = []
        self.lock = threading.Lock()
        self.cpu = 0.0
        self.rss = 0
        self._ticks = (0, 0.0)

    # ---- running it ----

    def spawn(self):
        cmd = list(self.cmd)
        if self.memory_max and shutil.which("systemd-run"):
            cmd = ["systemd-run", "--user", "--scope", "-q", "-p", f"MemoryMax={self.memory_max}",
                   "-p", "MemorySwapMax=1G"] + cmd
        env = dict(os.environ, PYTHONUNBUFFERED="1", **self.env)
        log = open(self.log_path, "ab", buffering=0)
        log.write(f"\n--- started {time.strftime('%Y-%m-%d %H:%M:%S')}: {' '.join(self.cmd)}\n".encode())
        self.proc = subprocess.Popen(cmd, cwd=self.cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
        self.started = time.time()
        self.state = "starting" if self.port else "running"
        threading.Thread(target=self._read, args=(self.proc, log), daemon=True).start()

    def _read(self, proc, log):
        for raw in proc.stdout:
            log.write(raw)
            line = raw.decode(errors="replace").rstrip()
            if line:
                with self.lock:
                    self.tail.append(f"{stamp()}  {line}")
                    del self.tail[:-200]
        log.close()

    def stop(self, grace=10):
        proc = self.proc
        if not proc or proc.poll() is not None:
            return
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        try:
            proc.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait()

    # ---- what it is doing ----

    def find_outside(self, running):
        """A copy of this program that something else started: the pid, or None."""
        mine = self.proc.pid if self.proc and self.proc.poll() is None else None
        # A bot is known by its vault folder, which no two can share; anything
        # else by its whole command, so that another server on another port
        # is not taken for this one.
        whole = " ".join(self.cmd)
        for pid, (group, command, _, _) in running.items():
            if pid == os.getpid() or group == mine or "corded_host.py" in command:
                continue
            if command.startswith(("/usr/bin/bash", "bash ", "sh ", "systemd-run")):
                continue
            if (self.vault and self.vault in command.split(" ")) or (not self.vault and command.endswith(whole)):
                return pid
        # Or whatever of this user's answers on its port, started with other words.
        if self.port and not mine and shutil.which("ss"):
            try:
                listening = subprocess.run(["ss", "-H", "-ltnp", f"sport = :{int(self.port)}"],
                                           capture_output=True, text=True, timeout=5).stdout
                found = listening.split("pid=")[1].split(",")[0] if "pid=" in listening else ""
                if found.isdigit() and int(found) in running:
                    return int(found)
            except (OSError, subprocess.TimeoutExpired, IndexError):
                pass
        return None

    def measure(self, running):
        if not self.proc or self.proc.poll() is not None:
            self.cpu, self.rss = 0.0, 0
            return
        ticks = pages = 0
        for pid, (group, _, used, held) in running.items():
            if group == self.proc.pid:
                ticks += used
                pages += held
        now = time.time()
        before, when = self._ticks
        if when and now > when and ticks >= before:
            self.cpu = (ticks - before) / TICKS / (now - when) * 100
        self._ticks = (ticks, now)
        self.rss = pages * PAGE

    def enabled(self):
        """A bot's own switch (!name off): True, False, or None when it has none."""
        if not self.vault:
            return None
        try:
            with open(os.path.join(self.vault, "store.json"), encoding="utf-8") as f:
                return bool(json.load(f).get("enabled", True))
        except (OSError, ValueError):
            return True

    def view(self):
        with self.lock:
            tail = self.tail[-6:]
        alive = self.proc is not None and self.proc.poll() is None
        return {"name": self.name, "kind": self.kind, "about": self.about, "state": self.state,
                "pid": self.proc.pid if alive else self.outside_pid, "up": int(time.time() - self.started) if alive else 0,
                "restarts": self.restarts, "last_exit": self.last_exit, "cpu": round(self.cpu, 1) if alive else 0.0,
                "rss": self.rss if alive else 0,
                "port": self.port, "enabled": self.enabled() if self.kind == "bot" else None, "tail": tail,
                "command": " ".join(self.cmd)}


class Host:
    def __init__(self, config, config_path):
        self.config_path = config_path
        self.log_dir = os.path.expanduser(config.get("logs", "~/corded-host/logs"))
        os.makedirs(self.log_dir, exist_ok=True)
        self.units = [Unit(spec, self.log_dir) for spec in config["units"]]
        self.by_name = {u.name: u for u in self.units}
        self.key = secrets.token_urlsafe(18)
        self.stopping = False
        self.machine = {}
        self.server_data = os.path.expanduser(config.get("server_data", ""))
        self._data_size = (0, 0.0)

    def order(self):
        """Units so that each comes after the ones it waits for."""
        done, out = set(), []
        while len(out) < len(self.units):
            moved = False
            for unit in self.units:
                if unit.name not in done and all(a in done or a not in self.by_name for a in unit.after):
                    out.append(unit)
                    done.add(unit.name)
                    moved = True
            if not moved:
                raise SystemExit("host.json: units wait for each other in a circle")
        return out

    # ---- starting and stopping ----

    def start(self, unit, by_hand=False):
        if unit.proc and unit.proc.poll() is None:
            return "already running"
        outside = unit.find_outside(processes())
        if outside or (unit.port and port_open(unit.port)):
            unit.state, unit.outside_pid = "outside", outside
            return "a copy started outside the host is running; stop that one first"
        for name in unit.after:
            other = self.by_name.get(name)
            if other and other.port:
                for _ in range(100):
                    if port_open(other.port) or self.stopping:
                        break
                    time.sleep(0.3)
        unit.wanted = True
        if by_hand:
            unit.restarts = 0
        unit.outside_pid = None
        try:
            unit.spawn()
        except OSError as error:
            unit.state, unit.last_exit, unit.wanted = "crashed", str(error), False
            say(f"{unit.name}: could not be started: {error}")
            return str(error)
        say(f"{unit.name}: started (pid {unit.proc.pid})")
        return "started"

    def stop(self, unit):
        unit.wanted = False
        if unit.state == "outside":
            return "it was started outside the host; stop it where it was started"
        unit.stop()
        unit.state = "stopped"
        say(f"{unit.name}: stopped")
        return "stopped"

    def take_over(self):
        """Ends copies that were started outside the host, so that the host's own can start."""
        running = processes()
        for unit in self.units:
            pid = unit.find_outside(running)
            if pid:
                say(f"{unit.name}: ending the copy started outside the host (pid {pid})")
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        end = time.time() + 15
        while time.time() < end and any(u.find_outside(processes()) for u in self.units):
            time.sleep(0.5)

    def watch(self):
        """Keeps what should run running, and measures it."""
        backoff = {}
        while not self.stopping:
            running = processes()
            for unit in self.units:
                proc = unit.proc
                if proc and proc.poll() is None:
                    if unit.state == "starting" and port_open(unit.port):
                        unit.state = "running"
                        say(f"{unit.name}: ready")
                    unit.measure(running)
                elif proc and unit.wanted and unit.state in ("running", "starting"):
                    unit.last_exit = f"stopped by itself (code {proc.returncode}) at {stamp()}"
                    unit.state = "crashed"
                    # One that ran a good while before stopping starts the count of quick failures afresh.
                    unit.quick = 0 if time.time() - unit.started > 60 else unit.quick + 1
                    unit.restarts += 1
                    wait = min(60, 2 * 2 ** min(unit.quick, 5))
                    backoff[unit.name] = time.time() + wait
                    say(f"{unit.name}: {unit.last_exit}; starting it again in {wait}s")
                elif unit.state == "crashed" and unit.wanted and time.time() >= backoff.get(unit.name, 0):
                    self.start(unit)
                elif unit.state == "outside":
                    pid = unit.find_outside(running)
                    if not pid and not (unit.port and port_open(unit.port)):
                        unit.state, unit.outside_pid = "stopped", None
                    else:
                        unit.outside_pid = pid
            self.machine = self.measure_machine()
            time.sleep(2)

    def measure_machine(self):
        out = {"time": stamp(), "load": os.getloadavg()[0], "cores": os.cpu_count()}
        try:
            with open("/proc/meminfo") as f:
                info = {line.split(":")[0]: int(line.split()[1]) * 1024 for line in f}
            out["memory"] = {"total": info["MemTotal"], "used": info["MemTotal"] - info["MemAvailable"]}
        except (OSError, KeyError, ValueError):
            pass
        if shutil.which("nvidia-smi"):
            try:
                row = subprocess.run(["nvidia-smi", "--query-gpu=name,memory.used,memory.total,utilization.gpu,temperature.gpu",
                                      "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=5).stdout
                name, used, total, busy, heat = [part.strip() for part in row.strip().splitlines()[0].split(",")]
                apps = subprocess.run(["nvidia-smi", "--query-compute-apps=pid,used_memory", "--format=csv,noheader,nounits"],
                                      capture_output=True, text=True, timeout=5).stdout
                holders = []
                running = processes()
                for line in apps.strip().splitlines():
                    pid, held = [part.strip() for part in line.split(",")]
                    group = running.get(int(pid), (None,))[0]
                    owner = next((u.name for u in self.units if u.proc and u.proc.poll() is None
                                  and group == u.proc.pid), None)
                    holders.append({"who": owner or os.path.basename(running.get(int(pid), (0, "another program"))[1].split(" ")[0]),
                                    "bytes": int(held) * 2 ** 20})
                out["card"] = {"name": name, "used": int(used) * 2 ** 20, "total": int(total) * 2 ** 20,
                               "busy": int(busy), "heat": int(heat), "holders": holders}
            except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
                pass
        if self.server_data and os.path.isdir(self.server_data):
            size, when = self._data_size
            if time.time() - when > 30:
                size = sum(os.path.getsize(os.path.join(d, f)) for d, _, files in os.walk(self.server_data) for f in files
                           if os.path.exists(os.path.join(d, f)))
                self._data_size = (size, time.time())
            disk = shutil.disk_usage(self.server_data)
            out["storage"] = {"server": size, "disk_free": disk.free, "disk_total": disk.total}
            out["storage"].update(self.server_files())
        return out

    def server_files(self):
        """What members have sent as files, against the allowance the owner
        set in the server's settings (storage_limit_mb; 0 is none). Read from
        the server's own database, without writing to it."""
        import sqlite3
        try:
            db = sqlite3.connect(f"file:{os.path.join(self.server_data, 'cordedd.db')}?mode=ro", uri=True, timeout=2)
            try:
                count, held = db.execute("SELECT COUNT(*), COALESCE(SUM(total), 0) FROM blobs").fetchone()
                row = db.execute("SELECT value FROM server_info WHERE key = 'set:storage_limit_mb'").fetchone()
                members = db.execute("SELECT COUNT(*) FROM users").fetchone()[0]
            finally:
                db.close()
            value = row[0] if row else b"0"
            value = value.decode(errors="replace") if isinstance(value, bytes) else str(value)
            limit = int(value) * 2 ** 20 if value.strip().isdigit() else 0
            return {"files": count, "files_bytes": held, "files_limit": limit, "members": members}
        except Exception:  # noqa: BLE001  (an older server, or one that is busy: the figure is left out)
            return {}

    def shutdown(self):
        self.stopping = True
        for unit in reversed(self.order()):
            if unit.proc and unit.proc.poll() is None:
                say(f"{unit.name}: stopping")
                unit.wanted = False
                unit.stop()

    # ---- what the page asks ----

    def act(self, name, action):
        unit = self.by_name.get(name)
        if not unit:
            return "no such program"
        if action == "start":
            return self.start(unit, by_hand=True)
        if action == "stop":
            return self.stop(unit)
        if action == "restart":
            self.stop(unit)
            return self.start(unit, by_hand=True)
        if action in ("on", "off") and unit.vault:
            with open(os.path.join(unit.vault, "power"), "w", encoding="utf-8") as f:
                f.write(action)
            return f"asked to turn {action}"
        return "that cannot be done to it"


def handler_for(host):
    class Handler(BaseHTTPRequestHandler):
        def send(self, code, body, kind="application/json"):
            raw = body if isinstance(body, bytes) else (json.dumps(body) if kind == "application/json" else body).encode()
            self.send_response(code)
            self.send_header("Content-Type", kind + "; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(raw)

        def allowed(self, url):
            given = self.headers.get("X-Key") or urllib.parse.parse_qs(url.query).get("key", [""])[0]
            return secrets.compare_digest(given, host.key)

        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            if not self.allowed(url):
                self.send(403, "This page needs the key that corded_host.py printed when it started.", "text/plain")
                return
            if url.path == "/":
                with open(os.path.join(HERE, "dashboard.html"), "rb") as f:
                    self.send(200, f.read(), "text/html")
            elif url.path == "/api/state":
                self.send(200, {"units": [u.view() for u in host.order()], "machine": host.machine,
                                "config": host.config_path})
            elif url.path.startswith("/api/log/"):
                unit = host.by_name.get(urllib.parse.unquote(url.path[len("/api/log/"):]))
                if not unit:
                    self.send(404, "no such program", "text/plain")
                    return
                with unit.lock:
                    self.send(200, "\n".join(unit.tail) or "Nothing printed yet.", "text/plain")
            else:
                self.send(404, "not here", "text/plain")

        def do_POST(self):
            url = urllib.parse.urlparse(self.path)
            parts = url.path.strip("/").split("/")
            if not self.allowed(url) or self.headers.get("X-Key") != host.key:
                self.send(403, {"error": "no key"})
                return
            if len(parts) == 4 and parts[:2] == ["api", "unit"]:
                # Not in this thread: starting may wait for another program to be ready.
                result = {}
                worker = threading.Thread(target=lambda: result.update(said=host.act(urllib.parse.unquote(parts[2]), parts[3])))
                worker.start()
                worker.join(timeout=20)
                self.send(200, {"said": result.get("said", "working on it")})
            else:
                self.send(404, {"error": "not here"})

        def log_message(self, *_):
            pass
    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=os.path.join(HERE, "host.json"))
    parser.add_argument("--port", type=int, default=None, help="where the page is served (7460 unless host.json says)")
    parser.add_argument("--take-over", action="store_true",
                        help="end copies of these programs that were started outside the host, then start its own")
    args = parser.parse_args()
    if not os.path.exists(args.config):
        raise SystemExit(f"{args.config} is missing. Copy host.example.json to host.json and change the paths in it.")
    with open(args.config, encoding="utf-8") as f:
        config = json.load(f)
    host = Host(config, os.path.abspath(args.config))
    port = args.port or config.get("port", 7460)
    web = ThreadingHTTPServer(("127.0.0.1", port), handler_for(host))
    threading.Thread(target=web.serve_forever, daemon=True).start()
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    say(f"dashboard: http://127.0.0.1:{port}/?key={host.key}")
    try:
        if args.take_over:
            host.take_over()
        threading.Thread(target=host.watch, daemon=True).start()
        for unit in host.order():
            if unit.autostart:
                said = host.start(unit)
                if said != "started":
                    say(f"{unit.name}: {said}")
        say("everything is started. Ctrl+C stops it all.")
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        print()
        say("stopping everything")
    finally:
        host.shutdown()
        say("all stopped")


if __name__ == "__main__":
    main()
