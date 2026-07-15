#!/bin/sh
# Example SIPp invocations for mod_broadcast (spec §26.3 / §21.7).
# Usage: FS_HOST=127.0.0.1 FS_PORT=5060 SPEAKER_EXT=888801 LISTENER_EXT=888802 ./run_sipp_examples.sh [smoke|load]
# Optional: SIPP_BIN=/path/to/sipp if sipp is not in PATH.

set -e
FS_HOST="${FS_HOST:-127.0.0.1}"
FS_PORT="${FS_PORT:-5060}"
SPEAKER_EXT="${SPEAKER_EXT:-888801}"
LISTENER_EXT="${LISTENER_EXT:-888802}"
HERE="$(cd "$(dirname "$0")" && pwd)"
MODE="${1:-smoke}"

TARGET="${FS_HOST}:${FS_PORT}"

# Resolve SIPp binary: explicit override, then PATH, then common install paths.
find_sipp() {
	if test -n "${SIPP_BIN}" && test -x "${SIPP_BIN}"; then
		printf '%s' "${SIPP_BIN}"
		return 0
	fi
	if command -v sipp >/dev/null 2>&1; then
		command -v sipp
		return 0
	fi
	for c in /usr/bin/sipp /usr/local/bin/sipp; do
		if test -x "$c"; then
			printf '%s' "$c"
			return 0
		fi
	done
	return 1
}

if ! SIPP="$(find_sipp)"; then
	echo "SIPp (sipp) not found." >&2
	echo "" >&2
	echo "Install on Debian/Ubuntu:" >&2
	echo "  apt-get update && apt-get install -y sipp" >&2
	echo "  # or: apt install sipp" >&2
	echo "" >&2
	echo "If sipp is installed outside PATH, set:" >&2
	echo "  export SIPP_BIN=/full/path/to/sipp" >&2
	exit 1
fi

case "$MODE" in
smoke)
	echo "Smoke: 1 speaker + 1 listener -> $TARGET (using $SIPP)"
	"$SIPP" -sf "$HERE/sipp/bcast_speaker.xml" -m 1 -r 1 -l 1 -s "$SPEAKER_EXT" "$TARGET" &
	SPID=$!
	sleep 1
	"$SIPP" -sf "$HERE/sipp/bcast_listener.xml" -m 1 -r 1 -l 1 -s "$LISTENER_EXT" "$TARGET"
	wait $SPID
	;;
load)
	echo "Load: 1 speaker + 200 listeners (10 cps) -> $TARGET (using $SIPP)"
	"$SIPP" -sf "$HERE/sipp/bcast_speaker.xml" -m 1 -r 1 -l 1 -s "$SPEAKER_EXT" "$TARGET" &
	SPID=$!
	sleep 2
	"$SIPP" -sf "$HERE/sipp/bcast_listener.xml" -m 200 -r 10 -l 200 -s "$LISTENER_EXT" "$TARGET"
	wait $SPID
	;;
*)
	echo "Usage: $0 [smoke|load]" >&2
	exit 2
	;;
esac
