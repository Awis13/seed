# Deploying the Linux seed

Runbook for putting the seed on a home Linux box (example host below:
home-rig; nothing is hardcoded in the scripts).

## Install / upgrade

```sh
git clone https://github.com/Awis13/seed && cd seed   # or git pull
sudo seeds/linux/deploy/install.sh
```

Idempotent — re-running upgrades the binary, source copy and unit, and
never touches data files. The previous `/opt/seed/seed.c` is kept as
`seed.c.pre-install` first, so a node whose source was grown via
`/firmware/source` does not lose it on re-install; an existing backup is
never overwritten (first backup wins — it is the oldest grown source).
On a box without a running systemd (e.g. a container) it installs
everything and prints manual start steps instead.

## Verify

```sh
curl http://localhost:8080/health
TOKEN="$(sudo cat /opt/seed/token)"
curl -H "Authorization: Bearer $TOKEN" http://localhost:8080/skill | head -30
```

From another machine, replace localhost with the host's LAN address
(e.g. `http://192.168.1.138:8080`).

## Where data lives

All under `/opt/seed/`:

| File | What |
|------|------|
| `token` | Bearer token, generated 0600 on first start — do not regenerate |
| `drop.jsonl` | Append-only message drop store |
| `drop-cursors.json` | Per-handle drop read cursors |
| `notes.json` | Agent dead-drop notes |
| `config.md` | Node configuration |
| `apply_failures` | Consecutive firmware-apply failure counter (3 locks apply) |
| `apply_status` | Apply watchdog progress, served by /firmware/apply/status |
| `seed`, `seed.c`, `seed.bak`, `build.log` | Binary, its source, apply-flow artifacts |
| `seed.c.pre-install` | Previous source, saved by install.sh before overwriting |

`install.sh` replaces only `seed`, `seed.c` and the unit.

## Security model (deliberate design decision)

**Loopback is unauthenticated by server design** — requests from 127.0.0.1
skip the token check (trusted-box assumption: whoever is on the box owns
it anyway). Everything else on the LAN must send
`Authorization: Bearer <token>`, rate-limited on failures. Do not expose
port 8080 beyond the LAN; the seed's whole point (`/exec`, `/fs`,
self-recompile) makes it root-equivalent on its host.

## Unit hardening — what is and is not there

- `KillMode=process`: required — the firmware-apply watchdog is a forked
  child that itself runs `systemctl restart seed` and must survive it to
  health-check and roll back. Side effect: long-running `/exec` children
  also survive a restart.
- `NoNewPrivileges=yes`: `/exec` children cannot escalate via setuid
  binaries (so `sudo` from `/exec` will not work; the service already
  runs as root).
- Left out deliberately, each would silently break a shipped skill:
  `ProtectSystem`/`ProtectHome` (the `/fs` skill writes arbitrary paths),
  `PrivateDevices` (`/gpio` needs `/dev/gpiochipN`, `/serial` needs
  `/dev/tty*`), `PrivateTmp` (`/exec` output under /tmp would be
  invisible to the user), `DynamicUser`/`User=` (token file ownership,
  systemctl from the watchdog, journal access), `PrivateNetwork`
  (it is a network server).

To change the port, edit `ExecStart` in the unit — and note the apply
watchdog health-checks whatever port the process was started with.

## Pointing the transport kit at it (Mac side)

In `~/.claude/drop.env` (see `seeds/linux/tools/drop/README.md`):

```sh
export DROP_URL="http://192.168.1.138:8080"
export DROP_TOKEN="<contents of /opt/seed/token on the box>"
export DROP_HANDLE="sess-mac-1"
```

## Uninstall

```sh
sudo systemctl disable --now seed
sudo rm /etc/systemd/system/seed.service
sudo systemctl daemon-reload
# data stays in /opt/seed until you decide otherwise:
# sudo rm -rf /opt/seed
```
