#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/tlora-rns-annsched.XXXXXX")
trap 'rm -f "$OUT"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  "$HERE/test_rns_annsched.cpp" -o "$OUT"
"$OUT"
echo "RNS announce schedule tests: OK"
