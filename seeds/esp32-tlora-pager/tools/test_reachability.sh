#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/tlora-reachability.XXXXXX")
trap 'rm -f "$OUT"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  "$HERE/test_reachability.cpp" -o "$OUT"
"$OUT"
echo "reachability tests: OK"
