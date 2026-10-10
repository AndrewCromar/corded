Corded, the desktop app
=======================

This folder is the whole app. Keep its files together.

  Windows:  double-click corded_app.exe
  Linux:    run ./corded_app   (needs GTK 3, which desktop Linux has)

The first time, it asks for a name and a passphrase, then for the address of a
server to join. To be the same person as on your phone or in the terminal
client, choose "I have another device" and enter your recovery key.

Your messages are kept encrypted on this computer:
  Windows:  %APPDATA%\org.corded\corded_app
  Linux:    ~/.local/share/org.corded.corded_app

Enter sends a message; Shift+Enter starts a new line. Right-click a message
for reply, react, thread and the rest.

This is an early build. Nothing here has been security audited.
