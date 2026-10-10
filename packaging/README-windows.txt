Corded for Windows, pre-alpha. Not security audited.
https://github.com/AndrewCromar/corded

This is the first Windows build. It compiles and its self-tests pass, but it
has had far less real use than the Linux version. Please report what breaks.

What is here
  corded-tui.exe   the terminal client
  cordedd.exe      the server
  corded-cli.exe   a scripting client (JSON lines)
  corded.dll       the core library the clients use; keep it next to them

Run the client
  Open Windows Terminal (or PowerShell) in this folder, then:

    .\corded-tui.exe --vault .\my-vault --server HOST:7443 --name yourname

  or, with an invite link from a member:

    .\corded-tui.exe --vault .\my-vault --name yourname --join "corded://..."

  To be the same person you already are on another device, get the key there
  with /recovery-key and add:   --recovery-key "THE-KEY"

  Windows may ask whether to allow it through the firewall the first time.
  Type /help inside the client for the commands, /exit to leave.

Run a server
    .\cordedd.exe --data .\server-data --name "My Server" --owner yourname --scope network

  --scope machine    only this computer can connect (the default)
  --scope network    devices on your local network
  --scope internet   anyone; new accounts then need an invite
