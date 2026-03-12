# Capabilities Specification

Standard contract for `GET /capabilities`. Every seed node MUST conform to this spec.

---

## Required fields

Every seed MUST return these fields:

| Field | Type | Description | Example |
|-------|------|-------------|---------|
| arch | string | CPU architecture | "armv6l", "esp32-s3", "pdp11", "x86_64" |
| mem_mb | number | Total RAM in megabytes | 427, 4, 512 |
| hostname | string | Node hostname or identifier | "seed-pi", "meshclaw-9cf4" |
| version | string | Firmware version (semver) | "1.0.0" |
| seed | boolean | true if running seed bootloader, false if grown firmware | true |
| endpoints | string[] | List of available HTTP endpoints | ["/health", "/capabilities", ...] |

---

## Optional fields

Seeds SHOULD return these if the hardware supports them:

| Field | Type | Description |
|-------|------|-------------|
| os | string | Operating system name |
| cpus | number | CPU core count |
| cpu_model | string | CPU model string |
| disk_mb | number | Total disk in MB |
| disk_free_mb | number | Free disk in MB |
| temp_c | number | CPU/board temperature in Celsius |
| board_model | string | Board/device model |
| has_gcc | boolean | gcc available for compilation |
| has_cc | boolean | cc available (PDP-11 uses PCC) |
| compiler | string | Compiler name if not gcc |
| has_wifi | boolean | WiFi available |
| has_bluetooth | boolean | Bluetooth available |
| net_interfaces | string[] | Network interface names |
| serial_ports | string[] | Serial port paths |
| gpio_chips | string[] | GPIO chip devices (Linux) |
| gpio_pins | number[] | Available GPIO pin numbers (ESP32) |
| i2c_buses | string[] | I2C bus paths or descriptions |
| int_bits | number | Integer width (16 for PDP-11, 32/64 for others) |
| kernel | string | Kernel version string |
| platform | string | Platform identifier (e.g. "2.11BSD") |
| gen | number | Firmware generation number |
| type | string | Legacy: node type. Use `arch` instead |
| modules | string[] | Legacy: available modules. Use `endpoints` instead |

---

## Notes

- `mem_mb` is always megabytes. If your platform reports KB (like PDP-11's 4096 KB), divide by 1024.
- `arch` should be the standard architecture string: "armv6l", "armv7l", "aarch64", "x86_64", "esp32", "esp32-s3", "pdp11", etc.
- `seed: true` means the bootloader is running. `seed: false` means grown firmware.
- `endpoints` lists all HTTP paths the node handles. Dashboard uses this to know what the node can do.
- Legacy fields (`type`, `modules`, `node`) are supported for backwards compatibility but new seeds should not use them.

---

## Migration

### ESP32 seed
- Add `arch: "esp32-s3"` (currently only has `type`)
- Add `mem_mb` with free heap / total heap
- Rename `node` -> `hostname`
- Add `endpoints` array (currently uses `modules`)
- Add `seed: true` and `version`

### PDP-11 seed
- Change `mem_kb: 4096` -> `mem_mb: 4`
- Keep `mem_kb` as optional legacy field

### Linux seed
- Already compliant. No changes needed.
