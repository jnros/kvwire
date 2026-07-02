#!/usr/bin/env bash
# Bring up Soft-RoCE (rdma_rxe) on eth
#
#   ./sh_setup.sh <netdev>      e.g. ./sh_setup.sh eth0
#   ./sh_setup.sh               auto-pick first UP non-loopback iface
set -euo pipefail

DEV="${1:-}"
if [ -z "$DEV" ]; then
	DEV=$(ip -o link show up | awk -F': ' '$2 != "lo" {print $2; exit}')
fi
[ -n "$DEV" ] || { echo "no netdev found"; exit 1; }

sudo modprobe rdma_rxe

# drop a prior rxe0 if present
if rdma link show 2>/dev/null | grep -q '^link rxe0/'; then
	sudo rdma link delete rxe0
fi
sudo rdma link add rxe0 type rxe netdev "$DEV"

echo "== rxe0 bound to $DEV =="
rdma link show rxe0/1
echo
echo "verify both ends see a device:"
ibv_devices
echo
echo "IP of $DEV (give this to the client):"
ip -4 -o addr show "$DEV" | awk '{print "  " $4}'
