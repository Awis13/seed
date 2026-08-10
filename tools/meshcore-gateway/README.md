# MeshCore pager gateway

Canonical source for the always-on Home Rig gateway used by the T-Lora Pager.
It sends only private MeshCore DMs to the configured pager public key.

## Agent routing

The gateway owns two independent, bounded durable queues:

- `GET /opencode/inbox` lists only `C1|opencode|...|u|...` messages.
- `GET /codex/inbox` lists only `C1|codex|...|u|...` messages.
- `POST /<agent>/inbox/ack` removes explicitly acknowledged message IDs.
- `POST /<agent>/inbox/reject` durably moves proven pre-submit failures into a
  bounded dead-letter list instead of leaving them in the active queue.

Replies use the same `POST /notify-out` contract. A chat reply becomes both a
complete C1 history entry and a short P1 door card only when its exact identity
matches one of these pairs:

- `source=opencode-pager`, `id=opencode-chat`
- `source=codex-pager`, `id=codex-chat`

That strict pair prevents one integration from opening the other room.

Agent bridges should include a stable `deliveryId`. The gateway durably
reserves the scoped source/id/delivery tuple before transport and stores
successful results. Concurrent duplicates are suppressed. An unfinished
reservation after restart returns HTTP 409 with `ambiguous=true` and is never
resent automatically. This avoids duplicate chat history but requires manual
resolution for the rare crash-after-radio window.

## Install or update

1. Create a virtual environment and install `aiohttp`, `PyYAML`, and the
   MeshCore Python package used by the radio companion.
2. Copy `config.yaml.example` to `config.yaml` and set the serial port, pager
   URL, exact pager public key, and durable inbox path. Put `PAGER_TOKEN=...`,
   `MESHCORE_GATEWAY_TOKEN=...`, `MESHCORE_OPENCODE_TOKEN=...`,
   `MESHCORE_CODEX_TOKEN=...`, and `MESHCORE_HERMES_NOTIFY_TOKEN=...` in a
   mode-0600 service environment file. All four API tokens are required; the
   daemon refuses to start if any of them is empty.
3. Adapt `meshcore-daemon.service.example`, install it with systemd, run
   `systemctl daemon-reload`, then restart the service.
4. Verify `/health`; then send one message to each room and confirm only the
   corresponding inbox receives it.

Only `/health` is public. Agent endpoints and exact chat downlinks require that
agent's scoped bearer; the admin bearer may operate every route. Every MeshCore
command is restricted to the configured pager identity; `sh` and `reboot`
additionally remain disabled unless explicitly enabled.

Deploying this file or restarting the live service is a separate operational
step; repository tests do not touch the radio or pager.

## Tests

```sh
python3 -m unittest discover -s tests -v
python3 -m py_compile meshcore_daemon.py
```
