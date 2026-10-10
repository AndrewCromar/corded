# Hosting a Corded server at home, isolated from the home network

This is for running a server on a Raspberry Pi (or any small Linux machine) in a house
where other people's devices live, without asking them to trust the server.

The isolation comes from the **network**, not from Corded. Assume the server could be
broken into, and arrange things so that it would not matter to anyone else in the house.

> Corded is pre-alpha and unaudited. That is the reason to follow this guide rather than
> a reason to skip it.

## 1. Put the machine on a guest network

Nearly every home router has a guest network. Devices on it reach the internet but are
blocked from the main network.

1. In the router's settings, turn on the guest network.
2. Make sure guests **cannot** reach the local network. Routers word this differently:
   "Allow guests to see each other and access my local network" must be **off**; "AP
   isolation" or "client isolation" must be **on**.
3. Connect the server machine to the guest network, and to nothing else. If it has both
   Wi-Fi and a cable, use only the one that is on the guest network.
4. Check it. From the server machine, try to reach a device on the main network:

   ```sh
   ping -c 3 192.168.1.10      # use the address of a computer on the main network
   ```

   It must fail. Then, from a computer on the main network, try to reach the server
   machine. That must fail too. If either works, the guest network is not isolating, and
   you should fix that before going further.

For a stronger wall, plug a second, cheap router into the first and put only the server
machine behind it. Same idea, separate hardware.

## 2. Install the server as a locked-down service

Unpack a release for your machine (ARM64 for a Raspberry Pi 4 or 5 with a 64-bit system)
and run the installer from inside it:

```sh
sudo ./install-server.sh --name "My Server" --owner yourname --scope internet --firewall
```

It creates an account that can do nothing except run the server, installs the server
with the operating system's sandboxing switched on, and with `--firewall`:

- lets in only SSH and the Corded port, and
- stops the server's account from connecting to private network addresses at all, so it
  could not be used to probe other machines even if the guest network were misconfigured.

`--firewall` replaces the machine's incoming firewall rules. Use it on a machine that
does nothing else.

The installer prints the server's fingerprint. Keep it: people can check it when they
first connect.

## 3. Let people reach it

A home router does not pass incoming connections to devices by default. Two ways:

| | What changes on the router | What the house exposes |
|---|---|---|
| **Port forwarding** | One rule: forward port 7443 to the server machine | One port, which leads only to the isolated machine |
| **An outgoing tunnel** | Nothing | Nothing. The server connects out to a relay, and people connect to the relay |

Port forwarding is fine when step 1 is done properly, but not every router can forward
to a device on its guest network. A tunnel service avoids the router entirely. Corded
does not have a built-in tunnel yet; it is planned.

Start with `--scope internet` either way. That makes new accounts need an invite, which
you create from your own client with `/invite`.

## 4. Keep it single-purpose

- Nothing else on the machine: no family files, no saved passwords, no other services.
- Keep its operating system updated (`sudo apt update && sudo apt full-upgrade`).
- Manage the server from your own Corded client (`/settings`, `/status`, `/reboot`), so
  you rarely need to log in to the machine.

## What this gives you, and what it does not

**It gives you:** someone who completely takes over the server cannot reach any other
device in the house. That does not depend on Corded having no bugs.

**It does not give you:** zero risk of any kind. The machine's internet connection could
still be misused, and it shares the home's public address. A guest network is only as
good as the router that implements it; check it as described in step 1.

## Checking on it later

```sh
sudo systemctl status corded       # is it running?
sudo journalctl -u corded -n 50    # recent log
```
