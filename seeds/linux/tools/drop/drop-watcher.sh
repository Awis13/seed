#!/usr/bin/env bash
#
# drop-watcher.sh — background poller that turns drop mail into a file event.
# Every ${DROP_POLL_S:-20}s it PEEKS the inbox; when there is mail it writes
# $DROP_INBOX_FILE (atomic tmp+mv, only on change), which fires an active
# session's FileChanged hook. Peek never advances the cursor, so the
# consuming hooks still deliver. When the inbox empties (consumed elsewhere)
# the file is removed so stale mail cannot re-fire.
#
# WATCH_ONCE=1 runs a single poll and exits (used by the tests).
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=drop-common.sh
# shellcheck disable=SC1091
. "$here/drop-common.sh" || exit 1
drop_configured || {
    echo "drop-watcher: DROP_URL/DROP_HANDLE not configured (or bad handle)" >&2
    exit 1
}

pidfile="${DROP_WATCHER_PIDFILE:-${TMPDIR:-/tmp}/drop-watcher-$DROP_HANDLE.pid}"
# Atomic claim via noclobber create: two racing starts cannot both win.
# A stale file (previous run SIGKILLed) is taken over when its pid is dead.
if ! (set -C; echo $$ > "$pidfile") 2>/dev/null; then
    oldpid="$(cat "$pidfile" 2>/dev/null)"
    if [ -n "$oldpid" ] && kill -0 "$oldpid" 2>/dev/null; then
        echo "drop-watcher: already running (pid $oldpid)" >&2
        exit 1
    fi
    rm -f "$pidfile"
    if ! (set -C; echo $$ > "$pidfile") 2>/dev/null; then
        echo "drop-watcher: lost the pidfile race" >&2
        exit 1
    fi
fi
trap 'rm -f "$pidfile"' EXIT
trap 'exit 0' TERM INT

poll_s="${DROP_POLL_S:-20}"
case "$poll_s" in *[!0-9]*|'') poll_s=20 ;; esac
[ "$poll_s" -ge 1 ] || poll_s=20

poll_once() {
    json="$(drop_fetch_inbox peek)" || return 0
    [ -n "$json" ] || return 0
    count="$(printf '%s' "$json" | python3 -c '
import json, sys
print(json.load(sys.stdin).get("count", 0))
' 2>/dev/null)" || return 0
    # Only a CONFIRMED count may act: on anything unparsable do nothing —
    # in particular, never delete the inbox file on a failed peek.
    case "$count" in *[!0-9]*|'') return 0 ;; esac

    if [ "$count" -gt 0 ]; then
        # The tmp name must not contain the FileChanged matcher substring
        # (.drop-inbox.md), or every tmp write would fire the hook itself.
        tmp="$(dirname "$DROP_INBOX_FILE")/.drop-tmp.$$"
        {
            printf '# Drop inbox for %s — %s waiting\n\n' "$DROP_HANDLE" "$count"
            printf '%s' "$json" | drop_render_lines
        } > "$tmp"
        if [ -f "$DROP_INBOX_FILE" ] && cmp -s "$tmp" "$DROP_INBOX_FILE"; then
            rm -f "$tmp"   # same mail already signalled; do not re-fire
        else
            mv -f "$tmp" "$DROP_INBOX_FILE"
        fi
    else
        rm -f "$DROP_INBOX_FILE"
    fi
}

if [ "${WATCH_ONCE:-0}" = "1" ]; then
    poll_once
    exit 0
fi

while :; do
    poll_once
    sleep "$poll_s" &
    wait $! || true
done
