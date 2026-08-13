#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/tlora-net-view.XXXXXX")
trap 'rm -f "$OUT"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  "$HERE/test_net_view.cpp" -o "$OUT"
"$OUT"

python3 - "$HERE/../src/main.cpp" "$HERE/../src/hw_ui.cpp" "$HERE/../src/hw_ui.h" "$HERE/../src/skills/meshcore.cpp" <<'PYEOF'
import sys
from pathlib import Path

main = Path(sys.argv[1]).read_text(encoding="utf-8")
hw = Path(sys.argv[2]).read_text(encoding="utf-8")
hw_h = Path(sys.argv[3]).read_text(encoding="utf-8")
mesh = Path(sys.argv[4]).read_text(encoding="utf-8")

# MeshCore STATUS and WiFi STATUS share the same sectioned NET model while
# retaining the menu that opened it as the back target.
click = main[main.index("static void ui_on_click()") : main.index("static void ui_on_steps(")]
assert "ui_open_net(UINAV_WIFI);" in click
assert "ui_open_net(UINAV_MESHCORE);" in click
assert "ui_mesh_show_status" not in main and "mesh_status_lines" not in mesh
assert "net_origin = origin;" in main
assert "origin == UINAV_MESHCORE" in main and "NET_SEC_MESH" in main

# The final Mesh ping face is one Mesh column. The existing HTTP probe remains
# intact; this card changes presentation, not transport behaviour.
assert "static void ui_mesh_ping_step_http()" in main
assert "hw_ui_show_mesh_ping_result(mesh_ok, mesh_s1, mesh_s2);" in main
assert "if (mesh_ok) hw_haptic_notify(0);" in main
assert "ping_draw_wifi_icon" not in hw
assert "ping_draw_path_column(PANEL_W / 2, mesh_ok" in hw

# INFO no longer duplicates network/message state, and the old WiFi INFO name
# now describes its actual scan/connect-progress role.
info = hw[hw.index("void hw_ui_show_info(") : hw.index("// ---- micron page renderer")]
assert '"IP    %s"' not in info and '"INBOX %d unread"' not in info
assert "HW_UI_WIFI_PROGRESS" in hw_h and "HW_UI_WIFI_INFO" not in hw_h
assert "hw_ui_show_wifi_progress" in hw_h and "hw_ui_show_wifi_info" not in hw_h

# WireGuard remains in the shared NET snapshot but no longer paints a false
# clock-header badge.
clock = hw[hw.index("void hw_ui_clock_tick(") : hw.index("void hw_ui_clock_rule_tick(")]
assert '"W"' not in clock and "wg_ui" not in clock
assert "s.tun.ui_state = wg_ui_state();" in main
PYEOF
echo "net view tests: OK"
