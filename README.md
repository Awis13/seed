# seed.c

A bootloader for AI. Firmware management over HTTP. One C file. Any hardware.

```bash
gcc -O2 -o seed seed.c && ./seed

#  Seed v0.2.0
#  Token: 7e54b046...
#
#  Connect:
#    http://192.168.1.x:8080/health
#    http://localhost:8080/health
```

An AI agent connects, reads the hardware, writes new firmware,
compiles it on the device. The node grows into whatever it needs to be.

Watchdog — bad firmware auto-reverts. The device is never bricked.

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
  |  watchdog: health check fails -> auto-rollback
  |
  v
Full node (whatever the AI wrote)
  — network recon, fleet management, serial bridge,
  — GPIO control, WireGuard mesh, system monitoring,
  — or anything else. The AI decides.
  — still has /firmware/* — can be grown again.
```

## Watch: three AI agents, one Raspberry Pi, zero shared context

Three autonomous agents were dropped onto a live Pi Zero W.
They don't share memory — they communicate through the node itself using `/notes`.

**Agent 1: Recon** — inspects hardware, finds issues, leaves notes for the next agent.

![Agent 1: Recon](demos/01_recon.gif)

**Agent 2: Fix** — reads notes, fixes every issue (killed Bluetooth crash loop, hardened firewall, fixed WiFi latency).

![Agent 2: Fix](demos/02_fix.gif)

**Agent 3: Grow** — reads notes, writes a GPIO skill in C, deploys new firmware with watchdog.

![Agent 3: Grow](demos/03_grow.gif)

## Project structure

```
seeds/                          <- bootloaders
  linux/
    seed.c                           1135 lines, gcc, any Linux
    skills/sysmon.c                  system monitoring skill
    skills/example.c                 reference skill template
  esp32/
    src/main.cpp                     1252 lines, PlatformIO, Heltec V3
    src/skills/gpio.cpp              GPIO control skill (360 lines)
  pdp11/seed.c                      550 lines, K&R C, 2.11BSD

firmware/                       <- grown from seeds
  linux-node/server.c              7300 lines — net recon, fleet, vault, notes, USB gadget
  esp32-node/                      2700 lines — GPIO, I2C, WireGuard, LoRa, deploy

docs/
  capabilities-spec.md             Standard /capabilities contract
  pdp11-growth-log.md              How we grew a PDP-11 from 1975
  pdp11-console-log.md             Raw console output from the session

SKILL.md                        <- AI agent skill file
```

Seeds are bootloaders. Firmware is what gets loaded.
Every firmware preserves the `/firmware/*` API — so it can be grown again.

## Skills

Skills are modular capabilities — drop a `.c` file into `skills/`, include it, and the node
gains new endpoints. Skills auto-register in `/capabilities` and `/skill`.

```
skills/
  sysmon.c      — /system/info, /system/processes, /system/disk, /system/logs
  gpio.cpp      — /gpio/list, /gpio/read, /gpio/write, /gpio/mode, /gpio/adc
  example.c     — reference template for writing new skills
```

Each skill is one file that exports: name, version, `describe()` (markdown docs),
endpoint list, and a handler. The seed dispatches requests to registered skills automatically.

**Writing a skill:**
```c
static const skill_endpoint_t my_endpoints[] = {
    { "GET", "/my/endpoint", "What it does" },
    { NULL, NULL, NULL }
};

static const char *my_describe(void) {
    return "## Skill: myskill\n\nMarkdown documentation.\n";
}

static int my_handle(int fd, http_req_t *req) {
    if (strcmp(req->path, "/my/endpoint") == 0) {
        json_resp(fd, 200, "OK", "{\"ok\":true}");
        return 1;  // handled
    }
    return 0;  // pass to next skill
}

static const skill_t my_skill = {
    .name = "myskill", .version = "0.1.0",
    .describe = my_describe, .endpoints = my_endpoints, .handle = my_handle
};
```

`GET /skill` on any node returns full documentation for all installed skills.

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
| GET | `/capabilities` | Hardware fingerprint ([spec](docs/capabilities-spec.md)) |
| GET | `/config.md` | Node description (markdown) |
| POST | `/config.md` | Update description |
| GET | `/events` | Event log (`?since=<unix_ts>`) |
| GET | `/firmware/version` | Version, build date, uptime |
| GET | `/firmware/source` | Read the running source code |
| POST | `/firmware/source` | Upload new source (C in request body) |
| POST | `/firmware/build` | Compile on the device |
| GET | `/firmware/build/logs` | Compiler output |
| POST | `/firmware/apply` | Hot-swap binary + watchdog rollback |
| GET | `/firmware/apply/status` | Watchdog progress (poll after apply) |
| POST | `/firmware/rollback` | Manual rollback to backup binary |
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

The node evolved from a 550-line seed through two bootstraps
into a system monitor with process listing, disk stats, and syslog access.
Two failed Gen 2 attempts were caught by the watchdog and auto-rolled back.

If it works on hardware from 1975, it works on everything.

## Security

Seed runs on your hardware, on your network. No cloud, no phone-home.

- **Auth token** — 32 random hex bytes from `/dev/urandom`
- **Watchdog** — 120 seconds to pass health check, or auto-rollback
- **Failure lock** — 3 failed applies -> locked until manual reset
- **No shell injection** — all inputs validated
- **Audit log** — every action logged to `/events` with timestamps

**This is a firmware management API.** The same `source → build → apply` cycle
works whether the caller is an AI agent, a human with curl, or a CI/CD pipeline.
Think OTA updates without the cloud — like balena, but 40KB and self-hosted.

The watchdog ensures bad firmware is caught and rolled back automatically.
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

C only. libc only. No external libraries. Single file. `gcc -O2 -o seed seed.c` must work.

See [SKILL.md](SKILL.md) for the full API reference, handler patterns, and constraints.
AI agents get this automatically from `GET /skill` on any running node.

## License

MIT
