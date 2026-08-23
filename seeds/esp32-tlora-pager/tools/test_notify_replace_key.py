#!/usr/bin/env python3
"""Replace-by-key must stay on for ordinary keyed cards.

A card posted twice under the same client `id` has to UPDATE the card that is
already on the pager, not stack a second one. Everything the device does with
recurring alerts rests on that: the pills reminder re-pages hourly under the
stable key `pills-morning`, watchers re-post their state under their own keys,
and each is meant to land as one row that changes.

This is pinned as source shape because the regression it guards was a single
flipped comparison that nothing else could see. 5b72125 changed

    return resolution.conversation >= 0;   ->   return resolution.conversation < 0;

in notify_event_distinct_cb, which reads "everything that is NOT a conversation
is a distinct event". event_distinct switches replace-by-key OFF, so every
ordinary keyed card became un-replaceable. Measured on the live device before
the fix: ten unread CRIT "PILLS OVERDUE" cards from one rule, the store at
40/40, and crit_unread consequently holding the backlight at dim forever -- the
panel had stopped blanking altogether. Two POSTs under one id produced two
cards (408 and 409) instead of one.

The flag means "one chat event, never a key update". So it must name the chat
events and nothing else.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
notify = (ROOT / "src" / "skills" / "notify.cpp").read_text(encoding="utf-8")
record = (ROOT / "src" / "micron" / "notify_record.h").read_text(encoding="utf-8")

# 1. The gate itself: push replaces on a key only while the card is not distinct.
#    If this line changes shape, the rest of this test is checking nothing.
assert "if (notify_rec_key_replaces(e.key) && !e.event_distinct) {" in notify, (
    "notify_push must gate replace-by-key on the key being present and the card "
    "not being a distinct event"
)

# 2. A non-empty key is replaceable. Nothing narrower may creep in here: the
#    predicate is what makes `id` mean "update this card".
body = record[record.index("static inline int notify_rec_key_replaces("):]
body = body[: body.index("\n}") + 2]
assert "return key && key[0];" in body, (
    "any non-empty client key must be replaceable"
)

# 3. THE REGRESSION. Distinct names chat events, so the resolution test must be
#    >= 0. A `< 0` here silently disables replacement for every ordinary card.
cb = main[main.index("static bool notify_event_distinct_cb(const char *source, const char *key) {"):]
cb = cb[: cb.index("\n}") + 2]
assert "return resolution.conversation >= 0;" in cb, (
    "a card is a distinct event when it IS a chat event; reading it as "
    "'not a conversation' turns replace-by-key off for meds/watcher cards"
)
assert "resolution.conversation < 0" not in cb, (
    "the inverted comparison is the 5b72125 regression -- see this file's docstring"
)

# 4. A chat door is one doorbell and must keep replacing, whatever the
#    conversation lookup says, so its early return stays ahead of the lookup.
door = cb[: cb.index("agents_notify_chat_resolve_snapshot")]
assert "if (notify_rec_is_chat_door_key(key)) return false;" in door, (
    "the chat-door guard must short-circuit before the conversation lookup"
)

# 5. The HTTP ingest still takes a STRING `id` as the dedup key. If this stops
#    being read, every card arrives keyless and stacks no matter what (2)-(4) say.
assert re.search(
    r'if \(input\["id"\]\.is<const char\*>\(\)\)\s*\n\s*notify_copy_text\(e\.key,',
    notify,
), "POST /notify must keep taking the string id as the card's dedup key"

print("notify replace-by-key tests: OK")
sys.exit(0)
