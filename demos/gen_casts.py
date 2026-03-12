#!/usr/bin/env python3
"""
Generate asciinema .cast files for seed.c agent demos.

Three scenarios:
1. Recon agent — inspects node, leaves notes
2. Fix agent — reads notes, fixes issues
3. Grow agent — reads notes, adds GPIO skill

asciinema v2 format:
  Line 1: {"version": 2, "width": 120, "height": 35, "timestamp": ...}
  Lines 2+: [time, "o", "text"]
"""

import json
import time

PROMPT = "\x1b[1;32m❯ \x1b[0m"
COMMENT = "\x1b[38;5;242m"  # dim gray
CYAN = "\x1b[1;36m"
GREEN = "\x1b[1;32m"
YELLOW = "\x1b[1;33m"
RED = "\x1b[1;31m"
BOLD = "\x1b[1m"
RESET = "\x1b[0m"
CLEAR = "\x1b[2J\x1b[H"

def make_cast(filename, title, events):
    """Write a .cast file from a list of (delay, text) tuples."""
    header = {
        "version": 2,
        "width": 120,
        "height": 35,
        "timestamp": int(time.time()),
        "title": title,
        "env": {"TERM": "xterm-256color", "SHELL": "/bin/zsh"}
    }

    t = 0.0
    lines = [json.dumps(header)]

    for delay, text in events:
        t += delay
        lines.append(json.dumps([round(t, 3), "o", text]))

    with open(filename, 'w') as f:
        f.write('\n'.join(lines) + '\n')

    print(f"  {filename} — {len(events)} events, {t:.1f}s total")


def type_cmd(cmd, speed=0.03):
    """Simulate typing a command character by character."""
    events = []
    events.append((0.5, PROMPT))
    for ch in cmd:
        events.append((speed, ch))
    events.append((0.2, "\r\n"))
    return events


def show_output(text, delay=0.1):
    """Show command output line by line."""
    events = []
    for i, line in enumerate(text.split('\n')):
        d = delay if i == 0 else 0.02
        events.append((d, line + "\r\n"))
    return events


def comment(text, delay=0.8):
    """Show a comment line."""
    return [(delay, f"{COMMENT}# {text}{RESET}\r\n")]


def pause(seconds):
    return [(seconds, "")]


def banner(text):
    """Show a highlighted banner."""
    return [(0.5, f"\r\n{BOLD}{CYAN}{'═' * 60}{RESET}\r\n"),
            (0.1, f"{BOLD}{CYAN}  {text}{RESET}\r\n"),
            (0.1, f"{BOLD}{CYAN}{'═' * 60}{RESET}\r\n\r\n")]


