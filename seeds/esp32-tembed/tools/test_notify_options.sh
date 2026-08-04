#!/usr/bin/env bash
#
# Host-side regression test for the reply options in src/skills/notify.cpp:
# what the endpoint accepts, and what survives a reboot.
#
# Why this exists: a notification that carries reply options is asked and
# answered by two different parties at two different times — something posts a
# question over HTTP, a hand answers it on the knob hours later, and the answer
# has to still be there and still mean the same thing. Every way that can go
# wrong is silent. A label the panel has no glyphs for draws as garbage on a
# device with no console. An index restored past the end of the labels it points
# into answers a question nobody asked. A writer and a reader that disagree
# about a two-letter key lose every reply across a reboot and report nothing.
# None of that fails a build, and none of it is visible from the outside.
#
# The slot array is covered for the one thing the restore path can do to it:
# leave a slot carrying an id that never reaches the order list, which is a slot
# no free-slot search hands out again. A file of entries that all fail that way
# fills the queue with nothing and makes POST /notify answer "queue full" on an
# empty device, recoverable only by deleting the file. Refusing an entry is the
# ordinary case here — an older firmware's file, a truncated one, a replaced
# image — so the cost of getting it wrong is paid quietly.
#
# The one part of notify_push() that IS covered is the step where it takes an
# id, which sits in notify_take_id() for exactly that reason: the counter can be
# left at the top of its range by a restored file, and the notification after
# the next one then carries 0, the value that marks a slot free. The rest of
# notify_push() stays where it is.
#
# What this DOES NOT cover, said plainly rather than papered over: the rest of
# the store (spinlocks, eviction, the body of notify_push), the endpoints, the
# screen and the knob. Those need FreeRTOS, a panel and a hand. No coverage is
# claimed for them. tools/test_routes.sh checks the one failure mode the HTTP
# layer here actually has — a route that reads a body without registering a
# collector, and a route registered with a matcher loose enough to swallow its
# neighbours.
#
# The code is sliced straight out of the shipping source between its
# `host-test:begin` and `host-test:end` markers, the same way
# tools/test_progress.sh and tools/test_voice_url.sh slice theirs, so this runs
# the real implementation rather than a copy that could drift.
#
# ArduinoJson is compiled in from the PlatformIO library cache, because the
# snapshot round-trip is only worth anything against the real parser: the point
# is that the writer and the reader agree about the file, and a stand-in would
# only prove they agree about the stand-in. Run `pio run -e tembed` once to
# populate it, or point ARDUINOJSON_DIR somewhere else.
#
# Usage: tools/test_notify_options.sh

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../src/skills/notify.cpp"
main="$here/../src/main.cpp"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

[ -f "$src" ] || { echo "cannot find $src"; exit 1; }
[ -f "$main" ] || { echo "cannot find $main"; exit 1; }

# --- The rule the C tests below can only half reach ---
#
# notify_snapshot_restore() reads the id into the caller's entry BEFORE it knows
# whether the rest of the object is usable, and on the load path that entry IS a
# slot in the store. So every path that returns false has to put the id back to
# 0, or it leaves a slot that notify_free_slot() skips forever and that
# notify_order[] never lists — a queue that reports itself full on an empty
# device, recoverable only by deleting the file.
#
# The refusal that exists today IS covered below: refused_entry() calls the
# function directly and insists the id came back 0, so deleting that one clear
# turns four checks red. What no test below can cover is the NEXT false path
# somebody adds. The load loop clears the id a second time after a refusal —
# belt to this function's braces — so a new path that forgets its own clear
# strands nothing at load time, and no test below thinks to exercise it at all.
# That was checked rather than assumed: a second `return false` added here, with
# the loop's clear left in place, passes the whole C suite clean and is caught
# only by this gate.
#
# Deliberately dumb, like tools/test_routes.sh: it reads text, not C++, and asks
# one question of each `return false` in that function — is there an `e.id = 0`
# on that line or in the few lines above it?
#
# The window is what makes the gate honest. Both of these are correct code:
#
#     if (...) { e.id = 0; return false; }
#
#     if (...) {
#         e.id = 0;
#         return false;
#     }
#
# and a gate that insisted on the first would go red on somebody's correct
# rewrite into the second. A false failure here is worse than no gate: the next
# person to hit it has a passing suite, a red text check they cannot reconcile
# with the code in front of them, and an obvious way to make it go away.
body="$(awk '
    /^static bool notify_snapshot_restore\(/ { inf = 1 }
    inf                                      { print }
    inf && /^}/                              { exit }
' "$src")"

[ -n "$body" ] || { echo "cannot find notify_snapshot_restore() in $src"; exit 1; }

