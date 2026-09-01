#!/usr/bin/env bash
#
# SessionStart hook: consuming inbox fetch — a fresh session starts with its
# drop mail already in context. Empty inbox or unreachable server: silence.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../drop-common.sh
# shellcheck disable=SC1091
. "$here/../drop-common.sh" || exit 0
drop_configured || exit 0

lines="$(drop_fetch_inbox | drop_render_lines)"
[ -n "$lines" ] || exit 0

printf 'DROP INBOX for %s — messages from other sessions:\n%s\n' \
    "$DROP_HANDLE" "$lines"
exit 0