# ============================================================
# DEMO 1: Recon Agent
# ============================================================
def demo_recon():
    e = []
    e += [(0.0, CLEAR)]
    e += banner("AGENT 1: RECON — Inspect node, leave notes for next agent")

    e += comment("First, read the skill file to understand the node")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" $NODE/skill | head -8')
    e += show_output(f"""{BOLD}# Seed Node: seed-pi{RESET}

Hardware node accessible via HTTP. This is a {BOLD}seed{RESET} — a minimal firmware
that you can grow by uploading new C source code.

> {YELLOW}FIRST: Run GET /notes — previous agents may have left tasks{RESET}""")
    e += pause(1.5)

    e += comment("Check notes from previous agents")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" $NODE/notes')
    e += show_output(f"{COMMENT}[]{RESET}")
    e += pause(0.5)
    e += comment("Empty — we're the first agent on this node")
    e += pause(1.0)

    e += comment("What hardware are we on?")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" $NODE/capabilities | jq \'{board: .board_model, arch: .arch, ram_mb: .mem_mb, temp: .temp_c, wifi: .has_wifi}\'')
    e += show_output(f"""{GREEN}{{
  "board": "Raspberry Pi Zero W Rev 1.1",
  "arch": "armv6l",
  "ram_mb": 427,
  "temp": 41.7,
  "wifi": true
}}{RESET}""")
    e += pause(1.0)

    e += comment("Check system health")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" $NODE/system/info | jq \'{hostname: .hostname, os: .os, uptime: .uptime, load: .load_1, temp_c: .temp_c}\'')
    e += show_output(f"""{GREEN}{{
  "hostname": "seed-pi",
  "os": "Raspbian GNU/Linux 13 (trixie)",
  "uptime": 85094,
  "load": 0.25,
  "temp_c": 40.6
}}{RESET}""")
    e += pause(1.0)

    e += comment("Check running services for problems...")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/exec -d \'{"cmd":"systemctl --failed"}\'| jq -r .stdout')
    e += show_output(f"""{RED}● claude-usb-bt-pan.service loaded failed failed claude-usb Bluetooth PAN

1 loaded units listed.{RESET}""")
    e += pause(1.5)

    e += comment("!! Found a failing service. How bad is it?")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/exec -d \'{"cmd":"systemctl show claude-usb-bt-pan -p NRestarts"}\' | jq -r .stdout')
    e += show_output(f"{RED}{BOLD}NRestarts=26587{RESET}")
    e += pause(1.5)
    e += comment("26,587 restarts! Critical — crash loop flooding syslog")
    e += pause(1.0)

    e += comment("Check network")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" $NODE/net/interfaces | jq \'.[] | select(.loopback==false) | {name, ip, state}\'')
    e += show_output(f"""{GREEN}{{ "name": "wlan0", "ip": "192.168.1.162", "state": "up" }}
{{ "name": "pan0", "ip": "10.88.0.1", "state": "unknown" }}{RESET}""")
    e += pause(1.0)

    e += comment("Any other nodes on the network?")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" "$NODE/net/probe?host=192.168.1.211&port=8080" | jq .')
    e += show_output(f"""{GREEN}{{
  "host": "192.168.1.211",
  "port": 8080,
  "open": true,
  "rtt_ms": 460.7
}}{RESET}""")
    e += pause(0.5)
    e += comment("ESP32 node detected at 192.168.1.211!")
    e += pause(1.5)

    e += banner("Leaving notes for the next agent")

    e += comment("CRITICAL: BT service crash loop")
    e += type_cmd("""curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/notes -d '{"title":"CRITICAL: bt-pan crash loop — 26,587 restarts","body":"sudo systemctl disable --now claude-usb-bt-pan.service","priority":"critical","agent":"recon-agent","tags":"systemd"}' | jq .""")
    e += show_output(f"""{GREEN}{{ "ok": true, "id": 1, "title": "CRITICAL: bt-pan crash loop — 26,587 restarts" }}{RESET}""")
    e += pause(0.5)

    e += comment("MEDIUM: No firewall")
    e += type_cmd("""curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/notes -d '{"title":"No firewall — API exposed on all interfaces","body":"Install ufw, allow SSH+8080 from LAN only","priority":"medium","agent":"recon-agent","tags":"security"}' | jq .""")
    e += show_output(f"""{GREEN}{{ "ok": true, "id": 2, "title": "No firewall — API exposed on all interfaces" }}{RESET}""")
    e += pause(0.5)

    e += comment("LOW: WiFi latency")
    e += type_cmd("""curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/notes -d '{"title":"WiFi power save causing 452ms gateway latency","body":"iw dev wlan0 set power_save off","priority":"low","agent":"recon-agent","tags":"network"}' | jq .""")
    e += show_output(f"""{GREEN}{{ "ok": true, "id": 3, "title": "WiFi power save causing 452ms gateway latency" }}{RESET}""")
    e += pause(1.0)

    e += comment("Verify notes saved")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" "$NODE/notes?status=open" | jq \'.[] | {id, priority, title}\'')
    e += show_output(f"""{YELLOW}{{ "id": 1, "priority": "critical", "title": "CRITICAL: bt-pan crash loop — 26,587 restarts" }}
{{ "id": 2, "priority": "medium", "title": "No firewall — API exposed on all interfaces" }}
{{ "id": 3, "priority": "low", "title": "WiFi power save causing 452ms gateway latency" }}{RESET}""")
    e += pause(1.5)

    e += comment("Done. 3 notes left for the next agent. Signing off.")
    e += pause(2.0)

    return e


