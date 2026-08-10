#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/tlora-rns-hdlc.XXXXXX")
trap 'rm -f "$OUT"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  "$HERE/test_rns_hdlc.cpp" -o "$OUT"
"$OUT"
echo "RNS HDLC framing tests: OK"