echo "every refusal in notify_snapshot_restore() gives the slot back"
win=3   # how far above a `return false` its clear may sit
leaks="$(printf '%s\n' "$body" | awk -v win="$win" '
    { line[NR] = $0 }
    /return false/ {
        cleared = 0
        first = NR - win
        if (first < 1) first = 1
        for (i = first; i <= NR; i++) if (line[i] ~ /e\.id = 0/) cleared = 1
        if (!cleared) printf "%d:%s\n", NR, $0
    }
')"
if [ -n "$leaks" ]; then
    printf '  FAIL: a path returns false with no `e.id = 0` on it or in the %s lines\n' "$win"
    printf '        above it, so it may strand the slot it was restoring into.\n'
    printf '        Line numbers count from the start of the function:\n'
    printf '%s\n' "$leaks" | sed 's/^/          /'
    exit 1
fi
printf '  ok:   %s refusal path(s), each clearing the id it had read\n' \
    "$(printf '%s\n' "$body" | grep -c 'return false')"

json="${ARDUINOJSON_DIR:-$here/../.pio/libdeps/tembed/ArduinoJson/src}"
if [ ! -f "$json/ArduinoJson.h" ]; then
    echo "cannot find ArduinoJson.h under $json"
    echo "run 'pio run -e tembed' once to fetch it, or set ARDUINOJSON_DIR"
    exit 1
fi

# Read from main.cpp rather than repeated here: the restore path compares stored
# timestamps against it, and a copy that drifts would move that threshold in the
# test only.
epoch=$(grep -E '^#define[[:space:]]+TIME_VALID_EPOCH' "$main" | awk '{print $3}')
[ -n "$epoch" ] || { echo "cannot read TIME_VALID_EPOCH from $main"; exit 1; }

slice() {
    awk -v tag="$2" '
        $0 ~ ("host-test:begin " tag) { grab = 1; next }
        grab && /host-test:end/       { grab = 0; next }
        grab                          { print }
    ' "$1"
}

{
    cat <<PRELUDE
/* Generated by tools/test_notify_options.sh — do not edit. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string>
#include <ArduinoJson.h>

#define TIME_VALID_EPOCH ${epoch}

PRELUDE
    slice "$src" types
    slice "$src" text
    slice "$src" id
    slice "$src" store
    slice "$src" snapshot
    cat <<'MAIN'

/* ---- test scaffolding ---- */

static int failures = 0;

static void check(int cond, const char *what) {
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
    else       { printf("  ok:   %s\n", what); }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (strcmp(got, want) != 0) {
        printf("  FAIL: %s — got \"%s\", wanted \"%s\"\n", what, got, want);
        failures++;
    } else {
        printf("  ok:   %s — \"%s\"\n", what, got);
    }
}

/* One option set that must be accepted. */
static void good_set(const char *const *labels, size_t n, const char *what) {
    NotifyOptions op;
    uint8_t count = 200;
    const char *err = "(not set)";
    if (!notify_options_build(op, count, labels, n, &err)) {
        printf("  FAIL: %s should have been accepted (refused: %s)\n", what, err);
        failures++;
        return;
    }
    if (count != (uint8_t)n) {
        printf("  FAIL: %s — accepted %u labels, was given %u\n", what,
               (unsigned)count, (unsigned)n);
        failures++;
        return;
    }
    printf("  ok:   %s — %u label(s)\n", what, (unsigned)count);
}

/* One option set that must be refused, with a reason and with nothing kept. */
static void bad_set(const char *const *labels, size_t n, const char *what) {
    NotifyOptions op;
    uint8_t count = 200;
    const char *err = NULL;
    if (notify_options_build(op, count, labels, n, &err)) {
        printf("  FAIL: %s should have been refused\n", what);
        failures++;
        return;
    }
    if (!err || !err[0]) {
        printf("  FAIL: %s was refused with no reason\n", what);
        failures++;
        return;
    }
    if (count != 0) {
        printf("  FAIL: %s was refused but left %u labels behind\n", what,
               (unsigned)count);
        failures++;
        return;
    }
    printf("  ok:   %s refused — %s\n", what, err);
}

/* One label on its own, which is how most of the refusals below read. */
static void bad_label(const char *label, const char *what) {
    const char *labels[1] = { label };
    bad_set(labels, 1, what);
}

/* An entry with nothing in it but the two fields a restore insists on. */
static void blank_entry(Notification &e, NotifyOptions &op) {
    memset(&e, 0, sizeof(e));
    memset(&op, 0, sizeof(op));
    e.id = 1;
    e.chosen = -1;
    snprintf(e.title, sizeof(e.title), "Deploy to prod?");
}

/* Write one entry to JSON exactly as notify_save() does, and read it back
   exactly as notify_load() does. `now` is the wall clock both halves see. */
static bool round_trip(Notification &in, NotifyOptions &in_op,
                       Notification &out, NotifyOptions &out_op,
                       time_t now, std::string *text) {
    JsonDocument doc;
    notify_snapshot_store(doc.to<JsonObject>(), in, in_op, now);
    std::string json;
    serializeJson(doc, json);
    if (text) *text = json;

    JsonDocument back;
    if (deserializeJson(back, json) != DeserializationError::Ok) {
        printf("  FAIL: the snapshot did not parse: %s\n", json.c_str());
        failures++;
        return false;
    }
    return notify_snapshot_restore(back.as<JsonObjectConst>(), out, out_op, now, 500000);
}

/* Restore straight from a literal, for files this firmware would never write
   but is entitled to be handed: an older version's, or a replaced image. */
static bool restore_text(const char *json, Notification &out, NotifyOptions &out_op,
                         time_t now) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        printf("  FAIL: test JSON did not parse: %s\n", json);
        failures++;
        return false;
    }
    return notify_snapshot_restore(doc.as<JsonObjectConst>(), out, out_op, now, 500000);
}

