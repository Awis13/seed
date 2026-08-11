#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/tlora-l1-frame.XXXXXX")
trap 'rm -f "$OUT"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  "$HERE/test_l1_frame.cpp" -o "$OUT"
"$OUT"
echo "l1 frame tests: OK"
