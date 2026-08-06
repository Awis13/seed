#!/usr/bin/env bash
#
# Regression test for the drop transport kit. Runs every script against a
# mock curl (a PATH shim serving canned fixtures) — no network, no seed.
#
# The failure modes worth pinning are the quiet ones: a hook that prints on
# an empty inbox pollutes every session; one that errors on a dead server
# breaks every session; a Stop hook whose hand-built JSON tears on a quote
# in message text silently stops blocking; a watcher that rewrites an
# unchanged inbox file re-fires the FileChanged hook every poll; a mirror
# that rewrites an unchanged board keeps vault sync churning forever.
#
# Usage: tools/drop/test_drop_tools.sh

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Hermetic: no real ~/.claude/drop.env, no real curl, everything in $work.
export HOME="$work/home"
mkdir -p "$HOME" "$work/bin" "$work/fix" "$work/proj"
export PATH="$work/bin:$PATH"
export DROP_URL="http://drop.mock:1"
export DROP_TOKEN="mock-token"
export DROP_HANDLE="sess-test"
export DROP_WATCHER_PIDFILE="$work/watcher.pid"
unset DROP_INBOX_FILE DROP_BOARD_FILE 2>/dev/null || true
cd "$work/proj" || exit 1

# --- mock curl: fixture by URL shape, failure via MOCK_CURL_EXIT ---
cat > "$work/bin/curl" <<'MOCK'
#!/usr/bin/env bash
[ -n "${MOCK_CURL_LOG:-}" ] && printf '%s\n' "$*" >> "$MOCK_CURL_LOG"
[ "${MOCK_CURL_EXIT:-0}" != "0" ] && exit "$MOCK_CURL_EXIT"
url=""
for a in "$@"; do url="$a"; done
body=""
case "$url" in
    *peek=1*)          body="$MOCK_PEEK" ;;
    */drop/inbox*)     body="$MOCK_FETCH" ;;
    */drop/board.md*)  body="$MOCK_BOARD" ;;
    *) exit 22 ;;
esac
if [ "${MOCK_CURL_PARTIAL:-0}" = "1" ]; then
    # Torn transfer: half the body, then curl's "partial file" exit code.
    head -c 40 "$body"
    exit 18
fi
if [ "${MOCK_CURL_TRUNC:-0}" = "1" ]; then
    # Torn body but a clean exit — only JSON validation can catch this.
    head -c 40 "$body"
    exit 0
fi
cat "$body"
MOCK
chmod +x "$work/bin/curl"

# --- fixtures ---
cat > "$work/fix/inbox_full.json" <<'EOF'
{"handle":"sess-test","cursor":3,"count":2,"messages":[
{"id":4,"ts":49500,"from":"sess-b","to":"sess-test","reply_to":0,"link":"","text":"say \"hi\" \\ done"},
{"id":5,"ts":49560,"from":"sess-c","to":"all","reply_to":4,"link":"TICKET/C2","text":"second message"}]}
EOF
cat > "$work/fix/inbox_empty.json" <<'EOF'
{"handle":"sess-test","cursor":5,"count":0,"messages":[]}
EOF
printf '# Drop board\n\n1 message(s), oldest first.\n\n- line one\n' > "$work/fix/board1.md"
printf '# Drop board\n\n2 message(s), oldest first.\n\n- line one\n- line two\n' > "$work/fix/board2.md"

export MOCK_PEEK="$work/fix/inbox_full.json"
export MOCK_FETCH="$work/fix/inbox_full.json"
export MOCK_BOARD="$work/fix/board1.md"

