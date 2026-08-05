#!/usr/bin/env bash
#
# drop-mirror.sh — mirror GET /drop/board.md into a file (Obsidian vault).
# Writes only when the board actually changed (atomic tmp+mv), so file
# watchers and sync clients stay quiet. Meant for cron/launchd; an
# unreachable server is silence (exit 0), missing config is a loud error.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=drop-common.sh
# shellcheck disable=SC1091
. "$here/drop-common.sh" || exit 1

[ -n "${DROP_URL:-}" ] || { echo "drop-mirror: DROP_URL not set" >&2; exit 1; }
[ -n "${DROP_BOARD_FILE:-}" ] || {
    echo "drop-mirror: DROP_BOARD_FILE not set — refusing to guess a vault path" >&2
    exit 1
}

board="$(curl -sf -m 3 -H "Authorization: Bearer ${DROP_TOKEN:-}" \
    "$DROP_URL/drop/board.md" 2>/dev/null)" || exit 0
[ -n "$board" ] || exit 0

tmp="$DROP_BOARD_FILE.tmp.$$"
printf '%s\n' "$board" > "$tmp"
if [ -f "$DROP_BOARD_FILE" ] && cmp -s "$tmp" "$DROP_BOARD_FILE"; then
    rm -f "$tmp"
    exit 0
fi
mv -f "$tmp" "$DROP_BOARD_FILE"
