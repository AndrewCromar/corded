Corded, pre-alpha. Not security audited. See https://github.com/AndrewCromar/corded

What is here
  cordedd       the server
  corded-tui    the terminal client
  corded-cli    a scripting client (JSON lines)
  libcorded.so  the core library the clients use; keep it next to them

Run a server
  ./cordedd --data ./server-data --name "My Server" --owner yourname --scope network

  --scope machine    only this computer can connect (the default)
  --scope network    devices on your local network
  --scope internet   anyone; new accounts then need an invite

  The server prints a "server fingerprint". Settings are remembered, so later
  starts only need --data.

Run a client
  ./corded-tui --vault ./my-vault --server HOST:7443 --name yourname
  or, with an invite link from a member:
  ./corded-tui --vault ./my-vault --name yourname --join 'corded://...'

  Type /help inside the client for the commands.

Install the server as a locked-down service (Raspberry Pi OS, Debian, Ubuntu)
  sudo ./install-server.sh --name "My Server" --owner yourname --scope internet --firewall
  Read HOSTING-AT-HOME.md first if the machine is in a house with other
  people's devices.