# ============================================================
# DEMO 2: Fix Agent
# ============================================================
def demo_fix():
    e = []
    e += [(0.0, CLEAR)]
    e += banner("AGENT 2: FIX — Read notes, fix issues by priority")

    e += comment("Read work orders from previous agent")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" "$NODE/notes?status=open" | jq \'.[] | "\\(.priority): \\(.title)"\'')
    e += show_output(f"""{RED}"critical: CRITICAL: bt-pan crash loop — 26,587 restarts"{RESET}
{YELLOW}"medium: No firewall — API exposed on all interfaces"{RESET}
{GREEN}"low: WiFi power save causing 452ms gateway latency"{RESET}""")
    e += pause(1.5)

    e += banner("Fix #1: CRITICAL — Stop bt-pan crash loop")

    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/exec -d \'{"cmd":"sudo systemctl disable --now claude-usb-bt-pan.service 2>&1 && echo DONE"}\'| jq -r .stdout')
    e += show_output(f"Removed '/etc/systemd/system/multi-user.target.wants/claude-usb-bt-pan.service'.\n{GREEN}DONE{RESET}")
    e += pause(0.5)

    e += comment("Verify it's dead")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/exec -d \'{"cmd":"systemctl is-active claude-usb-bt-pan"}\'| jq -r .stdout')
    e += show_output(f"{GREEN}inactive{RESET}")
    e += pause(0.5)

    e += comment("Mark as done")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/notes/update -d \'{"id":1,"status":"done","body":"Disabled. 26,587 restarts stopped."}\'| jq .')
    e += show_output(f"""{GREEN}{{ "ok": true, "id": 1, "status": "done" }}{RESET}""")
    e += pause(1.0)

    e += banner("Fix #2: MEDIUM — Install firewall")

    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/exec -d \'{"cmd":"sudo apt install -y ufw > /dev/null 2>&1 && sudo ufw default deny incoming && sudo ufw allow from 192.168.1.0/24 to any port 22 && sudo ufw allow from 192.168.1.0/24 to any port 8080 && sudo ufw --force enable && echo DONE","timeout":60}\'| jq -r .stdout')
    e += show_output(f"""Default incoming policy changed to 'deny'
Rule added
Rule added
Firewall is active and enabled on system startup
{GREEN}DONE{RESET}""", delay=3.0)
    e += pause(0.5)

    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/notes/update -d \'{"id":2,"status":"done","body":"ufw installed. SSH+API open for LAN only."}\'| jq .')
    e += show_output(f"""{GREEN}{{ "ok": true, "id": 2, "status": "done" }}{RESET}""")
    e += pause(1.0)

    e += banner("Fix #3: LOW — WiFi latency")

    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/exec -d \'{"cmd":"sudo iw dev wlan0 set power_save off && echo DONE"}\'| jq -r .stdout')
    e += show_output(f"{GREEN}DONE{RESET}")
    e += pause(0.3)

    e += comment("Verify: ping gateway before vs after")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" "$NODE/net/ping?host=192.168.1.1" | jq \'{alive: .alive, rtt_ms: .rtt_avg_ms}\'')
    e += show_output(f"""{GREEN}{{
  "alive": true,
  "rtt_ms": 4.8
}}{RESET}""")
    e += pause(0.5)
    e += comment("452ms → 5ms. 90x improvement.")
    e += pause(0.5)

    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/notes/update -d \'{"id":3,"status":"done","body":"Power save off. RTT 452ms → 5ms."}\'| jq .')
    e += show_output(f"""{GREEN}{{ "ok": true, "id": 3, "status": "done" }}{RESET}""")
    e += pause(1.0)

    e += comment("Final status check")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" $NODE/notes | jq \'.[] | "\\(.status): \\(.title)"\'')
    e += show_output(f"""{GREEN}"done: CRITICAL: bt-pan crash loop — 26,587 restarts"
"done: No firewall — API exposed on all interfaces"
"done: WiFi power save causing 452ms gateway latency"{RESET}""")
    e += pause(1.0)
    e += comment("All issues resolved. Node is clean.")
    e += pause(2.0)

    return e


