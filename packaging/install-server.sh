#!/usr/bin/env bash
# Installs the Corded server as a locked-down system service on Linux
# (Raspberry Pi OS, Debian, Ubuntu). Run it from the folder of a release
# download, as root:
#
#   sudo ./install-server.sh --name "My Server" --owner yourname --scope internet
#
# What it does:
#   - creates an account called "corded" that can do nothing but run the server
#   - copies the server program to /opt/corded and keeps its data in /var/lib/corded
#   - installs a systemd service with the operating system's sandboxing turned on
#   - with --firewall, allows only SSH and the Corded port in, and stops the
#     server account from connecting out to private network addresses
#
# It does not open anything on your router. See the operator notes in the
# repository for putting the machine on a guest network.
#
# NOT YET TESTED ON A REAL RASPBERRY PI. Read it before running it.
set -euo pipefail

name="corded"
owner=""
scope="network"
port="7443"
firewall="no"
extra=()

while [ $# -gt 0 ]; do
  case "$1" in
    --name) name="$2"; shift 2 ;;
    --owner) owner="$2"; shift 2 ;;
    --scope) scope="$2"; shift 2 ;;
    --port) port="$2"; shift 2 ;;
    --firewall) firewall="yes"; shift ;;
    --invite-only|--open-registration|--closed|--no-history-sharing) extra+=("$1"); shift ;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this with sudo." >&2
  exit 1
fi
here="$(cd "$(dirname "$0")" && pwd)"
if [ ! -x "$here/cordedd" ]; then
  echo "cordedd was not found next to this script. Run it from the unpacked release folder." >&2
  exit 1
fi
case "$scope" in machine|network|internet) ;; *) echo "--scope must be machine, network or internet" >&2; exit 2 ;; esac
case "$port" in ''|*[!0-9]*) echo "--port must be a number" >&2; exit 2 ;; esac

echo "Creating the corded account and folders..."
if ! id corded >/dev/null 2>&1; then
  useradd --system --home-dir /var/lib/corded --shell /usr/sbin/nologin corded
fi
install -d -o root -g root -m 0755 /opt/corded
install -o root -g root -m 0755 "$here/cordedd" /opt/corded/cordedd
install -d -o corded -g corded -m 0700 /var/lib/corded

args=(--data /var/lib/corded --port "$port" --scope "$scope" --name "$name")
[ -n "$owner" ] && args+=(--owner "$owner")
args+=("${extra[@]}")
exec_line="/opt/corded/cordedd"
for a in "${args[@]}"; do
  # Quote each argument for the service file.
  exec_line+=" \"${a//\"/\\\"}\""
done

echo "Installing the service..."
cat > /etc/systemd/system/corded.service <<UNIT
[Unit]
Description=Corded server
After=network-online.target
Wants=network-online.target

[Service]
User=corded
Group=corded
ExecStart=$exec_line
Restart=always
RestartSec=3

# The server may touch its own data folder and nothing else.
StateDirectory=corded
ReadWritePaths=/var/lib/corded
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
PrivateDevices=true
NoNewPrivileges=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectKernelLogs=true
ProtectControlGroups=true
ProtectClock=true
ProtectHostname=true
RestrictNamespaces=true
RestrictRealtime=true
RestrictSUIDSGID=true
LockPersonality=true
MemoryDenyWriteExecute=true
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
SystemCallArchitectures=native
SystemCallFilter=@system-service
CapabilityBoundingSet=
AmbientCapabilities=
UMask=0077
MemoryMax=512M
TasksMax=64

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload

if [ "$firewall" = "yes" ]; then
  if ! command -v nft >/dev/null 2>&1; then
    echo "Installing nftables for the firewall..."
    apt-get update && apt-get install -y nftables
  fi
  echo "Setting the firewall..."
  uid="$(id -u corded)"
  cat > /etc/nftables.d-corded.nft <<NFT
# Installed by Corded's install-server.sh. Incoming: only SSH and the Corded
# port. Outgoing from the server's account: nothing to private addresses, so a
# break-in cannot be used to reach other machines on the local network.
table inet corded_guard
delete table inet corded_guard
table inet corded_guard {
  chain input {
    type filter hook input priority 0; policy drop;
    iif "lo" accept
    ct state established,related accept
    meta l4proto { icmp, ipv6-icmp } accept
    tcp dport 22 accept
    tcp dport $port accept
  }
  chain output {
    type filter hook output priority 0; policy accept;
    oif "lo" accept
    ct state established,related accept
    meta skuid $uid ip daddr { 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16, 100.64.0.0/10 } drop
    meta skuid $uid ip6 daddr { fc00::/7, fe80::/10 } drop
  }
}
NFT
  nft -f /etc/nftables.d-corded.nft
  # Load it again at boot.
  if [ -f /etc/nftables.conf ] && ! grep -q "nftables.d-corded.nft" /etc/nftables.conf; then
    echo 'include "/etc/nftables.d-corded.nft"' >> /etc/nftables.conf
  fi
  systemctl enable nftables >/dev/null 2>&1 || true
fi

systemctl enable --now corded.service
sleep 2
echo
systemctl --no-pager --lines=0 status corded.service || true
echo
echo "The server's fingerprint and settings are in its log:"
journalctl -u corded.service --no-pager -n 12 | sed 's/^/  /'
echo
echo "Done. Useful commands:"
echo "  sudo systemctl status corded      is it running?"
echo "  sudo journalctl -u corded -f      watch its log"
echo "  sudo systemctl restart corded     restart it"
echo "To update later: unpack a newer release and run this script again."
