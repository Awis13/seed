# Codex T-Lora Pager plugin

This plugin connects the T-Lora Pager `CODEX` room to one dedicated Codex App
Server task. It never guesses the newest Desktop task and never consumes the
OpenCode queue.

## Data path

1. T-Lora sends `C1|codex|...|u|...` over private MeshCore DM.
2. Home Rig gateway stores it in the isolated `GET /codex/inbox` queue.
3. The bridge submits it with `turn/start` to one explicit App Server task.
4. The final assistant message is posted as `source=codex-pager` and
   `id=codex-chat` to `/notify-out`.
5. Gateway sends the full C1 answer and a short `CODEX` P1 door card. Enter on
   that card opens the CODEX room.

Pager Wi-Fi may be OFF: both directions use the radio link between pager and
Home Rig. Only the Mac bridge and Home Rig gateway need LAN access.

## Configuration

The bridge reads environment variables:

- `CODEX_PAGER_GATEWAY` (default `http://192.168.1.138:8325`)
- `CODEX_PAGER_WORKSPACE` (default current directory)
- `CODEX_PAGER_THREAD_ID` (optional explicit existing task)
- `CODEX_PAGER_CODEX_BIN` (default `codex`; use an absolute path under launchd)
- `CODEX_PAGER_STATE` (default `~/.codex/codex-pager-bridge.json`)
- `CODEX_PAGER_POLL_INTERVAL` (default `2` seconds)
- `CODEX_PAGER_TURN_TIMEOUT` (default `1800` seconds)
- `CODEX_PAGER_SANDBOX` (default `read-only`)
- `CODEX_PAGER_APPROVAL_POLICY` (default `never`)

Without an explicit task ID, the bridge creates and names a dedicated task once
and persists its ID. Completed replies are also persisted until gateway
delivery succeeds.

## Verify

```sh
python3 -m unittest discover -s tests -v
python3 -m py_compile scripts/codex_pager_bridge.py
python3 scripts/codex_pager_bridge.py --help
```

Use the plugin creator validator before installation. Installing the plugin,
loading the launch agent, deploying the gateway, restarting services, and
uploading pager firmware are deliberately separate operations.
