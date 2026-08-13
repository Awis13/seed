#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/tlora-utf8-text.XXXXXX")
trap 'rm -f "$OUT"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  "$HERE/test_utf8_text.cpp" -o "$OUT"
"$OUT"

python3 - "$HERE/../src/skills/notify.cpp" "$HERE/../src/skills/progress.cpp" <<'PYEOF'
import sys
from pathlib import Path

notify = Path(sys.argv[1]).read_text(encoding="utf-8")
progress = Path(sys.argv[2]).read_text(encoding="utf-8")
assert '"title 60, body 240, source 16, id 24' in notify
assert "title 40, body 96" not in notify
assert "TFT_eSPI" not in notify and "TFT_eSPI" not in progress
PYEOF
