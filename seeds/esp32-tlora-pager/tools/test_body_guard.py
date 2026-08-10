#!/usr/bin/env python3
"""Static regressions for the shared POST body collector's bounds guard.

handle_body_collect() runs on the AsyncTCP task BEFORE the route handler and
therefore before any auth check, on every body route (/notify, /gw/token,
/wg/*, /agents/*, /backlight, /progress, /mesh/inject, /gps/fix, /config.md,
/clock/tz, /wifi/networks). ESPAsyncWebServer sets _contentLength only from a
Content-Length header; a Transfer-Encoding: chunked body has none, so the
callback receives total == 0 with a growing index and unclamped len. The
pre-fix collector read that as "zero bytes expected", allocated one byte, and
memcpy'd every later chunk past it — an unauthenticated heap overflow.

These asserts pin the proven guard shape (same form as the tembed/cardputer
and ats-mini seeds):
  - total == 0 (chunked, unknown length) refused up front with 411;
  - oversized total still refused with 413 against HTTP_BODY_MAX;
  - every chunk bounds-checked against total before the memcpy
    (index + len must not run past the allocation);
  - calloc, not malloc, so an incomplete body leaves defined NUL bytes;
  - NUL termination on every chunk, not only when index + len == total.
"""

from pathlib import Path

ROOT = Path(__file__).parents[1]
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

start = main.index("static void handle_body_collect")
end = main.index("static void handle_config_get")
collect = main[start:end]

# Chunked body (no Content-Length): total stays 0 and len > 0 chunks follow
# with a growing index. Refuse before any allocation — 411 Length Required.
assert "if (total == 0) { request->send(411," in collect, (
    "collector must refuse unknown-length (chunked) bodies with 411"
)
assert collect.index("total == 0") < collect.index("total > HTTP_BODY_MAX"), (
    "the 411 guard must run before the size cap so a zero total never "
    "reaches the allocation"
)
assert collect.index("total == 0") < collect.index("calloc"), (
    "the 411 guard must precede the allocation"
)

# Declared length still capped.
assert "if (total > HTTP_BODY_MAX) { request->send(413," in collect, (
    "collector must keep the 413 size cap"
)

# calloc, not malloc: a body that never completes leaves defined NUL bytes,
# not uninitialised heap handed to handlers as a C string.
assert "calloc(total + 1, 1)" in collect, "collector must calloc the body buffer"
assert "malloc" not in collect, "collector must not regress to malloc"

# Per-chunk bounds check before the memcpy: index + len can run past total on
# a chunked stream the library did not clamp. The subtraction form avoids
# size_t overflow in index + len.
assert "if (index > total || len > total - index) return;" in collect, (
    "every chunk must be bounds-checked against total before the memcpy"
)
assert collect.index("index > total || len > total - index") < collect.index(
    "memcpy"
), "the bounds check must precede the memcpy"

# Terminate on every chunk: the final chunk is not guaranteed to arrive.
assert "buf[index + len] = '\\0';" in collect, (
    "collector must NUL-terminate on every chunk"
)
assert "if (index + len == total && buf) buf[total]" not in collect, (
    "termination must not be gated on the final chunk arriving"
)

# The dedicated firmware upload body handler keeps its own total == 0 guard.
fw = main[main.index("static void handle_firmware_upload_body"):]
fw = fw[: fw.index("static void handle_firmware_upload(")]
assert "total == 0" in fw, "firmware upload body must keep its zero-size guard"

print("body collector guard tests: OK")
