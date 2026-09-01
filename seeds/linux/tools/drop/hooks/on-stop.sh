#!/usr/bin/env bash
#
# Stop hook: consuming inbox fetch. Mail that arrived mid-turn blocks the
# stop once, with the messages in the reason; the fetch advanced the cursor,
# so the next stop finds an empty inbox and passes — no loop.
# The block JSON is built by python3 so message text cannot break it.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../drop-common.sh
# shellcheck disable=SC1091
. "$here/../drop-common.sh" || exit 0
drop_configured || exit 0

json="$(drop_fetch_inbox)"
[ -n "$json" ] || exit 0

printf '%s' "$json" | python3 -c "${DROP_PY_FMT}"'
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    raise SystemExit(0)
msgs = d.get("messages") or []
if not msgs:
    raise SystemExit(0)
reason = "NEW DROP MESSAGES for %s: %s. Acknowledge/act, then finish." % (
    d.get("handle", ""), " | ".join(drop_line(m) for m in msgs))
print(json.dumps({"decision": "block", "reason": reason}))
' 2>/dev/null || exit 0
exit 0