# ============================================================
# DEMO 3: Grow Agent — adds GPIO skill
# ============================================================
def demo_grow():
    e = []
    e += [(0.0, CLEAR)]
    e += banner("AGENT 3: GROW — Read firmware, add GPIO skill, deploy")

    e += comment("Check what this node can do")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" $NODE/capabilities | jq \'{board: .board_model, gpio: .gpio_chips, skills: .endpoints | length}\'')
    e += show_output(f"""{GREEN}{{
  "board": "Raspberry Pi Zero W Rev 1.1",
  "gpio": ["/dev/gpiochip0", "/dev/gpiochip4"],
  "skills": 31
}}{RESET}""")
    e += pause(1.0)

    e += comment("GPIO hardware exists but no GPIO skill! Let's grow one.")
    e += comment("Read current firmware source...")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" $NODE/firmware/source | wc -c')
    e += show_output("118821")
    e += pause(0.5)

    e += comment("Save locally, add GPIO skill code...")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" $NODE/firmware/source > /tmp/seed.c && echo "Source downloaded"')
    e += show_output(f"{GREEN}Source downloaded{RESET}")
    e += pause(1.0)

    e += comment("Inject GPIO skill (sysfs-based pin control)...")
    e += type_cmd('cat >> /tmp/seed.c << \'SKILL\'  # ... 120 lines of GPIO skill code ...')
    e += show_output(f"""{COMMENT}/* skills/gpio.c — GPIO pin control via sysfs */
static int gpio_handle_pins(int fd, http_req_t *req) {{ ... }}
static int gpio_handle_setup(int fd, http_req_t *req) {{ ... }}
static int gpio_handle_write(int fd, http_req_t *req) {{ ... }}
static int gpio_handle_read(int fd, http_req_t *req) {{ ... }}
/* ... 120 lines ... */{RESET}""", delay=1.5)
    e += pause(1.0)

    e += banner("Deploy: Upload → Build → Apply")

    e += comment("Upload modified source")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: text/plain" --data-binary @/tmp/seed.c -X POST $NODE/firmware/source | jq .')
    e += show_output(f"""{GREEN}{{
  "ok": true,
  "bytes": 130457,
  "path": "/opt/seed/seed.c"
}}{RESET}""")
    e += pause(0.8)

    e += comment("Compile ON the device")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/firmware/build | jq .')
    e += show_output(f"""{GREEN}{{
  "ok": true,
  "exit_code": 0,
  "binary": "/opt/seed/seed-new",
  "size_bytes": 76196
}}{RESET}""", delay=3.0)
    e += pause(0.8)

    e += comment("Apply with watchdog (auto-rollback if health check fails)")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/firmware/apply | jq .')
    e += show_output(f"""{YELLOW}{{
  "ok": true,
  "warning": "restarting with watchdog, 45s grace period"
}}{RESET}""")
    e += pause(1.5)

    e += comment("Waiting for watchdog to confirm new firmware...")
    e += [(1.0, f"{COMMENT}")]
    for i in range(8):
        e += [(0.5, ".")]
    e += [(0.5, f"{RESET}\r\n")]
    e += pause(0.5)

    e += type_cmd('curl -s $NODE/health | jq .')
    e += show_output(f"""{GREEN}{{
  "ok": true,
  "uptime_sec": 52,
  "type": "seed",
  "version": "0.2.0",
  "seed": true,
  "arch": "armv6l"
}}{RESET}""")
    e += pause(1.0)
    e += comment("Node survived! Watchdog confirmed. Let's test GPIO.")
    e += pause(1.0)

    e += banner("Test: Control a physical GPIO pin")

    e += comment("Setup pin 17 as output")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/gpio/setup -d \'{"pin":17,"direction":"out"}\' | jq .')
    e += show_output(f"""{GREEN}{{ "ok": true, "pin": 17, "direction": "out" }}{RESET}""")
    e += pause(0.5)

    e += comment("Turn it ON")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/gpio/write -d \'{"pin":17,"value":1}\' | jq .')
    e += show_output(f"""{GREEN}{{ "ok": true, "pin": 17, "value": 1 }}{RESET}""")
    e += pause(0.5)

    e += comment("Read it back")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" "$NODE/gpio/read?pin=17" | jq .')
    e += show_output(f"""{GREEN}{{ "pin": 17, "value": 1, "direction": "out" }}{RESET}""")
    e += pause(0.5)

    e += comment("List all active pins")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" $NODE/gpio/pins | jq .')
    e += show_output(f"""{GREEN}{{
  "sysfs_base": 512,
  "pins": [
    {{ "pin": 17, "direction": "out", "value": 1 }}
  ]
}}{RESET}""")
    e += pause(1.0)

    e += comment("Clean up")
    e += type_cmd('curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/gpio/unexport -d \'{"pin":17}\' | jq .')
    e += show_output(f"""{GREEN}{{ "ok": true }}{RESET}""")
    e += pause(1.0)

    e += comment("Leave a note for future agents")
    e += type_cmd("""curl -s -H "Authorization: Bearer $TOKEN" -X POST $NODE/notes -d '{"title":"GPIO skill added — 5 endpoints for pin control","body":"BCM pins 0-27, sysfs interface, auto base offset","priority":"info","agent":"grow-agent"}' | jq .""")
    e += show_output(f"""{GREEN}{{ "ok": true, "id": 4, "title": "GPIO skill added — 5 endpoints for pin control" }}{RESET}""")
    e += pause(1.5)

    e += comment("The node just grew. From 31 to 36 endpoints. No reboot, no downloader, no SDK.")
    e += comment("Just C, HTTP, and a 45-second watchdog.")
    e += pause(3.0)

    return e


# ============================================================
# Generate all three
# ============================================================
if __name__ == '__main__':
    print("Generating asciinema casts...")
    base = "/Users/awis/claude/seed/demos"
    make_cast(f"{base}/01_recon.cast", "seed.c — Agent 1: Recon", demo_recon())
    make_cast(f"{base}/02_fix.cast", "seed.c — Agent 2: Fix", demo_fix())
    make_cast(f"{base}/03_grow.cast", "seed.c — Agent 3: Grow", demo_grow())
    print("\nPlay with: asciinema play demos/01_recon.cast")
    print("Or upload: asciinema upload demos/01_recon.cast")
