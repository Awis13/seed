#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/tlora-micron-render.XXXXXX")
trap 'rm -f "$OUT"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  "$HERE/test_micron_render.cpp" -o "$OUT"
"$OUT"
echo "micron render palette + diff tests: OK"
