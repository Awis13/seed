# Seeds

Minimal firmware that bootstraps a node.

Each seed is a single compilation unit — one command to build, zero dependencies beyond libc.
It starts an HTTP server with just enough API for an AI agent to connect,
read the hardware, upload new C code, compile it on the device, and apply it.

The seed grows into whatever the node needs to be.

## Available seeds

| Platform | File | Build | Size |
|----------|------|-------|------|
| Linux (any with gcc) | `linux/seed.c` | `gcc -O2 -o seed seed.c` | ~1000 lines |
| ESP32 (Heltec V3) | `esp32/` | PlatformIO: `pio run` | ~1250 lines |
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
