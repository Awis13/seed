# shellcheck shell=bash
#
# drop-common.sh — shared plumbing for the drop transport kit. Sourced, not
# executed. Everything here fails silent and fast: these functions run inside
# Claude Code hooks, and a dead drop server must never break a session.

# ~/.claude/drop.env provides defaults; already-exported vars win, keyed on
# DROP_URL so test environments stay hermetic.
if [ -z "${DROP_URL:-}" ] && [ -f "$HOME/.claude/drop.env" ]; then
    # Redirected: an env file that echoes would pollute every hook's stdout
    # and could corrupt the on-stop decision JSON.
    # shellcheck source=/dev/null
    . "$HOME/.claude/drop.env" >/dev/null 2>&1
fi

DROP_INBOX_FILE="${DROP_INBOX_FILE:-./.drop-inbox.md}"

# Resolve DROP_HANDLE. An explicit value wins; otherwise derive one from
# the project so each repo gets its own inbox. drop.env is SHARED across
# every session on the machine, so a hardcoded handle there would make all
# sessions register as one handle and consume each other's mail (a fetch
# advances the shared cursor). Sets DROP_HANDLE and returns 0 on success;
# returns 1 (leaving DROP_HANDLE empty) if nothing usable can be derived.
drop_resolve_handle() {
    if [ -n "${DROP_HANDLE:-}" ]; then
        return 0
    fi
    local base
    base="$(git rev-parse --show-toplevel 2>/dev/null)" || base=""
    [ -n "$base" ] || base="$PWD"
    base="$(basename "$base")"
    # Collapse anything outside the server charset into single '-', trim
    # leading/trailing separators, cap at 32.
    local h
    h="$(printf '%s' "$base" \
        | tr -c 'A-Za-z0-9_-' '-' \
        | tr -s '-' \
        | sed 's/^-*//; s/-*$//' \
        | cut -c1-32 \
        | sed 's/-*$//')"
    [ -n "$h" ] || return 1
    DROP_HANDLE="$h"
    return 0
}

# True when the kit has enough config to talk to a drop server and a usable
# handle (explicit or derived) matching the server charset [A-Za-z0-9_-]{1,32}.
# python3 is checked by RUNNING it, before any fetch: a consuming fetch
# followed by a dead renderer advances the server cursor and loses the
# mail forever. No python3 means no-op — the mail waits.
drop_configured() {
    [ -n "${DROP_URL:-}" ] || return 1
    drop_resolve_handle || return 1
    case "$DROP_HANDLE" in
        *[!A-Za-z0-9_-]*) return 1 ;;
    esac
    [ "${#DROP_HANDLE}" -ge 1 ] && [ "${#DROP_HANDLE}" -le 32 ] || return 1
    python3 -c "" >/dev/null 2>&1 || return 1
}

# GET the inbox for $DROP_HANDLE; "$1" = peek leaves the cursor alone.
# The output is validated as complete JSON BEFORE anything consumes it:
# curl failure or a torn body both mean "no fetch happened" (returns 1,
# prints nothing). The server only commits the cursor after a full
# response write, so a torn fetch redelivers next time — both layers
# agree that a duplicate is acceptable and loss is not.
# Consuming fetches get 8s (the node may be busy compiling firmware);
# the watcher's peek stays at 3s.
drop_fetch_inbox() {
    local url="$DROP_URL/drop/inbox?handle=$DROP_HANDLE"
    local timeout=8
    if [ "${1:-}" = "peek" ]; then
        url="$url&peek=1"
        timeout=3
    fi
    local raw
    raw="$(curl -sf -m "$timeout" -H "Authorization: Bearer ${DROP_TOKEN:-}" \
        "$url" 2>/dev/null)" || return 1
    printf '%s' "$raw" | python3 -c '
import json, sys
raw = sys.stdin.read()
json.loads(raw)
sys.stdout.write(raw)
' 2>/dev/null || return 1
}

# Canonical one-line message format — the single definition, prepended to
# every python snippet that renders a message (here and in on-stop.sh).
DROP_PY_FMT='
def drop_line(m):
    line = "#%s %s -> %s" % (m.get("id"), m.get("from"), m.get("to"))
    if m.get("reply_to"):
        line += " (re #%s)" % m.get("reply_to")
    if m.get("link"):
        line += " [%s]" % m.get("link")
    return "%s: %s" % (line, m.get("text", ""))
'

# stdin: inbox JSON; stdout: one line per message, nothing when empty or
# unparsable.
drop_render_lines() {
    python3 -c "${DROP_PY_FMT}"'
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    raise SystemExit(0)
for m in d.get("messages") or []:
    print(drop_line(m))
' 2>/dev/null || true
}
