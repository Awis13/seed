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
| LoRa SX1262 | Present, **not driven yet** in this seed |

Pin map: `src/board_pins.h` (from arduino-esp32 `lilygo_tlora_pager` + LilyGoLib).

## Build & flash

```bash
cd seeds/esp32-tlora-pager

# Provision SPIFFS (not committed — copy the examples)
cp data/wifi.json.example data/wifi.json
cp data/auth_token.txt.example data/auth_token.txt
# edit ssid/password; token = 32 hex chars (or leave empty and the seed generates one)

pio run -e tlora-pager
pio run -e tlora-pager -t upload --upload-port /dev/cu.usbmodemXXXX
pio run -e tlora-pager -t uploadfs --upload-port /dev/cu.usbmodemXXXX
```

Native USB-Serial/JTAG: use `board_upload.after_reset = watchdog_reset` (already set). After flash, open `http://<ip>:8080/health`.

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

Auth: `Authorization: Bearer <token>` on everything except `/health`.

## On-device UI

- **Clock** — home (warm HH:MM, amber seconds, BAT %, unread badge, breathing rule on crit)
- **Encoder click** → MENU → MESSAGES / INFO / BACK
- **Card** — open from list; click = ack; type any key = free-text REPLY
- **Enter** on reply = send (`reply` field) + mark read

## Out of scope (this PR)

MeshCore / SX1262, GNSS, NFC, BQ25896 charger skill. The seed still exposes `/firmware/*` so an agent can grow those next.
