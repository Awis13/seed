#!/usr/bin/env python3
"""Static regressions for tools/rns-send — the host side of RNS delivery.

The script cannot be exercised here: it needs a Reticulum instance, a path to a
live device and RNS itself, which is not a dependency of this repo (it lives in
~/rns-venv and nothing else in the tree imports it). So this pins the shape of
the things that are silent when they are wrong, and the tests are chosen for
exactly that property — every one of them guards a failure mode with no error
message anywhere:

  - THE ADDRESS IS app_name + aspects. Change either string and you have a
    different destination hash: no exception, no log line, no complaint from
    either end, just a packet nobody answers. The two constants are read out of
    the firmware's rns.cpp here so the two sides cannot drift apart quietly.
  - THE PATH SPIN. Without a known path the packet goes out as HEADER_1 and is
    dropped inside the local rnsd. send() still returns a receipt. So the spin
    must exist, and it must happen BEFORE the packet is built.
  - THE 383-BYTE CEILING, which is floor((464-48-32)/16)*16-1 and is computed
    identically by RNS/Packet.py and microReticulum's Type.h. The firmware sizes
    a fixed buffer at it; this end must refuse anything larger rather than let
    Packet.pack() raise from inside send().
  - THE EMPTY PAYLOAD, which is the quietest of the lot: it encrypts, travels,
    decrypts, and is then skipped by Destination::receive()'s `if (plaintext)`.
    No card, no counter, no error, and a successful-looking send.
"""

import ast
import py_compile
import re
from pathlib import Path

SEED = Path(__file__).parents[1]
REPO = SEED.parents[1]
SCRIPT = REPO / "tools" / "rns-send" / "rns_send.py"
README = REPO / "tools" / "rns-send" / "README.md"

assert SCRIPT.is_file(), f"{SCRIPT} is missing"
assert README.is_file(), (
    f"{README} is missing: this is the first thing in the repo to use RNS, so "
    "where the interpreter lives has to be written down next to it"
)

src = SCRIPT.read_text(encoding="utf-8")
readme = README.read_text(encoding="utf-8")
rns_cpp = (SEED / "src" / "skills" / "rns.cpp").read_text(encoding="utf-8")
inbox_h = (SEED / "src" / "rns" / "inbox.h").read_text(encoding="utf-8")

# --- 0. it is a valid, runnable Python script --------------------------------
py_compile.compile(str(SCRIPT), doraise=True)
assert src.startswith("#!/usr/bin/env python3\n"), (
    "the repo's host tools all start with `#!/usr/bin/env python3`"
)

# The interpreter is the one thing a reader cannot guess: RNS is in ~/rns-venv
# and in no other Python on this machine. It has to be said in the script's own
# header, not only in the README.
header = src[: src.index('"""', src.index('"""') + 3)]
assert "rns-venv" in header, (
    "the script's own docstring must name the interpreter that has RNS "
    "(~/rns-venv); nothing else in this repo imports RNS, so there is no "
    "convention for a reader to fall back on"
)
assert "rns-venv" in readme, "the README must name the interpreter too"

# --- 1. stdlib only, besides RNS ---------------------------------------------
tree = ast.parse(src)
imported = set()
for node in ast.walk(tree):
    if isinstance(node, ast.Import):
        imported.update(alias.name.split(".")[0] for alias in node.names)
    elif isinstance(node, ast.ImportFrom) and node.module:
        imported.add(node.module.split(".")[0])
ALLOWED_IMPORTS = {"__future__", "math", "os", "sys", "time", "typing", "RNS"}
assert imported <= ALLOWED_IMPORTS, (
    "host tools in this repo are stdlib plus the one library they exist for; "
    "found %r" % sorted(imported - ALLOWED_IMPORTS)
)
assert "RNS" in imported, "the script exists to talk to Reticulum"

# --- 2. the address: app_name and aspects must match the firmware ------------
# These two strings ARE the destination hash. A mismatch is not an error on
# either end — it is a different address — so it has to fail here or nowhere.
app_name = re.search(r'#define RNS_DEST_APP_NAME\s+"([^"]+)"', rns_cpp)
aspects = re.search(r'#define RNS_DEST_ASPECTS\s+"([^"]+)"', rns_cpp)
assert app_name and aspects, "could not read the destination constants from rns.cpp"
assert f'APP_NAME = "{app_name.group(1)}"' in src, (
    "the script's APP_NAME must be the firmware's RNS_DEST_APP_NAME (%r)"
    % app_name.group(1)
)
assert f'ASPECTS = ("{aspects.group(1)}",)' in src, (
    "the script's ASPECTS must be the firmware's RNS_DEST_ASPECTS (%r)"
    % aspects.group(1)
)
# ...and the script must not merely hold them: it must derive the address from
# them and compare, so that a future drift is loud instead of silent.
assert "RNS.Destination.hash(identity, APP_NAME, *ASPECTS)" in src, (
    "the script must derive the destination hash from its own constants"
)
assert "derived != dest_hash" in src, (
    "the derived address must be compared against the one the user passed; "
    "without the comparison the constants are decoration"
)

# --- 3. the path spin, and its position --------------------------------------
assert "RNS.Transport.has_path(" in src and "RNS.Transport.request_path(" in src, (
    "the script must request a path and check for it"
)
spin = src[src.index("def wait_for_path") : src.index("def main(")]
assert re.search(r"while .*time\(\)\s*<\s*deadline", spin), (
    "the wait must be a real spin with a deadline, not a single check"
)
assert "time.sleep(0.1)" in spin, "the spin must yield between checks"
assert "RNS.Transport.has_path(" in spin

