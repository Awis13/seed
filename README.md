# seed.c

AI-growable firmware. One C file. Any hardware.

```
gcc -O2 -o seed seed.c && ./seed
```
```
Seed v0.1.0
Token: <generated on first run>

Connect:
  http://192.168.1.x:8080/health
  http://localhost:8080/health

Skill file: GET /skill
API list:   GET /capabilities
```

An AI agent connects, reads the hardware, writes new firmware,
compiles it on the device. The node grows into whatever it needs to be.

10-second watchdog — bad firmware auto-reverts. The device is never bricked.

## How it works

```
seed (1000 lines)
  |
  |  AI connects
  |  GET /capabilities  ->  { arch, cpu, ram, gpio, i2c, serial, ... }
  |
  |  AI writes firmware
  |  POST /firmware/source  <-  new C code
  |  POST /firmware/build   ->  compiles on the device
  |  POST /firmware/apply   ->  atomic binary swap
  |
  |  10s watchdog: health check fails -> auto-rollback
  |
  v
Full node (whatever the AI wrote)
  — network recon, fleet management, serial bridge,
  — GPIO control, WireGuard mesh, system monitoring,
  — or anything else. The AI decides.
  — still has /firmware/* — can be grown again.
```

## Project structure

```
seeds/                          <- minimal bootstraps
  linux/seed.c                     1000 lines, gcc, any Linux
  esp32/                           1250 lines, PlatformIO, Heltec V3
  pdp11/seed.c                      550 lines, K&R C, 2.11BSD

firmware/                       <- grown from seeds
  linux-node/server.c              7300 lines — net recon, fleet, vault, notes, USB gadget
  esp32-node/                      2700 lines — GPIO, I2C, WireGuard, LoRa, deploy

docs/
  pdp11-growth-log.md              How we grew a PDP-11 from 1975

SKILL.md                        <- AI agent skill file
```

Seeds are the starting point. Firmware is what they become.
Every firmware preserves the `/firmware/*` API — so it can be grown again.

## Quick start

**Any Linux box:**
```bash
gcc -O2 -o seed seeds/linux/seed.c
./seed
curl http://localhost:8080/health
```

**Raspberry Pi:**
```bash
scp seeds/linux/seed.c pi@raspberrypi:~
ssh pi@raspberrypi 'gcc -O2 -o seed seed.c && ./seed'
```

**ESP32 Heltec V3:**
```bash
cd seeds/esp32
pio run -t upload
```

**PDP-11 (2.11BSD):**
```
cc -o seed seed.c
./seed 8080
```

The startup banner shows the token and all addresses. No config needed.

## For AI agents

```bash
curl http://<ip>:8080/skill
```

The `/skill` endpoint generates a complete skill file: connection details,
auth token, all endpoints, code patterns, constraints, capabilities example.
An agent can grow the node with no other context.

## Seed API

All requests except `/health` require `Authorization: Bearer <token>`.
Token is generated on first run, printed in the banner, saved to disk.

| Method | Path | What it does |
|--------|------|-------------|
| GET | `/health` | Alive check, no auth |
| GET | `/capabilities` | Hardware fingerprint — CPU, RAM, GPIO, I2C, serial, USB, WiFi, BT |
| GET | `/config.md` | Node description (markdown) |
| POST | `/config.md` | Update description |
| GET | `/events` | Event log (`?since=<unix_ts>`) |
| GET | `/firmware/version` | Version, build date, uptime |
| GET | `/firmware/source` | Read the running source code |
| POST | `/firmware/source` | Upload new source (C in request body) |
| POST | `/firmware/build` | Compile on the device |
| GET | `/firmware/build/logs` | Compiler output |
| POST | `/firmware/apply` | Hot-swap binary + 10s watchdog rollback |
| POST | `/firmware/apply/reset` | Unlock apply after 3 consecutive failures |
| GET | `/skill` | Generate AI agent skill file |

## What seeds grow into

The `firmware/` directory contains real examples of grown firmware:

**Linux node** (7300 lines, from 1000-line seed):
network scanning, mDNS discovery, WiFi recon, filesystem access,
fleet management, firmware vault, agent notes, USB HID keyboard,
serial bridge, security hardening.

**ESP32 node** (2700 lines, from 1250-line seed):
GPIO control, I2C device scanning, WireGuard VPN with peer management,
LoRa radio, OTA deploy system, mesh discovery.

**PDP-11 Gen 1** (grown live on 2.11BSD):
system monitoring, process list, disk usage, syslog viewer.
[Full growth log](docs/pdp11-growth-log.md).

## The PDP-11 experiment

We grew a PDP-11 from 1975 through HTTP.

The seed was compiled on 2.11BSD with `cc` (PCC compiler, K&R C).
An AI agent connected, wrote new firmware respecting 16-bit integer limits
and pre-ANSI C syntax, compiled it on the node, and applied it
with automatic watchdog rollback.

Three generations. Each compiled the next through the same API.
The node evolved from a 550-line seed into a system monitor
with process listing, disk stats, and syslog access.

If it works on hardware from 1975, it works on everything.

## Security

Seed runs on your hardware, on your network. No cloud, no phone-home.

- **Auth token** — 32 random hex bytes from `/dev/urandom`
- **Watchdog** — 10 seconds to pass health check, or auto-rollback
- **Failure lock** — 3 failed applies -> locked until manual reset
- **No shell injection** — all inputs validated
- **Audit log** — every action logged to `/events` with timestamps

**This is remote code execution by design.** An AI agent compiles and runs
C code on your device. The watchdog ensures bad code is killed in 10 seconds.
Run seed on hardware you control, on networks you trust.

## Hardware

| Device | Cost | What for |
|--------|------|----------|
| Any Linux box / VPS | $0 | seed.c compiles anywhere with gcc |
| Raspberry Pi Zero W | $10 | USB dongle — plug into any computer |
| Any Linux SBC | $5-50 | GPIO, I2C, serial — physical world |
| ESP32 (Heltec V3) | $15 | WiFi, BLE, LoRa, WireGuard |
| ESP32-S3 | $5 | WiFi/BLE mesh node |
| PDP-11 (2.11BSD) | priceless | Because why not |

## Writing firmware

C only. libc only. No external libraries.
Single-threaded. One request at a time.

Handler pattern:
```c
if (strcmp(req.path, "/myendpoint") == 0
    && strcmp(req.method, "GET") == 0) {
    char resp[4096];
    snprintf(resp, sizeof(resp), "{\"key\":\"value\"}");
    json_resp(fd, 200, "OK", resp);
    goto done;
}
```

Available helpers: `json_resp()`, `text_resp()`, `respond()`,
`file_read()`, `file_write()`, `cmd_out()`, `event_add()`.

Request fields: `req.method`, `req.path`, `req.body`,
`req.body_len`, `req.content_length`.

Auth is checked before your handler runs.
`/health` is always public.

## License

MIT
