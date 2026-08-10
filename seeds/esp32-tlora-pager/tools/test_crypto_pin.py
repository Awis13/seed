#!/usr/bin/env python3
"""Static regression for the Crypto dependency slot.

Two different libraries claim the name "Crypto" in this build: the registry
package rweather/Crypto that MeshCore and our ed25519 glue expect, and an
unpinned git fork (attermann/Crypto, tracks master) declared transitively by
microReticulum. PlatformIO deduplicates library dependencies by NAME and
installs direct dependencies first, so the registry package currently wins —
but that is resolution order, not identity: the installed .piopm records
`spec.uri: null` and nothing else in the tree says which one landed.

If the fork ever takes the slot (a `pio pkg update`, a different PlatformIO
version, or someone dropping our "redundant" direct dependency), MeshCore and
RadioLib would build against it silently. This test makes that loud.

Skips when .pio/libdeps does not exist — a clean checkout has not resolved
dependencies yet and must not fail the suite for it.
"""

from pathlib import Path
import json
import sys


ROOT = Path(__file__).parents[1]
LIBDEPS = ROOT / ".pio" / "libdeps"

if not LIBDEPS.is_dir():
    print("crypto pin tests: SKIP (no .pio/libdeps yet — run `pio run` first)")
    sys.exit(0)

# --- platformio.ini pins the registry package exactly, not as a range --------
ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")
assert "rweather/Crypto @ 0.4.0" in ini, (
    "platformio.ini must pin rweather/Crypto to an EXACT version — a caret "
    "range invites a resolver that prefers the unpinned attermann fork"
)
assert "rweather/Crypto @ ^" not in ini, "the caret range is back"

# --- whatever is installed in the Crypto slot is the rweather one ------------
checked = 0
for env_dir in sorted(p for p in LIBDEPS.iterdir() if p.is_dir()):
    piopm = env_dir / "Crypto" / ".piopm"
    if not piopm.is_file():
        continue
    meta = json.loads(piopm.read_text(encoding="utf-8"))
    owner = (meta.get("spec") or {}).get("owner")
    assert owner == "rweather", (
        f"{piopm}: Crypto is owned by {owner!r}, expected 'rweather'. The "
        "unpinned attermann/Crypto fork pulled in by microReticulum has taken "
        "the name slot — MeshCore and RadioLib are building against it."
    )
    assert meta.get("name") == "Crypto"
    assert meta.get("version") == "0.4.0", (
        f"{piopm}: Crypto {meta.get('version')!r} installed, pin says 0.4.0"
    )
    checked += 1

if checked == 0:
    print("crypto pin tests: SKIP (Crypto not installed in any env yet)")
    sys.exit(0)

print(f"crypto pin tests: OK ({checked} env(s))")
