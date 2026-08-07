# ESP32 Seed — LILYGO T-Lora Pager 868

AI-growable seed for the **LILYGO T-Lora Pager** (ESP32-S3, 16 MB flash, 8 MB QSPI PSRAM).

This is a full **pocket pager** port of the seed stack: HTTP notify inbox, Advisor-style clock face, encoder menu, progress bar, haptic (DRV2605), speaker beeps (ES8311), free-text reply from the TCA8418 keyboard, and **BQ27220 battery %** on the clock header.

## Hardware (verified)

| Part | Notes |
|------|--------|
| ESP32-S3 | External QSPI flash + PSRAM (`qio_qspi`) |
| Display | ST7796 480×222, AW9364 backlight on GPIO42 |
| I2C | SDA=3 SCL=2 — XL9555, BQ25896/BQ27220, TCA8418, ES8311, DRV2605, … |
| Fuel gauge | BQ27220 @ 0x55 — Voltage + SoC on clock (`BAT n%`) + `/capabilities` |
| XL9555 @ 0x20 | Power rails for LoRa/GPS/NFC/KB/SD/amp (raised at boot) |
| Encoder | A=40 B=41 C=7 |
| Keyboard | TCA8418 4×10 @ 0x34, INT=6 |
| LoRa SX1262 | **MeshCore private-link** (same RF as home Heltec companion) |
| MeshCore | Identity + pair in SPIFFS; private DM only (never Public) |

Pin map: `src/board_pins.h` (from arduino-esp32 `lilygo_tlora_pager` + LilyGoLib).

## Build & flash

```bash
cd seeds/esp32-tlora-pager

# Provision SPIFFS (not committed — copy the examples)
cp data/wifi.json.example data/wifi.json
cp data/auth_token.txt.example data/auth_token.txt
# edit ssid/password; token = 32 hex chars (or leave empty and the seed generates one)

# Optional MeshCore pair (written by the device after pair, or provisioned):
#   data/mesh_identity.id  — ed25519 secret (NEVER commit)
#   data/mesh_pair.json    — gateway pubkey + name (NEVER commit)

pio run -e tlora-pager
pio run -e tlora-pager -t upload --upload-port /dev/cu.usbmodemXXXX
pio run -e tlora-pager -t uploadfs --upload-port /dev/cu.usbmodemXXXX
```

Native USB-Serial/JTAG: use `board_upload.after_reset = watchdog_reset` (already set). After flash, open `http://<ip>:8080/health`.

Secrets under `data/` (`wifi.json`, `auth_token.txt`, `mesh_identity.id`, `mesh_pair.json`) are gitignored.

## API (pager core)

| Method | Path | Role |
|--------|------|------|
| POST | `/notify` | Queue message `{level,title,body,source,options?,id?}` |
| GET | `/notify` | List + unread count |
| GET | `/notify/one?id=N` | One entry; includes `reply` if typed on device |
| POST | `/notify/ack` | `{"id":N}` or `{"all":true}` |
| POST | `/progress` | `{label,percent,ttl_s}` or `{label,done:true}` |
| GET | `/capabilities` | Hardware + what this build drives |
| GET | `/skill` | Agent-facing docs |
| GET | `/meshcore/status` | Pair + radio + last-ok age (MeshCore skill) |

Auth: `Authorization: Bearer <token>` on everything except `/health`.

**Canonical dual-path notify egress** is the home gateway (`POST :8325/notify-out` — WiFi + private mesh DM). Services should not invent extra mesh endpoints on the pager; the pager is inbox + status.

## MeshCore private link

- **Radio:** SX1262, 869.618 MHz SF8 BW62.5 TX22 (aligned with Heltec companion).
- **Stack:** vendored MeshCore subset in `lib/meshcore` + ed25519 in `lib/ed25519`; client in `src/mesh/`; skill in `src/skills/meshcore.cpp`.
- **Wire (private DM text):**
  - `P1|…` — short notify → same cards as WiFi `POST /notify`
  - `M1|mid|i|n|level|source|title|chunk` — multi-part long notify
  - `C1|agent|mid|i|n|side|chunk` — bidirectional agents chat (`u` uplink / `a` agent)
  - Keepalive `MC|k` / pong `MC|a`
- **Clock:** `M` link glyph + age (`M0m` / `M12m` / `M2h` / `M--` if stale).
- **SPI:** shares display SPI (`hw_ui_spi()`); radio init deferred ~5s so boot never hangs on the bus.

## On-device UI

- **Clock** — home (warm HH:MM, amber seconds, BAT %, unread badge, MeshCore `M`+age, breathing rule on crit)
- **Encoder click** → MENU → MESSAGES / **AGENTS** / **MESHCORE** / **SETTINGS** / INFO / BACK
- **MESHCORE** — STATUS (pair/radio/last-ok) · PING GATEWAY (WiFi RTT to `:8325`) · pair info
- **Inbox list** — Nokia/pager style: `*` + bright title = NEW, dim = read; `I`/`W`/`C` severity chip; header `*NN NEW`
- **Card (severity)** — real pages from any service: `info` (teal) / `warn` (amber) / `crit` (red); click/Enter = ACKNOWLEDGE · REPLY · BACK; type = free-text REPLY
- **Chat door** — only when client `id` ends with `-chat` (e.g. bridge posts `hermes-chat`): pretty CHAT invite → opens AGENTS room. Not a severity page.
- **AGENTS** — Grok / Claude / Hermes chat threads; replies go WiFi bridge when online, else mesh uplink `C1`
- **Keyboard layouts** (SETTINGS → LAYOUT, persist `/kb_layout.txt`):
  - **ABC** — Latin
  - **RU PHON** — Apple `Russian - Phonetic` (Mac muscle memory: `privet` → привет)
  - **RU** — standard ЙЦУКЕН
  - Live cycle: **ALT+CAPS**; badge on REPLY shows `ABC`/`PHON`/`RU`
  - Panel font is UTF-8 (ASCII + Cyrillic). Backspace deletes one codepoint.
- **Enter** on reply = send (`reply` field or agent thread)

**Two inboxes, one queue API:** `POST /notify` always lands in MESSAGES. Routing on device: `id: "*-chat"` → chat door UI; anything else (any `source`, any agent/service) → coloured severity card + reply. Mesh `P1`/`M1` frames feed the same notify store.

## Out of scope (this seed)

GNSS, NFC, BQ25896 charger skill. Gateway daemon / agent bridge live outside this repo (home-rig).
