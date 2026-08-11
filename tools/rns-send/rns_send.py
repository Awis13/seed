#!/usr/bin/env python3
"""
rns-send — put one short text on the T-Lora pager's screen over Reticulum.

THE INTERPRETER IS NOT THE ONE ON YOUR PATH, and this is the first thing in
this repo to need saying: nothing else here imports RNS (a repo-wide grep for
`import RNS` finds only this file), so there is no convention to copy. Reticulum
1.4.2 lives in a virtualenv of its own:

    ~/rns-venv/bin/python3 tools/rns-send/rns_send.py <address> "hello"

`python3 rns_send.py` will fail on the import unless RNS happens to be in that
interpreter too. See README.md next to this file.

WHAT IT DOES. One raw encrypted packet to the device's `seed.pager` destination,
no Link and no LXMF: the firmware declines link requests (accepts_links(false))
and has no LXMF destination, so a single packet is the whole protocol. The
device decrypts it, hands the plaintext to its packet callback, and the loop
task raises a notification card with it.

WHAT SUCCESS MEANS, AND WHAT IT DOES NOT. `Packet.send()` returning a receipt
means the packet was accepted by the local Reticulum for transmission. It is NOT
a delivery confirmation: the device's destination keeps the library default
PROVE_NONE, so it never signs a proof and nothing ever comes back. The only
confirmation is at the other end — `GET /rns/status` on the pager, where
`data_rx` counts payloads that decrypted and `data_last_len` is the size of the
most recent one.

THE PATH SPIN IS NOT OPTIONAL. Without a known path the packet is sent as
HEADER_1 to an unknown next hop and is dropped inside the local rnsd with no
error to the caller: the send looks perfectly successful and nothing arrives.
So the path is requested and waited for, and a timeout is an error with a
message rather than a cheerful exit code 0.

Configuration is environment variables with defaults, like the other host tools
in this repo:

    RNS_PAGER_ADDR      32 hex characters, the device's DESTINATION hash (the
                        `address` field of GET /rns/status — NOT `hash`, which
                        is the identity hash and is a different value). Used
                        when no address is given on the command line.
    RNS_CONFIG_DIR      Reticulum config directory; unset means the library
                        default (~/.reticulum).
    RNS_PATH_TIMEOUT    seconds to wait for a path before giving up (default 15)
    RNS_SEND_LINGER     seconds to stay alive after the send (default 2) so the
                        packet reaches the interface before the process exits

Usage:
    rns_send.py <address> <text...>
    rns_send.py <text...>            (address from RNS_PAGER_ADDR)

Exit codes: 0 sent, 2 bad usage or payload, 3 no path or no identity,
4 the address does not belong to seed.pager, 5 the send failed.
"""

from __future__ import annotations

import math
import os
import sys
import time
from typing import NoReturn

import RNS

# The address is app_name + aspects, and it must match the firmware's
# RNS_DEST_APP_NAME / RNS_DEST_ASPECTS in
# seeds/esp32-tlora-pager/src/skills/rns.cpp exactly. A mismatch is not an
# error anywhere: it is simply a DIFFERENT destination hash, so the packet goes
# to an address nobody answers and both ends stay silent. That is why this
# script derives the address from these two strings below and refuses to send
# when the result is not the address it was given.
# seeds/esp32-tlora-pager/tools/test_rns_send.py reads both constants out of
# rns.cpp and fails if either side drifts.
APP_NAME = "seed"
ASPECTS = ("pager",)

# The largest plaintext a single encrypted packet to a SINGLE destination can
# carry: floor((MDU - TOKEN_OVERHEAD - KEYSIZE/16) / AES128_BLOCKSIZE)
# * AES128_BLOCKSIZE - 1 = floor((464 - 48 - 32) / 16) * 16 - 1 = 383.
# RNS/Packet.py computes ENCRYPTED_MDU this way and microReticulum's Type.h
# computes it identically, which is why the device can size a fixed 383-byte
# buffer for it. The number is written out rather than imported so that a
# library that ever disagrees is caught by the cross-check below instead of
# silently moving the limit under us.
PAYLOAD_MAX_BYTES = 383

DEFAULT_PATH_TIMEOUT = 15.0
DEFAULT_LINGER = 2.0


def fail(code: int, message: str) -> NoReturn:
    print("rns-send: " + message, file=sys.stderr)
    sys.exit(code)


def env_seconds(name: str, default: float) -> float:
    """A number of seconds from the environment, or a clear refusal.

    Bare float() on an environment variable produces the traceback this script's
    docstring promises not to produce, for the most ordinary typo there is.
    """
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        value = float(raw)
    except ValueError:
        fail(2, "%s must be a number of seconds, got %r" % (name, raw))
    if value < 0:
        fail(2, "%s must not be negative, got %r" % (name, raw))
    return value


