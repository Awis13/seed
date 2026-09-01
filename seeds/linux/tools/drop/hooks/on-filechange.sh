#!/usr/bin/env bash
#
# FileChanged hook for $DROP_INBOX_FILE (pairs with "asyncRewake": true):
# exit 2 + stderr surfaces the inbox to the model mid-turn. The watcher wrote
# the file from a peek, so the cursor is untouched and the consuming hooks
# still deliver the same mail.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../drop-common.sh
# shellcheck disable=SC1091
. "$here/../drop-common.sh" || exit 0

[ -f "$DROP_INBOX_FILE" ] || exit 0
cat "$DROP_INBOX_FILE" >&2
exit 2
