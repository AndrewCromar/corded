# The Corded host

One command that runs a Corded server and everything around it, and one page
that shows it all.

```sh
cp host.example.json host.json     # then change the paths in it
python3 corded_host.py
```

It starts what `host.json` lists, in order: the server, the model programs the
bots need, then the bots. Each is kept running: one that stops by itself is
started again, after 2 seconds the first time and longer each time it happens
again. Ctrl+C stops everything, bots first and the server last. Only the
Python standard library is used; it needs Linux (it reads `/proc`).

## The page

At start it prints an address like `http://127.0.0.1:7460/?key=…`. Open it in
a browser on the same machine. It shows, refreshed every two seconds:

- the machine: the graphics card (how full, and which program holds what), the
  memory, the processor and the disk, with the size of the server's own data;
- every program: a light (green running, amber starting, red stopped by
  itself, blue started outside the host, grey stopped), how long it has been
  up, its memory and processor use, how often it had to be started again, and
  the last lines it printed. Click those lines for more;
- buttons for each: **Start**, **Stop**, **Restart**, and for a bot **Turn
  off** / **Turn on**. Turned off, a bot stays connected but shows offline and
  does nothing, exactly as after `!name off` in the chat; it costs almost
  nothing and wakes at once. Stop ends its program.

The page is served to this machine only, and the key in the address is made
anew at each start. Without the key nothing is shown and no button works, so
another program or a web page open in your browser cannot press them.

## host.json

Each program is one entry in `units`:

| Field | Meaning |
|---|---|
| `name` | What the page calls it |
| `kind` | `server`, `model`, `bot` or `other`: where it is listed |
| `cmd` | The command, as a list. `~` means your home folder |
| `cwd`, `env` | The folder it runs in, and extra environment variables |
| `after` | Names of programs that are started first; if one has a `port`, this one waits until it answers |
| `port` | The port it listens on. It counts as ready when that answers |
| `vault` | For a bot: its folder. Needed for Turn off / Turn on |
| `memory_max` | Like `"6G"`: if it takes more ordinary memory than that, it alone is stopped (and started again) instead of the machine running out. Uses `systemd-run` |
| `autostart` | `false` to list it without starting it; press Start on the page when you want it |
| `about` | A line shown under its name |

Besides `units`: `port` (the page's, 7460), `logs` (a folder; every program's
output is also kept there in full) and `server_data` (the server's data
folder, for the disk figure).

The host reads `host.json` once, at start: after changing it, stop the host
with Ctrl+C and start it again.

## Programs that are already running

If a program in the list is already running when the host starts (you started
the server by hand, say), the host does not start a second one: it shows that
one as "started outside the host" and cannot stop it. Stop it where you
started it and press Start, or start the host with `--take-over`, which ends
such copies itself and then starts its own.

## Starting with the machine

`corded-host.service.example` is a unit for `systemd --user`; the steps are at
its top. With it, the one host unit replaces a unit for each bot.
