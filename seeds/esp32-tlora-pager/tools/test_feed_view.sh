#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/tlora-feed-view.XXXXXX")
trap 'rm -f "$OUT"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  "$HERE/test_feed_view.cpp" -o "$OUT"
"$OUT"

# Wiring pin (Bug2a, TLORA-UI-FIX C3): the value-level filter test above proves
# WHAT the filter decides; this slice proves main.cpp's card scan actually MAKES
# that call. Goes RED if the `if (notify_is_chat(v)) continue;` line is removed
# from ui_open_msglist. Same source-slice technique as test_idle_policy.py.
python3 - "$HERE/../src/main.cpp" <<'PYEOF'
import sys
from pathlib import Path

main = Path(sys.argv[1]).read_text(encoding="utf-8")
open_fn = main[main.index("static void ui_open_msglist() {") :]
open_fn = open_fn[: open_fn.index("// ===== Contacts screen =====")]

# Anchor on the DEFINITION (with the brace) — the bare name would bind the
# forward declaration hundreds of lines earlier and pin far too wide a span.
scan = main[main.index("static void ui_open_msglist() {") :]
scan = scan[: scan.index("feed_build_rows(")]
assert "if (!notify_view(i, v)) break;" in scan, (
    "the card scan changed shape; re-anchor this slice"
)
assert "if (notify_is_chat(v)) continue;" in scan, (
    "Bug2a: the feed card scan must skip chat door cards via the C2 "
    "classifier, or a chat shows twice (conversation row + door card)"
)
# View-level only: the scan must not delete or mutate the stored card. (The
# delivery-time ack pinned below lives in the loop() arrival branch, OUTSIDE
# this span, so it does not trip this pin.)
assert "notify_delete" not in scan and "notify_ack" not in scan, (
    "the door-card filter must stay view-level; never touch the store"
)

# C7: the renderer and handle cache expose the whole 12-card + 8-conversation
# merge, carry HH:MM beside every row, and keep the large merge workspaces off
# the loop-task stack.
hw_h = Path(sys.argv[1]).with_name("hw_ui.h").read_text(encoding="utf-8")
hw_cpp = Path(sys.argv[1]).with_name("hw_ui.cpp").read_text(encoding="utf-8")
feed_h = Path(sys.argv[1]).with_name("feed_view.h").read_text(encoding="utf-8")
assert "#define HW_UI_MSGLIST_MAX 20" in hw_h
assert "const char *const *times" in hw_h
assert "msglist_draw_row(const char *const *titles,\n                             const char *const *times" in hw_cpp
assert "tft_draw_text_r(PANEL_W - MARGIN - 4" in hw_cpp
assert "utf8_text_copy(out, out_n, raw, 28, true);" in hw_cpp
assert "static uint32_t msglist_unread_mask" in hw_cpp
assert "i < HW_UI_MSGLIST_MAX" in hw_cpp
for workspace in (
    "static FeedCardView cards[FEED_MAX_CARDS];",
    "static FeedConvView convs[CONV_MAX];",
    "static FeedRow rows[HW_UI_MSGLIST_MAX];",
):
    assert workspace in open_fn, f"loop-stack feed workspace returned: {workspace}"
assert "static FeedSortItem it[FEED_ORDER_MAX];" in feed_h
assert "agents_head_time(r.epoch, times[msglist_count]" in open_fn

# The counterpart of hiding the door card: arrival must use the shared plan and
# ACK only after the thread accepts the normalized line.
chat_arr = main[main.index("static bool notify_reconcile_pending_chats(") :]
chat_arr = chat_arr[: chat_arr.index("static void notify_take_chat_completions(")]
assert "agents_chat_door_enqueue(" in chat_arr
assert "agents_on_inbound(" not in chat_arr and "notify_ack" not in chat_arr
completion = main[main.index("static void notify_take_chat_completions(") :]
completion = completion[: completion.index("static void agents_head_time(")]
assert "done.accepted" in completion and "notify_ack_identity(" in completion, (
    "a chat card must be identity-checked and ACKed only after worker completion"
)
PYEOF
echo "feed view tests: OK"
