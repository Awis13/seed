# Firmware

Grown firmware — what seeds become after an AI agent evolves them.

Each firmware started as a seed and was grown through the `/firmware/*` API.
The growth process is preserved: every firmware still has the seed API,
so it can be grown again. And again.

## Available firmware

### linux-node (7300+ lines, grown from 1000-line seed)

Full node for Linux. Grown capabilities:

- **Network recon**: ARP scan, mDNS browse, WiFi scan, ping, HTTP probe
- **Filesystem access**: ls, read, write (path-restricted)
- **System monitoring**: process list, port scan, interface stats
- **Firmware vault**: store, list, push firmware binaries across nodes
- **Fleet management**: broadcast commands to all nodes, aggregate status
- **Agent notes**: inter-agent message board (dead drop between sessions)
- **USB gadget**: HID keyboard, serial, Ethernet (Pi Zero W)
- **Security hardened**: input validation, path restriction, no shell injection

### esp32-node (2700+ lines, grown from 1250-line seed)

IoT node for ESP32 Heltec V3. Grown capabilities:

- **GPIO control**: read/write digital pins
- **I2C scanning**: detect connected devices
- **WireGuard VPN**: full userspace WG with peer management
- **Deploy system**: receive and flash firmware binaries
- **Mesh discovery**: find other nodes on the network
- **LoRa radio**: send/receive over LoRa (Heltec V3 onboard SX1262)

### pdp11-gen1 (documented in growth log)

The PDP-11 seed was grown live on 2.11BSD through the HTTP API.
See [docs/pdp11-growth-log.md](../docs/pdp11-growth-log.md) for the full evolution record.

Gen 1 added: /system/info, /system/processes, /system/disk, /system/who, /system/logs.

## Growth pattern

Every firmware follows the same structure as its seed — same HTTP parser,
same auth, same handler pattern. New endpoints are added by inserting
route handlers before the 404 fallback. The `/firmware/*` API is always preserved.