def address_length_hex() -> int:
    return (RNS.Reticulum.TRUNCATED_HASHLENGTH // 8) * 2


def check_library_ceiling() -> None:
    """Refuse to guess if this RNS disagrees with the number above."""
    computed = (
        math.floor(
            (
                RNS.Reticulum.MDU
                - RNS.Identity.TOKEN_OVERHEAD
                - RNS.Identity.KEYSIZE // 16
            )
            / RNS.Identity.AES128_BLOCKSIZE
        )
        * RNS.Identity.AES128_BLOCKSIZE
        - 1
    )
    if computed != PAYLOAD_MAX_BYTES or RNS.Packet.ENCRYPTED_MDU != PAYLOAD_MAX_BYTES:
        fail(
            2,
            "this Reticulum computes a payload ceiling of %d (Packet.ENCRYPTED_MDU "
            "%d) but this script and the firmware are built around %d. One of the "
            "three has moved; fix them together rather than trusting this send."
            % (computed, RNS.Packet.ENCRYPTED_MDU, PAYLOAD_MAX_BYTES),
        )


def looks_like_address(word: str) -> bool:
    """Is this argument an address rather than the first word of a message?

    The only honest test is the shape of the thing: exactly the right number of
    hexadecimal characters. Without it, `rns_send.py hello world` with
    RNS_PAGER_ADDR set complains that "hello" is not 32 hex characters, and
    `rns_send.py <address>` with the same variable set silently sends the
    address itself as the message.

    THE COST OF THE SHAPE TEST, stated because it is a real regression for one
    case: with RNS_PAGER_ADDR set, an address carrying a single non-hex typo is
    no longer reported as a bad address. It fails this test, so it becomes the
    first word of the MESSAGE and the send goes to the environment's address
    instead. Before, the same typo was caught and named. There is no way to have
    both — "is this an address or a word" has exactly one signal here — and the
    quiet failure is the one that ends with the message delivered somewhere real
    rather than a refusal for a message that was fine.
    """
    word = word.strip().lower()
    if len(word) != address_length_hex():
        return False
    return all(c in "0123456789abcdef" for c in word)


def parse_args(argv: list[str]) -> tuple[str, str]:
    if not argv:
        fail(2, "nothing to send.\n\n" + __doc__.strip())

    env_addr = os.environ.get("RNS_PAGER_ADDR", "").strip()

    if looks_like_address(argv[0]):
        if len(argv) >= 2:
            return argv[0], " ".join(argv[1:])
        # An address and nothing else is a forgotten message, not a message that
        # happens to be an address. Sending the address text to the screen is
        # never what anyone means, and with RNS_PAGER_ADDR set it is what a
        # naive parser does silently.
        fail(
            2,
            "that is a destination address with no message after it. If the hex "
            "string really is the message, give the address first: "
            "rns_send.py <address> %s" % argv[0],
        )

    if env_addr:
        # The first word is not address-shaped, so everything on the command
        # line is the message and the address comes from the environment.
        return env_addr, " ".join(argv)

    fail(
        2,
        "no destination address: %r is not a %d-character hexadecimal address "
        "and RNS_PAGER_ADDR is not set. Pass the address as the first argument, "
        "or export RNS_PAGER_ADDR — it is the `address` field of the pager's "
        "GET /rns/status." % (argv[0], address_length_hex()),
    )


def check_address(address: str) -> bytes:
    want = address_length_hex()
    address = address.strip().lower()
    if len(address) != want:
        fail(
            2,
            "destination address must be %d hexadecimal characters, got %d (%r). "
            "It is the `address` field of GET /rns/status, not `hash` — `hash` is "
            "the identity hash and is a different value."
            % (want, len(address), address),
        )
    try:
        return bytes.fromhex(address)
    except ValueError:
        fail(2, "destination address is not hexadecimal: %r" % address)


def check_payload(text: str) -> bytes:
    payload = text.encode("utf-8")
    if len(payload) == 0:
        fail(
            2,
            "refusing to send an empty message. It would encrypt and decrypt "
            "perfectly and then vanish: the device's Destination::receive() only "
            "calls its packet callback inside `if (plaintext)`, so an empty "
            "payload produces no card, no counter and no error, while this end "
            "reports a successful send.",
        )
    if len(payload) > PAYLOAD_MAX_BYTES:
        fail(
            2,
            "message is %d bytes of UTF-8 and the ceiling is %d. A single "
            "encrypted packet to a SINGLE destination carries no more, and the "
            "device's inbox buffer is exactly that size. Shorten it, or send it "
            "in pieces."
            % (len(payload), PAYLOAD_MAX_BYTES),
        )
    return payload


def wait_for_path(dest_hash: bytes, timeout: float) -> bool:
    """Request a path and spin until it exists. Never skip this — see the note
    in the module docstring about the silent drop inside rnsd."""
    if RNS.Transport.has_path(dest_hash):
        return True

    RNS.Transport.request_path(dest_hash)
    print(
        "rns-send: requesting a path to %s (up to %.0fs)..."
        % (dest_hash.hex(), timeout),
        file=sys.stderr,
    )
    deadline = time.time() + timeout
    while time.time() < deadline:
        if RNS.Transport.has_path(dest_hash):
            return True
        time.sleep(0.1)
    return RNS.Transport.has_path(dest_hash)


def main(argv: list[str]) -> int:
    address, text = parse_args(argv)
    check_library_ceiling()
    dest_hash = check_address(address)
    payload = check_payload(text)

    timeout = env_seconds("RNS_PATH_TIMEOUT", DEFAULT_PATH_TIMEOUT)
    linger = env_seconds("RNS_SEND_LINGER", DEFAULT_LINGER)
    configdir = os.environ.get("RNS_CONFIG_DIR") or None

    RNS.Reticulum(configdir=configdir, loglevel=RNS.LOG_WARNING)

    if not wait_for_path(dest_hash, timeout):
        fail(
            3,
            "no path to %s after %.0fs. Sending anyway would look successful "
            "and be dropped inside the local Reticulum, so it is an error.\n"
            "  Check this end first, because it is the likelier half:\n"
            "    - is rnsd running, and does its config have an interface onto "
            "the network the device is on? A Reticulum with no interface never "
            "hears an announce and every address is unreachable.\n"
            "    - is this the device's `address` and not its `hash`? Both are "
            "32 hex characters in GET /rns/status and only one of them is a "
            "destination; the identity hash has no path and never will.\n"
            "  Then the far end:\n"
            "    - the device announces about 5s after its TCP interface comes "
            "up and then every 30 minutes, so one that has just been power-"
            "cycled needs a moment, and one whose interface is down never "
            "announces at all (GET /rns/status, iface.state)."
            % (dest_hash.hex(), timeout),
        )

    identity = RNS.Identity.recall(dest_hash)
    if identity is None:
        fail(
            3,
            "a path to %s is known but its identity is not, so the packet cannot "
            "be encrypted. That means the announce that taught this Reticulum the "
            "path did not carry the destination's public key — wait for the next "
            "announce and try again." % dest_hash.hex(),
        )

    # The address is derived rather than trusted. If APP_NAME/ASPECTS here ever
    # stop matching the firmware, every send goes to a destination hash nobody
    # owns and NOTHING reports it — not this script, not the device, not the
    # network. This is the check that turns that silence into an error.
    derived = RNS.Destination.hash(identity, APP_NAME, *ASPECTS)
    if derived != dest_hash:
        fail(
            4,
            "the address %s does not belong to %s on that identity — this script "
            "derives %s from app_name %r and aspects %r. Either the address is "
            "for a different destination of the same node, or these constants no "
            "longer match the firmware's RNS_DEST_APP_NAME / RNS_DEST_ASPECTS."
            % (
                dest_hash.hex(),
                ".".join((APP_NAME,) + ASPECTS),
                derived.hex(),
                APP_NAME,
                ASPECTS,
            ),
        )

    destination = RNS.Destination(
        identity,
        RNS.Destination.OUT,
        RNS.Destination.SINGLE,
        APP_NAME,
        *ASPECTS,
    )

    packet = RNS.Packet(destination, payload)
    try:
        # pack() explicitly, even though send() calls it: an oversize packet
        # raises OSError out of pack(), and catching it here reports the size
        # rather than a traceback.
        packet.pack()
    except OSError as exc:
        fail(
            5,
            "Reticulum refused to pack a %d-byte payload: %s" % (len(payload), exc),
        )

    try:
        receipt = packet.send()
    except OSError as exc:
        fail(5, "send failed: %s" % exc)

    if not receipt:
        fail(
            5,
            "send returned no receipt — the local Reticulum did not accept the "
            "packet for transmission.",
        )

    # The packet leaves on Reticulum's own threads. Exiting the instant send()
    # returns can cut that off before the bytes reach the interface, so linger
    # briefly by default.
    if linger > 0:
        time.sleep(linger)

    print(
        "sent %d byte%s to %s (%s). No delivery proof exists: the destination "
        "keeps PROVE_NONE, so confirm on the device with GET /rns/status "
        "(data_rx, data_last_len)."
        % (
            len(payload),
            "" if len(payload) == 1 else "s",
            dest_hash.hex(),
            ".".join((APP_NAME,) + ASPECTS),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
