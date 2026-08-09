# OpenCode T-Lora pager plugin

Canonical source for the live OpenCode pager bridge. Copy `pager-bridge.js` to
`~/.config/opencode/plugins/` and restart OpenCode to load changes.

The OpenCode tools expose only the T-Lora development pager. T-Embed is
reserved for Home Assistant and household timers.

Secrets are loaded from environment variables or a mode-0600 JSON file at
`~/.config/opencode/pager-bridge-secrets.json`:

```json
{
  "tloraToken": "pager-http-token",
  "gatewayToken": "shared-home-rig-api-token"
}
```

No token belongs in `pager-bridge.js`. The gateway inbox is non-destructive;
the plugin acknowledges a message ID only after `promptAsync()` succeeds. A
single T-Lora owner session replaces older subscriptions instead of fanning one
pager message into multiple OpenCode tasks.

Validate before installation:

```sh
node --check pager-bridge.js
```
