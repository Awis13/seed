#!/usr/bin/env bash
#
# Wiring gate: every route that READS a request body must REGISTER a body
# handler.
#
# Why this exists, precisely. 0.8.0 shipped POST /voice like this:
#
#     server.on(AsyncURIMatcher::exact("/voice"), HTTP_POST, [](...) {
#         char *body = notify_take_body(req);
#         ...
#     });
#
# `server.on(uri, method, onRequest)` registers no body handler, so
# handle_body_collect() never runs, `_tempObject` is never filled, and
# notify_take_body() returns NULL on every single request. The endpoint then
# answered `400 {"error":"body must be JSON"}` to every POST — including
# perfectly good ones. The feature was unreachable and the error message
# blamed the client.
#
# What makes that defect worth a gate of its own is that NOTHING ELSE CATCHES
# IT. It compiles: the three-argument overload is valid C++. The host tests
# pass: they slice out pure functions and the parser was never at fault. The
# unit under test was fine and the wiring around it was not, which is a shape
# no amount of testing the unit will ever find.
#
# So this checks the wiring, statically, from the source. It is deliberately
# dumb — it reads text, not C++ — and it only has to be right about one
# question: does a handler that reaches for a body also register the thing that
# fills it?
#
# Usage: tools/test_routes.sh

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../src"

[ -d "$src" ] || { echo "cannot find $src"; exit 1; }

fail=0

# --- Part 1: the inline-lambda registrations, which is every skill route ---
#
# Walk each `server.on(` ... `});` span and ask two questions of it: does the
# body of the handler read a request body, and does the closing line pass a
# body handler? Anything where the first is true and the second is false is the
# 0.8.0 defect.
#
# A span ends at the first line that closes the registration at the same
# indentation the skills all use: `    });` or `    }, NULL, something);`.
awk '
    /server\.on\(/ {
        inreg = 1
        start = FNR
        route = $0
        sub(/.*exact\("/, "", route)
        sub(/".*/, "", route)
        method = ($0 ~ /HTTP_POST/) ? "POST" : (($0 ~ /HTTP_GET/) ? "GET" : "?")
        reads = 0
        # A one-line registration with a named handler is Part 2s job.
        if ($0 ~ /\);[[:space:]]*$/) { inreg = 0; next }
        next
    }
    inreg && (/notify_take_body/ || /_tempObject/) { reads = 1 }
    inreg && /^[[:space:]]*\}[,)]/ {
        collects = ($0 ~ /,[[:space:]]*NULL[[:space:]]*,[[:space:]]*[A-Za-z_]+\)/) ? 1 : 0
        printf "%s\t%s\t%s\t%d\t%d\t%d\n", FILENAME, route, method, start, reads, collects
        inreg = 0
    }
' "$src"/main.cpp "$src"/skills/*.cpp > /tmp/seed_routes.$$

echo "routes registered with an inline handler"
while IFS=$'\t' read -r file route method line reads collects; do
    short="${file##*/}"
    if [ "$reads" = "1" ] && [ "$collects" = "0" ]; then
        echo "  FAIL: $method $route ($short:$line) reads a body but registers no body handler"
        fail=$((fail + 1))
    elif [ "$reads" = "1" ]; then
        echo "  ok:   $method $route reads a body and collects one"
    else
        echo "  ok:   $method $route takes no body"
    fi
done < /tmp/seed_routes.$$
rm -f /tmp/seed_routes.$$

# --- Part 2: the one-line registrations with named handlers ---
#
# main.cpp registers a few routes as `server.on(uri, method, handler, ...)` on
# a single line, so there is no lambda body to scan. These are resolved by
# name: find what the named handler is, and check whether IT reads a body.
echo "routes registered with a named handler"
grep -nE 'server\.on\(AsyncURIMatcher::exact\("[^"]+"\), HTTP_(GET|POST), [a-z_]+' "$src/main.cpp" |
while IFS= read -r reg; do
    line="${reg%%:*}"
    route=$(printf '%s' "$reg" | sed -E 's/.*exact\("([^"]+)".*/\1/')
    method=$(printf '%s' "$reg" | grep -oE 'HTTP_(GET|POST)')
    handler=$(printf '%s' "$reg" | sed -E 's/.*HTTP_(GET|POST), ([a-z_]+).*/\2/')
    # Does the named handler function read a body? Read its definition: from
    # its opening line to the first line that closes at column 0.
    reads=$(awk -v fn="$handler" '
        $0 ~ ("^static [a-z]+ " fn "\\(") { inf = 1 }
        inf && (/notify_take_body/ || /_tempObject/) { found = 1 }
        inf && /^\}/ { inf = 0 }
        END { print found + 0 }
    ' "$src/main.cpp")
    collects=$(printf '%s' "$reg" | grep -cE ', NULL, [a-z_]+\);' || true)
    if [ "$reads" = "1" ] && [ "$collects" = "0" ]; then
        echo "  FAIL: $method $route (main.cpp:$line -> $handler) reads a body but registers no body handler"
        echo "FAILED" >> /tmp/seed_routes_fail.$$
    elif [ "$reads" = "1" ]; then
        echo "  ok:   $method $route -> $handler reads a body and collects one"
    else
        echo "  ok:   $method $route -> $handler takes no body"
    fi
done
if [ -f "/tmp/seed_routes_fail.$$" ]; then
    fail=$((fail + $(wc -l < "/tmp/seed_routes_fail.$$")))
    rm -f "/tmp/seed_routes_fail.$$"
fi

# --- Part 3: exact matching on every route ---
#
# The other wiring rule this firmware keeps, and for a reason it already paid
# for once: the library's default matcher answers ^{uri}(/.*)?$, under which
# /ir/tvbgone also answered /ir/tvbgone/stop and a stop request started a
# blast instead of aborting one.
echo "every route uses AsyncURIMatcher::exact()"
# Comment lines are skipped: this reads text, not C++, and a comment that
# quotes a registration is prose rather than a route.
loose=$(grep -nE 'server\.on\(' "$src/main.cpp" "$src"/skills/*.cpp |
        grep -vE ':[[:space:]]*(/\*|\*|//)' |
        grep -v 'AsyncURIMatcher::exact' || true)
if [ -n "$loose" ]; then
    echo "$loose" | while IFS= read -r l; do echo "  FAIL: $l"; done
    fail=$((fail + 1))
else
    echo "  ok:   no route is registered with the default matcher"
fi

echo
if [ "$fail" -gt 0 ]; then
    echo "FAILED"
    exit 1
fi
echo "all checks passed"