# --- scaffolding ---
pass=0
fail=0
ok()  { pass=$((pass + 1)); printf '  ok:   %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf '  FAIL: %s\n' "$1"; }

rc=0
run() {  # run <cmd...>; captures stdout/stderr/rc
    "$@" > "$work/out" 2> "$work/err"
    rc=$?
}
expect_rc() {  # expect_rc <want> <desc>
    if [ "$rc" -eq "$1" ]; then ok "$2"; else bad "$2 (rc=$rc, wanted $1)"; fi
}
out_has()   { if grep -qF -- "$1" "$work/out"; then ok "$2"; else bad "$2"; fi }
err_has()   { if grep -qF -- "$1" "$work/err"; then ok "$2"; else bad "$2"; fi }
out_empty() { if [ ! -s "$work/out" ]; then ok "$1"; else bad "$1 (got: $(head -c 120 "$work/out"))"; fi }
sig() { python3 -c 'import os,sys; s=os.stat(sys.argv[1]); print(s.st_ino, s.st_mtime_ns)' "$1"; }

echo "session-start.sh"
{
    MOCK_FETCH="$work/fix/inbox_full.json" run bash "$here/hooks/session-start.sh"
    expect_rc 0 "exits 0 with mail"
    out_has "DROP INBOX for sess-test" "prints the banner"
    out_has '#4 sess-b -> sess-test: say "hi" \ done' "renders the first message"
    out_has "#5 sess-c -> all (re #4) [TICKET/C2]: second message" \
        "renders reply_to and link"

    MOCK_FETCH="$work/fix/inbox_empty.json" run bash "$here/hooks/session-start.sh"
    expect_rc 0 "exits 0 on an empty inbox"
    out_empty "and prints nothing"

    MOCK_CURL_EXIT=7 run bash "$here/hooks/session-start.sh"
    expect_rc 0 "exits 0 when the server is unreachable"
    out_empty "and prints nothing then either"
}

echo "broken python3: no-op BEFORE any fetch (mail must keep waiting)"
{
    # A consuming fetch with a dead renderer would advance the cursor and
    # lose the mail forever, so a broken python3 must stop the hook before
    # curl runs — asserted via the mock's call log.
    mkdir -p "$work/nopy"
    printf '#!/bin/sh\nexit 127\n' > "$work/nopy/python3"
    chmod +x "$work/nopy/python3"

    rm -f "$work/curl.log"
    PATH="$work/nopy:$PATH" MOCK_CURL_LOG="$work/curl.log" \
        run bash "$here/hooks/session-start.sh"
    expect_rc 0 "session-start exits 0"
    out_empty "silently"
    if [ ! -s "$work/curl.log" ]; then
        ok "and never calls curl (cursor untouched)"
    else bad "and never calls curl (cursor untouched)"; fi

    rm -f "$work/curl.log"
    PATH="$work/nopy:$PATH" MOCK_CURL_LOG="$work/curl.log" \
        run bash "$here/hooks/on-stop.sh"
    expect_rc 0 "on-stop exits 0"
    out_empty "with no block JSON"
    if [ ! -s "$work/curl.log" ]; then
        ok "and never calls curl either"
    else bad "and never calls curl either"; fi
}

echo "prompt-inject.sh"
{
    MOCK_FETCH="$work/fix/inbox_full.json" run bash "$here/hooks/prompt-inject.sh"
    expect_rc 0 "exits 0 with mail"
    out_has "NEW DROP MESSAGES for sess-test" "prints only when non-empty"
    out_has "#4 sess-b -> sess-test" "with the messages"

    MOCK_FETCH="$work/fix/inbox_empty.json" run bash "$here/hooks/prompt-inject.sh"
    expect_rc 0 "empty inbox exits 0"
    out_empty "silently"

    MOCK_CURL_EXIT=7 run bash "$here/hooks/prompt-inject.sh"
    expect_rc 0 "unreachable server exits 0"
    out_empty "silently too"
}

echo "on-stop.sh"
{
    MOCK_FETCH="$work/fix/inbox_full.json" run bash "$here/hooks/on-stop.sh"
    expect_rc 0 "exits 0 with mail"
    if python3 - "$work/out" <<'PY'
import json, sys
d = json.loads(open(sys.argv[1]).read())
assert d["decision"] == "block", d
assert "NEW DROP MESSAGES for sess-test" in d["reason"], d
assert 'say "hi" \\ done' in d["reason"], d
assert "Acknowledge/act, then finish." in d["reason"], d
# the canonical line format shared with drop_render_lines
assert '#4 sess-b -> sess-test: say' in d["reason"], d
assert "#5 sess-c -> all (re #4) [TICKET/C2]: second message" in d["reason"], d
PY
    then ok "emits machine-valid block JSON carrying the tricky text"
    else bad "emits machine-valid block JSON carrying the tricky text"; fi

    MOCK_FETCH="$work/fix/inbox_empty.json" run bash "$here/hooks/on-stop.sh"
    expect_rc 0 "empty inbox exits 0"
    out_empty "with no output (stop passes)"

    MOCK_CURL_EXIT=7 run bash "$here/hooks/on-stop.sh"
    expect_rc 0 "unreachable server exits 0"
    out_empty "with no output (stop passes) too"
}

echo "torn fetches are treated as no fetch at all"
{
    # Half a JSON body must never count as mail — whether curl noticed the
    # tear (exit 18) or not (clean exit, caught only by JSON validation).
    MOCK_CURL_PARTIAL=1 run bash "$here/hooks/session-start.sh"
    expect_rc 0 "session-start survives a torn transfer"
    out_empty "and stays silent"

    MOCK_CURL_TRUNC=1 run bash "$here/hooks/session-start.sh"
    expect_rc 0 "session-start survives a torn body with a clean curl exit"
    out_empty "and stays silent then too"

    MOCK_CURL_PARTIAL=1 run bash "$here/hooks/on-stop.sh"
    expect_rc 0 "on-stop survives a torn transfer"
    out_empty "and does not emit a block"

    MOCK_CURL_TRUNC=1 run bash "$here/hooks/prompt-inject.sh"
    expect_rc 0 "prompt-inject survives a torn body"
    out_empty "silently"
}

echo "on-filechange.sh"
{
    printf 'inbox body here\n' > "$work/proj/.drop-inbox.md"
    run bash "$here/hooks/on-filechange.sh"
    expect_rc 2 "exits 2 when the inbox file exists"
    err_has "inbox body here" "with its content on stderr"
    out_empty "and nothing on stdout"

    rm -f "$work/proj/.drop-inbox.md"
    run bash "$here/hooks/on-filechange.sh"
    expect_rc 0 "exits 0 when the file is gone"
    out_empty "with no output"
}

echo "drop-watcher.sh (WATCH_ONCE)"
{
    MOCK_PEEK="$work/fix/inbox_full.json" WATCH_ONCE=1 run bash "$here/drop-watcher.sh"
    expect_rc 0 "single poll exits 0"
    if [ -f "$work/proj/.drop-inbox.md" ]; then ok "writes the inbox file"; else bad "writes the inbox file"; fi
    if grep -qF "#4 sess-b -> sess-test" "$work/proj/.drop-inbox.md"; then
        ok "with the messages in it"; else bad "with the messages in it"; fi
    if grep -qF "2 waiting" "$work/proj/.drop-inbox.md"; then
        ok "and the count in the header"; else bad "and the count in the header"; fi
    leftovers="$(find "$work/proj" -name '.drop-tmp.*' | wc -l | tr -d ' ')"
    if [ "$leftovers" = "0" ]; then ok "no tmp files left (atomic mv)"; else bad "no tmp files left (atomic mv)"; fi
    matcher_hits="$(find "$work/proj" -name '*.drop-inbox.md*' ! -name '.drop-inbox.md' | wc -l | tr -d ' ')"
    if [ "$matcher_hits" = "0" ]; then
        ok "no other filename carries the FileChanged matcher substring"
    else bad "no other filename carries the FileChanged matcher substring"; fi
    if [ ! -f "$DROP_WATCHER_PIDFILE" ]; then ok "pidfile cleaned up on exit"; else bad "pidfile cleaned up on exit"; fi

    before="$(sig "$work/proj/.drop-inbox.md")"
    MOCK_PEEK="$work/fix/inbox_full.json" WATCH_ONCE=1 run bash "$here/drop-watcher.sh"
    after="$(sig "$work/proj/.drop-inbox.md")"
    if [ "$before" = "$after" ]; then
        ok "same mail again: file untouched (no hook re-fire)"
    else bad "same mail again: file untouched (no hook re-fire)"; fi

    MOCK_PEEK="$work/fix/inbox_empty.json" WATCH_ONCE=1 run bash "$here/drop-watcher.sh"
    expect_rc 0 "empty inbox poll exits 0"
    if [ ! -f "$work/proj/.drop-inbox.md" ]; then
        ok "and removes the stale inbox file"
    else bad "and removes the stale inbox file"; fi

    MOCK_CURL_EXIT=7 WATCH_ONCE=1 run bash "$here/drop-watcher.sh"
    expect_rc 0 "unreachable server poll exits 0"

    # A torn peek must not delete the inbox file: only a CONFIRMED empty
    # inbox may. Recreate mail state, then tear the peek.
    MOCK_PEEK="$work/fix/inbox_full.json" WATCH_ONCE=1 run bash "$here/drop-watcher.sh"
    if [ -f "$work/proj/.drop-inbox.md" ]; then ok "inbox file back for the torn-peek case"; else bad "inbox file back for the torn-peek case"; fi
    MOCK_CURL_PARTIAL=1 WATCH_ONCE=1 run bash "$here/drop-watcher.sh"
    expect_rc 0 "a torn peek exits 0"
    if [ -f "$work/proj/.drop-inbox.md" ]; then
        ok "and leaves the inbox file alone"
    else bad "and leaves the inbox file alone"; fi
    MOCK_PEEK="$work/fix/inbox_empty.json" WATCH_ONCE=1 run bash "$here/drop-watcher.sh"
    if [ ! -f "$work/proj/.drop-inbox.md" ]; then
        ok "while a confirmed-empty inbox still removes it"
    else bad "while a confirmed-empty inbox still removes it"; fi

    DROP_HANDLE="not valid!" run bash "$here/drop-watcher.sh"
    if [ "$rc" -ne 0 ]; then ok "a bad handle is refused loudly"; else bad "a bad handle is refused loudly"; fi

    echo $$ > "$DROP_WATCHER_PIDFILE"
    WATCH_ONCE=1 run bash "$here/drop-watcher.sh"
    if [ "$rc" -ne 0 ]; then ok "a live pidfile refuses a second instance"; else bad "a live pidfile refuses a second instance"; fi
    err_has "already running" "and says whose it is"
    rm -f "$DROP_WATCHER_PIDFILE"

    echo 99999999 > "$DROP_WATCHER_PIDFILE"
    MOCK_PEEK="$work/fix/inbox_empty.json" WATCH_ONCE=1 run bash "$here/drop-watcher.sh"
    expect_rc 0 "a stale pidfile (dead pid) is taken over"
    if [ ! -f "$DROP_WATCHER_PIDFILE" ]; then
        ok "and cleaned up on exit"
    else bad "and cleaned up on exit"; fi

    # A junk poll interval must fall back to 20s, not hot-loop the server:
    # in one second of loop mode that is exactly one poll, not hundreds.
    rm -f "$work/curl.log"
    env MOCK_PEEK="$work/fix/inbox_empty.json" MOCK_CURL_LOG="$work/curl.log" \
        DROP_POLL_S=abc DROP_WATCHER_PIDFILE="$work/watcher-loop.pid" \
        bash "$here/drop-watcher.sh" >/dev/null 2>&1 &
    wpid=$!
    sleep 1
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null || true
    calls="$(wc -l < "$work/curl.log" | tr -d ' ')"
    if [ "$calls" -le 2 ]; then
        ok "DROP_POLL_S=abc falls back to 20s ($calls poll in 1s, no hot loop)"
    else bad "DROP_POLL_S=abc falls back to 20s (got $calls polls in 1s)"; fi
    if [ ! -f "$work/watcher-loop.pid" ]; then
        ok "and SIGTERM cleans the pidfile"
    else bad "and SIGTERM cleans the pidfile"; fi
}

echo "drop-mirror.sh"
{
    run bash "$here/drop-mirror.sh"
    if [ "$rc" -ne 0 ]; then ok "refuses to run without DROP_BOARD_FILE"; else bad "refuses to run without DROP_BOARD_FILE"; fi
    err_has "DROP_BOARD_FILE" "and says why"

    board="$work/proj/board-mirror.md"
    DROP_BOARD_FILE="$board" MOCK_BOARD="$work/fix/board1.md" run bash "$here/drop-mirror.sh"
    expect_rc 0 "first mirror run exits 0"
    if cmp -s "$board" "$work/fix/board1.md"; then ok "writes the board verbatim"; else bad "writes the board verbatim"; fi

    before="$(sig "$board")"
    DROP_BOARD_FILE="$board" MOCK_BOARD="$work/fix/board1.md" run bash "$here/drop-mirror.sh"
    after="$(sig "$board")"
    if [ "$before" = "$after" ]; then ok "unchanged board: file untouched"; else bad "unchanged board: file untouched"; fi

    DROP_BOARD_FILE="$board" MOCK_BOARD="$work/fix/board2.md" run bash "$here/drop-mirror.sh"
    if cmp -s "$board" "$work/fix/board2.md"; then ok "changed board: file updated"; else bad "changed board: file updated"; fi

    before="$(sig "$board")"
    DROP_BOARD_FILE="$board" MOCK_CURL_EXIT=7 run bash "$here/drop-mirror.sh"
    after="$(sig "$board")"
    expect_rc 0 "unreachable server exits 0 (cron stays quiet)"
    if [ "$before" = "$after" ]; then ok "and the mirror file is untouched"; else bad "and the mirror file is untouched"; fi
    leftovers="$(find "$work/proj" -name 'board-mirror.md.tmp.*' | wc -l | tr -d ' ')"
    if [ "$leftovers" = "0" ]; then ok "no tmp files left behind"; else bad "no tmp files left behind"; fi
}

echo "handle derivation (drop_resolve_handle)"
{
    # Resolve in a clean subshell: unset DROP_HANDLE, cd somewhere, source
    # the common file, ask it for a handle. DROP_URL is exported so the
    # env-file fallback stays inert.
    resolve() {  # resolve <dir> [explicit]; echoes the handle or nothing
        (
            cd "$1" || exit 1
            if [ -n "${2:-}" ]; then export DROP_HANDLE="$2"; else unset DROP_HANDLE; fi
            # shellcheck source=drop-common.sh
            # shellcheck disable=SC1091
            . "$here/drop-common.sh"
            drop_resolve_handle && printf '%s' "$DROP_HANDLE"
        )
    }

    git init -q "$work/cleanrepo"
    mkdir -p "$work/cleanrepo/sub/dir"
    got="$(resolve "$work/cleanrepo/sub/dir")"
    if [ "$got" = "cleanrepo" ]; then ok "in a repo: derives the repo-root basename"; else bad "in a repo: derives the repo-root basename (got '$got')"; fi

    got="$(resolve "$work/cleanrepo/sub/dir" "my-override")"
    if [ "$got" = "my-override" ]; then ok "explicit DROP_HANDLE wins over derivation"; else bad "explicit DROP_HANDLE wins (got '$got')"; fi

    # A non-repo dir: /tmp workdir is not under git, so it falls to PWD.
    mkdir -p "$work/plaindir"
    got="$(resolve "$work/plaindir")"
    if [ "$got" = "plaindir" ]; then ok "outside a repo: derives the PWD basename"; else bad "outside a repo: derives the PWD basename (got '$got')"; fi

    # Spaces, dots and punctuation collapse to the server charset.
    mkdir -p "$work/My.Weird Repo!!"
    got="$(resolve "$work/My.Weird Repo!!")"
    if [ "$got" = "My-Weird-Repo" ]; then ok "a messy dir name sanitizes to the charset"; else bad "a messy dir name sanitizes to the charset (got '$got')"; fi

    # Over-long names truncate to 32.
    mkdir -p "$work/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaBBBB"
    got="$(resolve "$work/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaBBBB")"
    if [ "${#got}" -le 32 ] && [ -n "$got" ]; then ok "an over-long name truncates to <=32 ($got)"; else bad "an over-long name truncates to <=32 (got '$got' len ${#got})"; fi

    # A name that is nothing but punctuation yields no handle -> fail loud.
    mkdir -p "$work/@@@"
    got="$(resolve "$work/@@@")"
    if [ -z "$got" ]; then ok "an all-punctuation name derives nothing (fails loud, no garbage handle)"; else bad "an all-punctuation name should derive nothing (got '$got')"; fi

    # And a hook run from a repo with no DROP_HANDLE actually fetches under
    # the derived handle: the mock logs the URL it was given.
    rm -f "$work/curl.log"
    (
        cd "$work/cleanrepo" || exit 1
        unset DROP_HANDLE
        MOCK_FETCH="$work/fix/inbox_empty.json" MOCK_CURL_LOG="$work/curl.log" \
            bash "$here/hooks/prompt-inject.sh"
    ) >/dev/null 2>&1
    if grep -q "handle=cleanrepo" "$work/curl.log" 2>/dev/null; then
        ok "a hook with no DROP_HANDLE fetches under the derived handle"
    else bad "a hook with no DROP_HANDLE fetches under the derived handle"; fi
}

printf '\n%d checks, %d failed — %s\n' "$((pass + fail))" "$fail" \
    "$([ "$fail" -eq 0 ] && echo "all passed" || echo FAILED)"
[ "$fail" -eq 0 ]