main_body = src[src.index("def main(") :]
assert "wait_for_path(" in main_body
assert main_body.index("wait_for_path(") < main_body.index("RNS.Packet("), (
    "the path must be waited for BEFORE the packet is built and sent: without "
    "one the packet is dropped inside the local rnsd and send() still succeeds"
)
assert main_body.index("wait_for_path(") < main_body.index("RNS.Identity.recall("), (
    "recall() needs the announce the path request fetches"
)
# A timeout is an error, not a shrug.
assert re.search(r"if not wait_for_path\([^)]*\):\s*\n\s*fail\(", main_body), (
    "a path timeout must fail loudly; sending anyway looks successful and "
    "delivers nothing"
)
assert "RNS_PATH_TIMEOUT" in src and "DEFAULT_PATH_TIMEOUT = 15.0" in src, (
    "the timeout needs a sane default and an environment override"
)
# The timeout report has to start on THIS side. The two likeliest causes at two
# in the morning are local — no rnsd, or no interface onto the device's network —
# and the third is passing the identity `hash` where the destination `address`
# belongs, which never gets a path however long it waits.
fail_msg = src[src.index("no path to %s after") :]
fail_msg = fail_msg[: fail_msg.index('% (dest_hash.hex(), timeout)')]
for hint in ("rnsd", "interface", "`hash`"):
    assert hint in fail_msg, (
        "the path-timeout message must name the local causes too, not only the "
        "device's announce schedule; missing %r" % hint
    )

# --- 4. the 383-byte ceiling -------------------------------------------------
assert "PAYLOAD_MAX_BYTES = 383" in src, (
    "the ceiling is 383 bytes: floor((464-48-32)/16)*16-1, the same number "
    "RNS/Packet.py and microReticulum's Type.h compute"
)
assert "#define RNS_INBOX_PAYLOAD_MAX 383" in inbox_h, (
    "the firmware's receive buffer must be the same 383 bytes"
)
assert "RNS_INBOX_PAYLOAD_MAX == (size_t)RNS::Type::Packet::ENCRYPTED_MDU" in rns_cpp, (
    "the firmware must static_assert its buffer against the library's own "
    "ENCRYPTED_MDU, so a drift is a failed build and not a truncated message"
)
assert "RNS.Packet.ENCRYPTED_MDU != PAYLOAD_MAX_BYTES" in src, (
    "the script must cross-check its constant against the installed library "
    "rather than assume the two agree"
)
check = src[src.index("def check_payload") : src.index("def wait_for_path")]
assert "len(payload) > PAYLOAD_MAX_BYTES" in check, (
    "the payload must be measured against the ceiling before anything is sent"
)
assert main_body.index("check_payload(") < main_body.index("RNS.Reticulum("), (
    "an oversize message must be refused before a Reticulum instance is even "
    "brought up"
)
# pack() raises OSError and send() calls it implicitly, so both are caught.
assert "except OSError" in src, (
    "Packet.pack() raises OSError and send() calls pack() implicitly"
)
assert "packet.pack()" in main_body and main_body.index(
    "packet.pack()") < main_body.index("packet.send()"), (
    "pack() explicitly before send(), so a size error is reported here rather "
    "than as a traceback out of send()"
)

# --- 5. the empty payload ----------------------------------------------------
assert "len(payload) == 0" in check, "an empty payload must be refused"
assert "if (plaintext)" in check, (
    "the refusal must explain itself: the device's Destination::receive() skips "
    "the callback on empty plaintext, so an empty send is silent at both ends"
)

# --- 6. configuration through the environment, with defaults -----------------
for var in ("RNS_PAGER_ADDR", "RNS_CONFIG_DIR", "RNS_PATH_TIMEOUT"):
    assert var in src, (
        "%s must be configurable through the environment, like the other host "
        "tools here" % var
    )
    assert var in readme, "%s must be documented in the README" % var
# A bare float() on an environment variable is a traceback for the most ordinary
# typo there is, and this script's docstring promises exit codes instead.
assert "def env_seconds(" in src and "except ValueError" in src, (
    "seconds from the environment must be parsed with a refusal, not a bare "
    "float() that raises"
)
assert not re.search(r"float\(os\.environ", src), (
    "float(os.environ.get(...)) turns RNS_PATH_TIMEOUT=soon into a traceback"
)

# --- 6b. the first argument is an address only when it looks like one ---------
# Without the shape test, `rns_send.py hello world` with RNS_PAGER_ADDR set
# complains that "hello" is not 32 hex characters, and `rns_send.py <address>`
# with the same variable set sends the address itself as the message.
assert "def looks_like_address(" in src, (
    "the argument parser must decide by the shape of the first word"
)
parse = src[src.index("def parse_args(") : src.index("def check_address(")]
assert "looks_like_address(argv[0])" in parse, (
    "parse_args must use the shape test rather than argument count alone"
)
assert 'return env_addr, " ".join(argv)' in parse, (
    "with RNS_PAGER_ADDR set and a first word that is not an address, the whole "
    "command line is the message"
)

# --- 7. `address` is not `hash`, and the README says which is which ----------
# GET /rns/status publishes both; they are different values and swapping them is
# a send to nothing.
assert "identity hash" in src and "identity hash" in readme, (
    "both the script and the README must warn that the destination hash is not "
    "the identity hash"
)

print("RNS sender static tests: OK")
