#!/bin/sh
# Adds Corded to this computer's application menu, with its icon, pointing at
# the folder this script is in. Nothing outside your home folder is touched.
# Run it again after moving the folder. To undo: delete the two files it names.
set -e
here=$(cd "$(dirname "$0")" && pwd)
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/512x512/apps"
mkdir -p "$apps" "$icons"
cp "$here/corded.png" "$icons/org.corded.corded_app.png"
# The file is named after the app's id, which is how the desktop matches an
# open window to its entry and so to its icon.
cat > "$apps/org.corded.corded_app.desktop" <<ENTRY
[Desktop Entry]
Type=Application
Name=Corded
Comment=End-to-end encrypted chat
Exec="$here/corded_app"
Icon=org.corded.corded_app
Terminal=false
Categories=Network;InstantMessaging;
StartupWMClass=org.corded.corded_app
ENTRY
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor" >/dev/null 2>&1 || true
echo "Corded is in your application menu now."
echo "  $apps/org.corded.corded_app.desktop"
echo "  $icons/org.corded.corded_app.png"
