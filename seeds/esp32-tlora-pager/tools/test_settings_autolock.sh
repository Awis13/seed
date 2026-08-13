#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/tlora-settings.XXXXXX")
trap 'rm -f "$OUT"' EXIT
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror "$HERE/test_settings_policy.cpp" -o "$OUT"
"$OUT"
