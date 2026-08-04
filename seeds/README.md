# Seeds

Bootloaders for AI agents.

Each seed is a single compilation unit — one command to build, zero dependencies beyond libc.
It starts an HTTP server with just enough API for an AI agent to connect,
read the hardware, upload new C code, compile it on the device, and apply it.

The seed grows into whatever the node needs to be.

## Available seeds

| Platform | File | Build | Size |
|----------|------|-------|------|
| Linux (any with gcc) | `linux/seed.c` | `gcc -O2 -o seed seed.c` | ~1000 lines |
| ESP32 (Heltec V3) | `esp32/` | PlatformIO: `pio run` | ~1250 lines |
| ESP32-S3 (LilyGO T-Embed CC1101) | `esp32-tembed/` | PlatformIO: `pio run -e tembed` | ~1900 lines |
| PDP-11 (2.11BSD) | `pdp11/seed.c` | `cc -o seed seed.c` | ~550 lines |

## Seed API

Every seed implements the same core protocol:

| Method | Path | Description |
|--------|------|-------------|
| GET | /health | Alive check (no auth) |
| GET | /capabilities | Hardware fingerprint |
| GET | /config.md | Node configuration |
| POST | /config.md | Update configuration |
| GET | /events | Event log |
| GET | /firmware/version | Version + uptime |
| GET | /firmware/source | Read running source code |
| POST | /firmware/source | Upload new source |
| POST | /firmware/build | Compile on device |
| GET | /firmware/build/logs | Compiler output |
| POST | /firmware/apply | Hot-swap + watchdog rollback |
| GET | /skill | AI agent skill file |

Auth: `Authorization: Bearer <token>` on all endpoints except `/health`.
Token is generated on first run and printed in the startup banner.

## How growth works

```
seed.c (1000 lines)
  | AI connects, reads /capabilities
  | AI writes new firmware with added endpoints
  | POST /firmware/source -> POST /firmware/build -> POST /firmware/apply
  | 10-second watchdog: health check fails -> auto-rollback
  v
grown firmware (keeps /firmware/* API — can be grown again)
```

See `firmware/` for examples of what seeds grow into.

## Third-party data

One file in this tree is not MIT:

| File | Upstream | License | Text |
|------|----------|---------|------|
| `esp32-tembed/src/skills/ir_codes_tvbgone.h` | [Arduino-TV-B-Gone](https://github.com/shirriff/Arduino-TV-B-Gone), `WORLD_IR_CODES.h` | `CC-BY-SA-2.5` | [`esp32-tembed/LICENSE-CC-BY-SA`](esp32-tembed/LICENSE-CC-BY-SA) |

It holds the TV-B-Gone power-off code database — 270 captured TV power codes in
the NA and EU sets the original device ships. TV-B-Gone firmware is
(c) Mitch Altman and Limor Fried, 2009; the Arduino port these codes come from
is by Ken Shirriff; the NA/EU split is by Mitch Altman.

The firmware source states its own terms — *"Distributed under Creative Commons
2.5 -- Attib & Share Alike"* — and the Arduino port's README repeats it. Note
that Adafruit's `TV-B-Gone-kit` repository ships the CC BY-SA **3.0** Unported
text for the same firmware, so upstream is inconsistent about the version;
`LICENSE-CC-BY-SA` carries that 3.0 text verbatim and cites the URI for 2.5,
which satisfies both.

The file contains data and nothing else — no logic reads it, decodes it or
transmits it there. That is deliberate: it keeps the share-alike obligation
attached to the table, so the code that consumes it
(`esp32-tembed/src/skills/ir.cpp`) stays MIT along with the rest of the
repository. If you reuse that table, it travels under CC BY-SA with the
attribution in its header intact.
