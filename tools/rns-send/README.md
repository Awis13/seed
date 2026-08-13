# rns-send

Send one short text from a Mac to the T-Lora pager over Reticulum. It arrives as
a notification card on the device's screen.

## The interpreter

**This is the only thing in this repo that imports RNS.** A repo-wide grep for
`import RNS` finds this directory and nothing else, so there is no existing
convention to follow — and the system `python3` does not have Reticulum in it.
Reticulum 1.4.2 lives in its own virtualenv:

```
~/rns-venv/bin/python3
```

Everything below assumes that interpreter. `python3 rns_send.py` will fail on
the import.

## Usage

```
~/rns-venv/bin/python3 tools/rns-send/rns_send.py <address> "hello from the mac"
```

`<address>` is the device's **destination hash** — 32 hexadecimal characters,
the `address` field of `GET /rns/status` on the pager. It is *not* the `hash`
field, which is the identity hash and is a different value.

With `RNS_PAGER_ADDR` set, the address can be left off:

```
export RNS_PAGER_ADDR=<32 hex chars>
~/rns-venv/bin/python3 tools/rns-send/rns_send.py "hello from the mac"
```

The first argument is treated as an address only when it is exactly 32
hexadecimal characters *and* something follows it, so `rns_send.py hello world`
sends "hello world" and does not complain that "hello" is not an address. An
address with nothing after it is refused rather than sent as the message text.

⚠️ The cost of that rule: with `RNS_PAGER_ADDR` set, an address with a single
non-hex typo is not reported as a bad address — it fails the shape test, becomes
the first word of the message, and the send goes to the environment's address
instead. Check the address echoed in the success line if you pasted one.

## Environment

| Variable | Default | Meaning |
| --- | --- | --- |
| `RNS_PAGER_ADDR` | — | destination hash, used when no address is given as an argument |
| `RNS_CONFIG_DIR` | library default (`~/.reticulum`) | Reticulum config directory |
| `RNS_PATH_TIMEOUT` | `15` | seconds to wait for a path before giving up |
| `RNS_SEND_LINGER` | `2` | seconds to stay alive after the send so the packet reaches the interface |

## What it does, and what it refuses to do

One raw encrypted packet to the device's `seed.pager` destination. No Link and
no LXMF: the firmware declines link requests and has no LXMF destination, so a
single packet is the whole protocol.

Three things it refuses, each because the failure is otherwise silent:

- **An empty message.** It would encrypt, travel, decrypt — and then vanish.
  The device's `Destination::receive()` calls its packet callback only inside
  `if (plaintext)`, so a zero-length payload produces no card, no counter and no
  error, while the sender reports success.
- **More than 383 bytes of UTF-8.** That is the largest plaintext a single
  encrypted packet to a SINGLE destination carries —
  `floor((464 - 48 - 32) / 16) * 16 - 1` — computed identically by Reticulum's
  `Packet.py` and by microReticulum's `Type.h`, and it is the size of the fixed
  buffer the firmware receives into.
- **An address that is not `seed.pager` on that identity.** The script recalls
  the identity behind the address, re-derives the destination hash from
  `app_name = "seed"` and `aspects = ("pager",)`, and compares. A mismatch in
  those strings is not an error anywhere in Reticulum — it is simply a different
  address — so without this check a typo would send into the void with both ends
  silent.

It also **waits for a path** before sending, and reports a timeout as an error.
This is not politeness: without a known path the packet goes out as HEADER\_1 and
is dropped inside the local `rnsd` with no error to the caller. The send looks
successful and nothing arrives.

## What "sent" means

`Packet.send()` returning a receipt means the local Reticulum accepted the
packet for transmission. It is **not** a delivery confirmation. The device's
destination keeps the library default `PROVE_NONE`, so it never signs a proof
and nothing comes back.

Confirm at the other end:

```
curl -s -H "Authorization: Bearer $PAGER_TOKEN" http://<pager>:8080/rns/status
```

- `data_rx` — payloads the device decrypted and handed to its packet callback
- `data_last_len` — size in bytes of the most recent one
- `data_dropped` — messages refused because all eight inbox slots were still full
  (the device keeps the oldest and counts the drop)
- `data_cards` / `data_card_cut` — cards raised, and cards whose body could not
  hold the whole message

## What the card looks like

Title `RNS <n> B` with the payload's true byte count, source `rns`, body the
sanitised text. The device cannot name the sender — a packet to a SINGLE
destination carries no source — so anyone who knows the address can raise a card,
and nothing on the device rate-limits how often.

The card paints five rows of about 37 characters and does not scroll, so a
message longer than that is cut. A cut message is **prefixed** with `[+N B]`,
where N is the number of bytes the screen does not carry — a prefix rather than a
suffix because word wrap decides how much of the last row is used, and the first
row is always drawn. Control bytes are rendered as `.` and malformed UTF-8 as
`?`, because the firmware sanitises the payload before it reaches the screen.

## Bursts

The device holds eight messages — exactly what one socket drain can deliver
between two screen-side pickups, which run every 20 ms. So a burst of eight
arrives as eight cards; anything beyond that in a single tick is refused and
counted in `data_dropped`. If you split a long message into pieces, eight at a
time with a moment between them is the safe shape.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | sent |
| 2 | bad usage, bad address, or a payload that is empty or too large |
| 3 | no path within the timeout, or the identity could not be recalled |
| 4 | the address does not belong to `seed.pager` on that identity |
| 5 | the local Reticulum refused the packet |
