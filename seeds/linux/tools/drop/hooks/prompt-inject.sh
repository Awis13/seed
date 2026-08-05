#!/usr/bin/env bash
#
# UserPromptSubmit hook: consuming inbox fetch on every prompt. Prints only
# when there is mail; anything else is silence. Must stay fast — the hook
# budget is 30s and curl is capped at 3.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../drop-common.sh
# shellcheck disable=SC1091
. "$here/../drop-common.sh" || exit 0
drop_configured || exit 0

lines="$(drop_fetch_inbox | drop_render_lines)"
[ -n "$lines" ] || exit 0

printf 'NEW DROP MESSAGES for %s:\n%s\n' "$DROP_HANDLE" "$lines"
exit 0