/* One stored object that must be refused AND must leave nothing behind.
 *
 * The second half is the part with teeth. notify_load() restores straight into
 * a slot, so a refusal that leaves a non-zero id there leaves a slot that is
 * neither free — notify_free_slot() skips it — nor in notify_order[]. The
 * output is pre-filled with rubbish so that "id 0" means the restore cleared
 * it rather than that it was never written. */
static void refused_entry(const char *json, time_t now, const char *what) {
    Notification out;
    NotifyOptions out_op;
    memset(&out, 'X', sizeof(out));
    memset(&out_op, 'X', sizeof(out_op));

    if (restore_text(json, out, out_op, now)) {
        printf("  FAIL: %s should have been refused\n", what);
        failures++;
        return;
    }
    if (out.id != 0) {
        printf("  FAIL: %s was refused but left id %lu in the slot, which no "
               "free-slot search will hand out again\n",
               what, (unsigned long)out.id);
        failures++;
        return;
    }
    printf("  ok:   %s refused, and its slot left free\n", what);
}

/* The store as setup() finds it, so each load below starts from a clean one. */
static void store_reset(void) {
    memset(notify_slot, 0, sizeof(notify_slot));
    memset(notify_opt, 0, sizeof(notify_opt));
    memset(notify_order, 0, sizeof(notify_order));
    notify_len = 0;
    notify_next_id = 1;
}

/* A whole stored file through the loop notify_load() runs, which is the level
   the slot leak is visible at. Returns how many entries were restored. */
static int load_text(const char *json, time_t now) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        printf("  FAIL: test JSON did not parse: %s\n", json);
        failures++;
        return -1;
    }
    return notify_restore_entries(doc["n"].as<JsonArrayConst>(), now, 500000);
}

static int slots_occupied(void) {
    int n = 0;
    for (int i = 0; i < NOTIFY_MAX; i++) if (notify_slot[i].id != 0) n++;
    return n;
}

/* One id as ?id= carries it, which must be accepted and must come back whole. */
static void good_id(const char *s, uint32_t want) {
    uint32_t got = 12345;
    if (!notify_parse_id(s, got)) {
        printf("  FAIL: \"%s\" should have been accepted\n", s);
        failures++;
        return;
    }
    if (got != want) {
        printf("  FAIL: \"%s\" parsed as %lu, wanted %lu\n", s,
               (unsigned long)got, (unsigned long)want);
        failures++;
        return;
    }
    printf("  ok:   \"%s\" is id %lu\n", s, (unsigned long)got);
}

/* And one that must be refused with the output left alone: the handler answers
   400 on false and must not have had its id quietly moved first. */
static void bad_id(const char *s, const char *what) {
    uint32_t got = 12345;
    if (notify_parse_id(s, got)) {
        printf("  FAIL: %s (\"%s\") should have been refused, got %lu\n", what,
               s ? s : "(null)", (unsigned long)got);
        failures++;
        return;
    }
    if (got != 12345) {
        printf("  FAIL: %s (\"%s\") was refused but wrote %lu out anyway\n", what,
               s ? s : "(null)", (unsigned long)got);
        failures++;
        return;
    }
    printf("  ok:   %s refused\n", what);
}

