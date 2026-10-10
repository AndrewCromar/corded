#!/bin/sh
# Downloads the newest Corded release for Linux (x86-64 or ARM64, including a
# Raspberry Pi) and unpacks it under ~/.local/share/corded. One line:
#
#   curl -fsSL https://raw.githubusercontent.com/AndrewCromar/corded/main/packaging/install.sh | sh
#
# Then run the programs from the folder it prints. Nothing is installed
# system-wide. Run it again later to update.
#
# Corded is pre-alpha and has not been security audited.
set -eu

case "$(uname -m)" in
  x86_64|amd64) arch="x86_64" ;;
  aarch64|arm64) arch="arm64" ;;
  *) echo "There is no Corded build for $(uname -m) yet." >&2; exit 1 ;;
esac

api="https://api.github.com/repos/AndrewCromar/corded/releases?per_page=10"
url="$(curl -fsSL -H 'User-Agent: corded-installer' "$api" \
  | grep -o "https://[^\"]*-linux-${arch}\.tar\.gz" | head -n 1)"
if [ -z "$url" ]; then
  echo "No Corded release with a Linux ${arch} build was found." >&2
  exit 1
fi

name="$(basename "$url" .tar.gz)"
root="${XDG_DATA_HOME:-$HOME/.local/share}/corded"
mkdir -p "$root"
if [ ! -x "$root/$name/corded-tui" ]; then
  echo "Downloading $name ..."
  tmp="$(mktemp)"
  curl -fsSL -o "$tmp" "$url"
  # Check the download against the checksum published next to it.
  if expected="$(curl -fsSL "$url.sha256" 2>/dev/null | cut -d' ' -f1)" && [ -n "$expected" ]; then
    actual="$(sha256sum "$tmp" | cut -d' ' -f1)"
    if [ "$expected" != "$actual" ]; then
      echo "The download does not match its published checksum; not installing it." >&2
      rm -f "$tmp"
      exit 1
    fi
  fi
  tar -C "$root" -xzf "$tmp"
  rm -f "$tmp"
fi
ln -sfn "$root/$name" "$root/current"

echo
echo "Corded is in $root/current"
echo "  Client:  $root/current/corded-tui --vault $root/vault --server HOST:7443 --name yourname"
echo "  Server:  $root/current/cordedd --data $root/server-data --name \"My Server\" --owner yourname --scope network"
