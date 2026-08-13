#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/tlora-secret-store.XXXXXX")
trap 'rm -f "$OUT"' EXIT

# secret_store.cpp is compiled against the tools/host_arduino mocks so the
# thin device wrappers (put/get/has/del) execute on the host, not just the
# pure header logic.
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I "$HERE/host_arduino" \
  "$HERE/test_secret_store.cpp" "$HERE/../src/secret_store.cpp" -o "$OUT"
"$OUT"
echo "secret store tests: OK"