int main(void) {
    /* Any hang is a failure, not a hung test run. */
    alarm(60);

    /* Comfortably past TIME_VALID_EPOCH, so ages are computed from the wall
       clock the way they are on a device whose NTP sync has landed. */
    const time_t now = (time_t)TIME_VALID_EPOCH + 1000000;

    printf("how many options a notification may carry\n");
    {
        const char *one[]  = { "OK" };
        const char *two[]  = { "Yes", "No" };
        const char *four[] = { "Yes", "No", "Later", "Never" };
        const char *five[] = { "Yes", "No", "Later", "Never" };  /* a fifth is not read */

        good_set(NULL, 0, "no options at all");
        good_set(one, 1, "a single option");
        good_set(two, 2, "two options");
        good_set(four, NOTIFY_OPT_MAX, "four options, the maximum");
        /* The count is what is refused, and it is refused before the labels are
           touched — which is what lets the endpoint pass the size it saw while
           filling only the pointers that fit. */
        bad_set(five, NOTIFY_OPT_MAX + 1, "five options");
        bad_set(NULL, 99, "ninety-nine options, without reading any of them");
    }

    printf("what a label may contain\n");
    {
        bad_label(NULL, "a null label");
        bad_label("", "an empty label");
        bad_label("   ", "a label of nothing but spaces");
        /* The one this firmware has already been bitten by. These bytes are a
           Cyrillic word in UTF-8; the panel's compiled fonts cover 32..126 and
           would draw them as garbage with no error raised anywhere, so they are
           refused while somebody is still holding the curl command. */
        bad_label("\xd0\xb4\xd0\xb0", "a Cyrillic label");
        bad_label("caf\xc3\xa9", "an accented character");
        bad_label("a\nb", "an embedded newline");
        bad_label("a\rb", "an embedded carriage return");
        bad_label("a\tb", "an embedded tab");
        bad_label("a\x01" "b", "a control character");
        bad_label("a\x7f" "b", "DEL, which is above the printable range");
        /* Blank AFTER the cut, which is the only way the two rules interact:
           the printable character that carries this label past the check is
           also the one the cut throws away, leaving fifteen spaces to draw as a
           chip and to serve back over GET /notify. Refused on what is stored,
           not on what was typed. */
        bad_label("               XYZ", "a label whose only printable characters are cut off");
        {
            const char *kept[] = { "              X" };
            good_set(kept, 1, "and one whose printable character survives the cut");
        }

        const char *ok[] = { "Yes please", "no-2", "~!@#$%^&*()" };
        good_set(ok, 3, "spaces, digits, punctuation");

        /* One bad label refuses the whole set rather than dropping it and
           renumbering the rest: `chosen` is an index into this array. */
        const char *mixed[] = { "Yes", "\xd0\xb4\xd0\xb0", "No" };
        bad_set(mixed, 3, "one bad label among good ones");
    }

    printf("an over-long label is cut, not refused\n");
    {
        /* Losing the tail of a chip is cosmetic; refusing the notification over
           it would lose the message. */
        const char *labels[] = { "Restart everything now" };
        NotifyOptions op;
        uint8_t count = 0;
        check(notify_options_build(op, count, labels, 1, NULL),
              "a label longer than the field is accepted");
        check(strlen(op.label[0]) == NOTIFY_OPT_LEN - 1, "and cut to the field width");
        check_str(op.label[0], "Restart everyth", "the label it was cut to");
        /* Deliberately checked, because a UTF-8-aware cut must not shorten pure
           ASCII by an extra byte "to be safe". */
        check(strncmp(op.label[0], labels[0], NOTIFY_OPT_LEN - 1) == 0,
              "and the part that fits is exactly what was sent");
    }

    printf("the cut lands on a character, not on a byte\n");
    {
        /* Option labels never reach this with multi-byte input — the ASCII
           check refuses them first — but titles and bodies do, and this is the
           copy all of them share. A cut through the middle of a sequence leaves
           a fragment that GET /notify hands out verbatim. */
        char dst[8];   /* seven bytes fit */
        /* Six ASCII and then a two-byte character: only its first byte would
           fit, so the whole character goes rather than half of it. */
        notify_copy_text(dst, sizeof(dst), "aaaaaa\xd0\xb4");
        check_str(dst, "aaaaaa", "a two-byte character that does not fit is dropped whole");
        /* Four ASCII and then two of them: the first fits entirely and stays,
           the second does not and goes. The cut is per character, not a blanket
           retreat to the last ASCII byte. */
        notify_copy_text(dst, sizeof(dst), "aaaa\xd0\xb4\xd0\xb0");
        check_str(dst, "aaaa\xd0\xb4", "and the one before it is kept whole");
        notify_copy_text(dst, sizeof(dst), "aaaaaaaaaa");
        check_str(dst, "aaaaaaa", "and pure ASCII fills the field to the last byte");
        notify_copy_text(dst, sizeof(dst), "");
        check_str(dst, "", "an empty string copies to an empty field");
    }

    printf("a snapshot written by this firmware, read back by it\n");
    {
        Notification in, out;
        NotifyOptions in_op, out_op;
        blank_entry(in, in_op);
        in.id = 12;
        in.level = NOTIFY_WARN;
        in.unread = false;
        in.ttl_s = 3600;
        in.created_epoch = now - 60;
        snprintf(in.source, sizeof(in.source), "claude");
        snprintf(in.body, sizeof(in.body), "the staging run is green");
        snprintf(in.key, sizeof(in.key), "deploy");
        const char *labels[] = { "Yes", "No", "Later" };
        check(notify_options_build(in_op, in.opt_count, labels, 3, NULL),
              "an entry with three options is built");
        in.chosen = 1;

        std::string text;
        check(round_trip(in, in_op, out, out_op, now, &text), "it restores");
        printf("  ..   %s\n", text.c_str());
        check(out.id == 12, "the id survives");
        check(out.level == NOTIFY_WARN, "the level survives");
        check(out.unread == false, "the read flag survives");
        check(out.ttl_s == 3600, "the ttl survives");
        check_str(out.source, "claude", "the source survives");
        check_str(out.title, "Deploy to prod?", "the title survives");
        check_str(out.body, "the staging run is green", "the body survives");
        check_str(out.key, "deploy", "the client key survives");
        check(out.opt_count == 3, "all three options survive");
        check_str(out_op.label[0], "Yes", "the first label");
        check_str(out_op.label[1], "No", "the second label");
        check_str(out_op.label[2], "Later", "the third label");
        /* The whole point of the commit: the answer is still there after the
           power cut that the message was warning about. */
        check(out.chosen == 1, "and so does the reply");
        check(out.created_epoch == in.created_epoch, "the creation time survives");
    }

    printf("a snapshot outlives the entries it was taken from\n");
    {
        /* notify_save() copies each entry out of the store into a stack local
           and serialises the document afterwards, so by then every one of those
           locals is gone. ArduinoJson keeps a `const char *` by pointer and
           copies a `char *`; if the writer ever takes its entry by const
           reference, this test is what notices. */
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < 3; i++) {
            Notification e;
            NotifyOptions op;
            blank_entry(e, op);
            e.id = (uint32_t)(i + 1);
            snprintf(e.title, sizeof(e.title), "message %d", i);
            const char *labels[] = { "Yes", "No" };
            notify_options_build(op, e.opt_count, labels, 2, NULL);
            notify_snapshot_store(arr.add<JsonObject>(), e, op, now);
            /* Exactly what the next pass of the loop does to the same stack. */
            memset(&e, 'X', sizeof(e));
            memset(&op, 'X', sizeof(op));
        }
        std::string json;
        serializeJson(doc, json);
        check(json.find("message 0") != std::string::npos, "the first title is in the file");
        check(json.find("message 1") != std::string::npos, "and the second");
        check(json.find("message 2") != std::string::npos, "and the third");
        check(json.find("XXX") == std::string::npos, "and no dead stack came with them");
    }

    printf("an entry with no options\n");
    {
        Notification in, out;
        NotifyOptions in_op, out_op;
        blank_entry(in, in_op);
        std::string text;
        check(round_trip(in, in_op, out, out_op, now, &text), "restores");
        check(text.find("\"op\"") == std::string::npos,
              "and carries no options array in the file");
        check(text.find("\"ch\"") == std::string::npos, "and no reply either");
        check(out.opt_count == 0, "and comes back with nothing to answer");
        check(out.chosen == -1, "and unanswered rather than answered with zero");
    }

    printf("what a restore refuses outright, and what it leaves behind\n");
    {
        /* Every one of these is checked twice: refused, and refused without
           leaving an id in the entry. The entry IS the slot on the load path,
           and an id left in a slot that is then skipped strands it. */
        refused_entry("{\"ti\":\"no id\"}", now, "an entry with no id");
        refused_entry("{\"id\":0,\"ti\":\"zero id\"}", now,
                      "an entry whose id is 0, which is how a free slot is marked");
        refused_entry("{\"id\":5}", now, "an entry with no title");
        refused_entry("{\"id\":5,\"ti\":7}", now,
                      "an entry whose title is not a string");
        refused_entry("{\"id\":5,\"ti\":null}", now, "an entry whose title is null");
        refused_entry("{}", now, "an empty object");
        /* An id it could read, and a title it could not: the shape that leaks,
           because the id is stored before the title is looked at. */
        refused_entry("{\"id\":4294967295,\"op\":[\"Yes\"]}", now,
                      "an entry with a large id and no title at all");
    }

    printf("a stored file that is refused entry by entry\n");
    {
        /* The failure this section exists for. Every entry carries an id and
           none carries a title, so all twenty are refused — and if a refusal
           leaves its id in the slot, twenty slots are occupied by nothing,
           notify_free_slot() returns -1, and POST /notify answers 500 "queue
           full" on an empty queue until somebody deletes the file. */
        store_reset();
        int restored = load_text(
            "{\"n\":[{\"id\":1},{\"id\":2},{\"id\":3},{\"id\":4},{\"id\":5},"
            "{\"id\":6},{\"id\":7},{\"id\":8},{\"id\":9},{\"id\":10},"
            "{\"id\":11},{\"id\":12},{\"id\":13},{\"id\":14},{\"id\":15},"
            "{\"id\":16},{\"id\":17},{\"id\":18},{\"id\":19},{\"id\":20}]}", now);
        check(restored == 0, "a file of twenty titleless entries restores none of them");
        check(notify_len == 0, "and puts nothing in the list");
        check(slots_occupied() == 0, "and leaves every slot free");
        check(notify_free_slot() == 0, "so the next notification still has somewhere to go");

        /* The milder version, which leaks one slot per bad entry and is the one
           that would ship unnoticed. */
        store_reset();
        restored = load_text("{\"n\":[{\"id\":1,\"ti\":\"first\"},{\"id\":2},"
                             "{\"id\":3,\"ti\":\"third\"}]}", now);
        check(restored == 2, "a file with one bad entry among good ones restores the good ones");
        check(notify_len == 2, "and lists exactly those");
        check(slots_occupied() == 2, "and occupies exactly two slots");
        check_str(notify_slot[notify_order[0]].title, "first", "the first, in file order");
        check_str(notify_slot[notify_order[1]].title, "third", "then the third");
        check(notify_next_id == 4, "and the next id issued clears the highest restored");

        /* The branch that was already right, pinned so it stays right: a
           duplicate is dropped AND gives its slot back. */
        store_reset();
        restored = load_text("{\"n\":[{\"id\":1,\"ti\":\"first\"},"
                             "{\"id\":1,\"ti\":\"the same one twice\"}]}", now);
        check(restored == 1, "a file carrying the same id twice restores it once");
        check(slots_occupied() == 1, "and gives the duplicate's slot back");
        check_str(notify_slot[notify_order[0]].title, "first",
                  "keeping the copy that came first");

        /* And a genuinely full store still reads as full, so the fix did not
           buy a free slot by losing an entry. */
        store_reset();
        char full[1024];
        int at = snprintf(full, sizeof(full), "{\"n\":[");
        for (int i = 1; i <= NOTIFY_MAX; i++)
            at += snprintf(full + at, sizeof(full) - at, "%s{\"id\":%d,\"ti\":\"m%d\"}",
                           i > 1 ? "," : "", i, i);
        snprintf(full + at, sizeof(full) - at, "]}");
        restored = load_text(full, now);
        check(restored == NOTIFY_MAX, "a full file restores every entry");
        check(slots_occupied() == NOTIFY_MAX, "and occupies every slot");
        check(notify_free_slot() == -1, "which is the one case that has no free slot");
        store_reset();
    }

    printf("a stored id that would wrap the id counter\n");
    {
        /* notify_next_id is unsigned and the load loop winds it one past the
           highest id it restored, so an id of 0xFFFFFFFF in the file leaves it
           at 0 — and notify_push() hands out notify_next_id++. The next
           notification is then queued carrying id 0, which is what marks a slot
           free: POST /notify answers 500 "queue full" for a message it did
           queue, the slot reads as free, the one after it lands on top of it and
           is published into notify_order[] twice. Nothing on this device ever
           issues such an id, but the file it comes back from can be replaced
           wholesale by anyone who can write a filesystem image. */
        store_reset();
        int restored = load_text(
            "{\"n\":[{\"id\":4294967295,\"ti\":\"from a replaced snapshot\"}]}", now);
        check(restored == 1, "an entry carrying the largest possible id still restores");
        check(notify_next_id != 0, "and cannot leave the next id at 0, which marks a slot free");
        check(notify_slot[notify_order[0]].id <= NOTIFY_ID_MAX,
              "the id it is restored under is inside the range an id can take");

        /* One below the wrap is an ordinary id and is left exactly alone: the
           clamp must not cost the range it is protecting. */
        store_reset();
        restored = load_text("{\"n\":[{\"id\":4294967294,\"ti\":\"the largest id there is\"}]}", now);
        check(restored == 1, "the largest id that is not the wrap restores");
        check(notify_slot[notify_order[0]].id == NOTIFY_ID_MAX, "under its own id");
        check(notify_next_id != 0, "and still leaves an id counter clear of 0");

        /* And a clamp that lands on an id already in the store is dropped by the
           duplicate check rather than restored twice under one id. */
        store_reset();
        restored = load_text("{\"n\":[{\"id\":4294967294,\"ti\":\"first\"},"
                             "{\"id\":4294967295,\"ti\":\"clamps onto the first\"}]}", now);
        check(restored == 1, "a clamp landing on a restored id is dropped as the duplicate it became");
        check(slots_occupied() == 1, "and gives its slot back");
        check_str(notify_slot[notify_order[0]].title, "first", "keeping the entry that was there");
        store_reset();
    }

    printf("the id handed to the next notification\n");
    {
        /* The clamp above is only half of it, and the weaker half. A snapshot
           restored at the ceiling leaves the counter at 0xFFFFFFFF, which is
           still a perfectly usable id — so the first notification after the
           restore is fine and the SECOND one carries 0, the value that marks a
           slot free. Clamping the restore alone does not prevent that; it
           delays it by exactly one message.
           notify_push() cannot be sliced onto the host — spinlocks, eviction,
           struct copies — so the counter step it runs lives in
           notify_take_id() and is called here directly. Everything else about
           notify_push() is unchanged by that: the counter still starts at 1 and
           is still read and incremented once per notification. */
        store_reset();
        int restored = load_text(
            "{\"n\":[{\"id\":4294967294,\"ti\":\"at the ceiling of the range\"}]}", now);
        check(restored == 1, "an entry restored at the ceiling of the id range");
        uint32_t first  = notify_take_id();
        uint32_t second = notify_take_id();
        check(first != 0, "leaves the next notification an id that does not mark its slot free");
        check(second != 0, "and the one after it too, which is where the counter runs out");

        /* Where the roll lands, said outright: the bottom of the range, not
           one step past the top of it. */
        store_reset();
        notify_next_id = NOTIFY_ID_MAX;
        check(notify_take_id() == NOTIFY_ID_MAX, "the largest id there is gets issued");
        check(notify_take_id() == 1, "and the counter rolls back to 1 rather than to 0");

        /* And the ordinary succession, which the guard must not have cost: a
           device that has just booted counts up from 1. */
        store_reset();
        check(notify_take_id() == 1, "a fresh device issues id 1 first");
        check(notify_take_id() == 2, "then 2");
        check(notify_take_id() == 3, "then 3");
        store_reset();
    }

    printf("the id in /notify/one?id=N\n");
    {
        good_id("12", 12);
        good_id("1", 1);
        good_id("4294967294", NOTIFY_ID_MAX);
        /* Leading zeroes are still just digits; nothing here is octal. */
        good_id("0012", 12);

        bad_id(NULL, "no parameter at all");
        bad_id("", "an empty parameter");
        bad_id("0", "zero, which is what marks a slot free");
        bad_id("000", "zero written the long way");
        bad_id("twelve", "a word");
        bad_id("12x", "trailing rubbish");
        bad_id("0x10", "hexadecimal");
        bad_id("1.5", "a decimal point");
        /* strtoul() would have taken all four of these: it skips leading
           whitespace, accepts a sign, and negates rather than refusing — so
           "-1" would have looked up notification 4294967295 and answered 404
           to what is plainly a malformed request. */
        bad_id(" 12", "a leading space");
        bad_id("\t12", "a leading tab");
        bad_id("+12", "a leading plus");
        bad_id("-1", "a negative id");
        bad_id("-4294967295", "a negative id that would have wrapped into range");
        /* The ceiling is the store's, not the type's. 4294967295 fits a
           uint32_t and is refused anyway: no notification can carry it — the
           id counter is never left there — so asking for it is a malformed
           request, and answering 404 would say "not here yet" about a value
           that can never be here. */
        bad_id("4294967295", "the one id above the store's ceiling");
        /* And past the range of an id rather than truncated into some other
           entry's: 4294967297 & 0xFFFFFFFF is 1. */
        bad_id("4294967296", "one past what a uint32_t holds");
        bad_id("4294967297", "a value that would truncate onto id 1");
        bad_id("99999999999999999999999999", "a value past every integer here");
    }

    printf("a stored reply that points nowhere\n");
    {
        Notification out;
        NotifyOptions out_op;

        /* The file can be replaced wholesale by anyone who can write a
           filesystem image, so an index out of range is not a theory. */
        check(restore_text("{\"id\":5,\"ti\":\"q\",\"op\":[\"Yes\",\"No\"],\"ch\":7}",
                           out, out_op, now), "an entry answered past its last option");
        check(out.opt_count == 2, "keeps its options");
        check(out.chosen == -1, "and comes back unanswered");

        check(restore_text("{\"id\":5,\"ti\":\"q\",\"op\":[\"Yes\",\"No\"],\"ch\":2}",
                           out, out_op, now), "an entry answered one past the end");
        check(out.chosen == -1, "is unanswered too");

        check(restore_text("{\"id\":5,\"ti\":\"q\",\"op\":[\"Yes\",\"No\"],\"ch\":-5}",
                           out, out_op, now), "an entry with a negative reply");
        check(out.chosen == -1, "is unanswered rather than negative");

        check(restore_text("{\"id\":5,\"ti\":\"q\",\"op\":[\"Yes\",\"No\"],\"ch\":\"1\"}",
                           out, out_op, now), "an entry whose reply is a string");
        check(out.chosen == -1, "is unanswered");

        check(restore_text("{\"id\":5,\"ti\":\"q\",\"ch\":1}", out, out_op, now),
              "a reply with no options beside it");
        check(out.opt_count == 0 && out.chosen == -1, "is dropped with them");

        /* The boundary from the other side: the last valid index is kept. */
        check(restore_text("{\"id\":5,\"ti\":\"q\",\"op\":[\"Yes\",\"No\"],\"ch\":1}",
                           out, out_op, now), "an entry answered with its last option");
        check(out.chosen == 1, "keeps the answer");
    }

    printf("a stored option set that could not have been posted\n");
    {
        Notification out;
        NotifyOptions out_op;

        /* More labels than the array can hold. The bound is checked before any
           of them is read, which is the same check the endpoint makes. */
        check(restore_text("{\"id\":5,\"ti\":\"q\","
                           "\"op\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\"],\"ch\":1}",
                           out, out_op, now), "six options restores as an entry");
        check(out.opt_count == 0, "with no options at all");
        check(out.chosen == -1, "and no reply");
        check_str(out.title, "q", "but the message itself is kept");

        /* A label the panel cannot draw, arriving through the file rather than
           through the endpoint. Same validator, same answer. */
        check(restore_text("{\"id\":5,\"ti\":\"q\",\"op\":[\"Yes\",\"\xd0\xb4\xd0\xb0\"],\"ch\":0}",
                           out, out_op, now), "a non-ASCII label restores as an entry");
        check(out.opt_count == 0, "with the whole set dropped");
        check(out.chosen == -1, "and the reply dropped with it");

        check(restore_text("{\"id\":5,\"ti\":\"q\",\"op\":[\"Yes\",\"\"]}",
                           out, out_op, now), "an empty label restores as an entry");
        check(out.opt_count == 0, "with the set dropped");

        check(restore_text("{\"id\":5,\"ti\":\"q\",\"op\":[\"Yes\",7]}",
                           out, out_op, now), "a label that is not a string");
        check(out.opt_count == 0, "drops the set rather than the message");

        check(restore_text("{\"id\":5,\"ti\":\"q\",\"op\":\"Yes\"}",
                           out, out_op, now), "an options field that is not an array");
        check(out.opt_count == 0, "is ignored");

        check(restore_text("{\"id\":5,\"ti\":\"q\",\"op\":[]}", out, out_op, now),
              "an empty options array");
        check(out.opt_count == 0 && out.chosen == -1, "leaves a plain notification");

        /* And an over-long stored label is cut rather than dropped, exactly as
           the endpoint would have cut it. */
        check(restore_text("{\"id\":5,\"ti\":\"q\",\"op\":[\"Restart everything now\"]}",
                           out, out_op, now), "an over-long stored label");
        check(out.opt_count == 1, "keeps its set");
        check_str(out_op.label[0], "Restart everyth", "cut to the field width");
    }

    printf("the clamps that were here before options were\n");
    {
        Notification out;
        NotifyOptions out_op;

        check(restore_text("{\"id\":5,\"ti\":\"q\",\"lv\":9}", out, out_op, now),
              "an entry with a level past crit");
        check(out.level == NOTIFY_INFO, "comes back as info rather than as 9");

        char buf[128];
        snprintf(buf, sizeof(buf), "{\"id\":5,\"ti\":\"q\",\"tt\":%lu}",
                 (unsigned long)NOTIFY_TTL_MAX + 1);
        check(restore_text(buf, out, out_op, now), "an entry with a ttl past the ceiling");
        check(out.ttl_s == NOTIFY_TTL_MAX, "is capped at the ceiling");

        check(restore_text("{\"id\":5,\"ti\":\"q\",\"sr\":\"\xd0\xb4\xd0\xb0\xd0\xb4\xd0\xb0\"}",
                           out, out_op, now), "a source of multi-byte characters");
        check(strlen(out.source) < NOTIFY_SOURCE_LEN, "is cut to the field");
    }

    printf("an older file, and a newer one\n");
    {
        Notification out;
        NotifyOptions out_op;

        /* Written before this commit existed: no `op`, no `ch`. Nothing here
           insists on them. */
        check(restore_text("{\"id\":5,\"lv\":2,\"ur\":true,\"tt\":0,\"ts\":0,"
                           "\"sr\":\"home-rig\",\"ti\":\"RAID degraded\"}",
                           out, out_op, now), "a snapshot from before reply options");
        check(out.opt_count == 0 && out.chosen == -1,
              "restores as the plain notification it is");
        check_str(out.title, "RAID degraded", "with everything it did carry");

        /* And the other direction: keys this firmware does not know are
           ignored rather than fatal, which is what makes a downgrade safe. */
        check(restore_text("{\"id\":5,\"ti\":\"q\",\"zz\":{\"nested\":[1,2]},"
                           "\"op\":[\"Yes\"],\"ch\":0}", out, out_op, now),
              "a snapshot carrying fields from some later version");
        check(out.opt_count == 1 && out.chosen == 0, "restores everything it does know");
    }

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
MAIN
} > "$work/test.cpp"

cxx="${CXX:-c++}"
"$cxx" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -I "$json" \
    -o "$work/test" "$work/test.cpp"
"$work/test"
