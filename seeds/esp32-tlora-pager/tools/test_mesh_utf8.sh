#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/tlora-mesh-utf8.XXXXXX")
trap 'rm -f "$OUT"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  "$HERE/test_mesh_utf8.cpp" -o "$OUT"
"$OUT"
echo "mesh UTF-8 chunk tests: OK"
