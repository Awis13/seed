# MeshCore pager gateway

Canonical source for the always-on Home Rig gateway used by the T-Lora Pager.
It sends only private MeshCore DMs to the configured pager public key.

## Agent routing

The gateway owns two independent, bounded RAM queues:

- `GET /opencode/inbox` drains only `C1|opencode|...|u|...` messages.
- `GET /codex/inbox` drains only `C1|codex|...|u|...` messages.

Replies use the same `POST /notify-out` contract. A chat reply becomes both a
complete C1 history entry and a short P1 door card only when its exact identity
matches one of these pairs:

- `source=opencode-pager`, `id=opencode-chat`
- `source=codex-pager`, `id=codex-chat`

That strict pair prevents one integration from opening the other room.

## Install or update

1. Create a virtual environment and install `aiohttp`, `PyYAML`, and the
   MeshCore Python package used by the radio companion.
2. Copy `config.yaml.example` to `config.yaml` and set the serial port, pager
   URL, and exact pager public key. Put `PAGER_TOKEN=...` in a root-readable
   environment file instead of committing it.
3. Adapt `meshcore-daemon.service.example`, install it with systemd, run
   `systemctl daemon-reload`, then restart the service.
4. Verify `/health`; then send one message to each room and confirm only the
   corresponding inbox receives it.

Deploying this file or restarting the live service is a separate operational
step; repository tests do not touch the radio or pager.

## Tests

```sh
python3 -m unittest discover -s tests -v
python3 -m py_compile meshcore_daemon.py
```
