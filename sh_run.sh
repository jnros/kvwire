#!/usr/bin/env bash
# Two servers:
#
#   (target):  ./sh_run.sh server lat
#   (driver):  ./sh_run.sh client <B-ip> lat /path/to/kvmix/data/c4_fp16
#
set -euo pipefail
cd "$(dirname "$0")"
make -s

case "${1:-}" in
probe)
	./kvwire --probe ${2:+--dev "$2"}
	;;
server)
	./kvwire --server --mode "${2:-lat}"
	;;
client)
	./kvwire --client "$2" --mode "${3:-lat}" ${4:+--kv "$4"}
	;;
smoke)
	MODE="${2:-lat}"
	./kvwire --server --mode "$MODE" &
	sleep 0.5
	./kvwire --client 127.0.0.1 --mode "$MODE"
	wait
	;;
*)
	echo "usage: $0 {probe [dev] | server [mode] | client <ip> [mode] [kvdir] | smoke [mode]}"
	exit 1
	;;
esac
