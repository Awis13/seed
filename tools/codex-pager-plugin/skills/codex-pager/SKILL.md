---
name: codex-pager
description: Inspect and operate the local T-Lora Pager to Codex Desktop bridge. Use when checking, binding, starting, stopping, or troubleshooting the Codex CODEX-room integration. Do not flash the pager or deploy/restart the Home Rig gateway without explicit user approval.
---

# Codex T-Lora Pager bridge

Use the repository scripts and service instructions in this plugin. Keep the
T-Embed desk pager out of development-agent workflows; it is reserved for home
timers and Home Assistant data.

## Safety

- Treat firmware upload, Home Rig deployment/restart, and plugin installation
  as separate state-changing operations that require explicit user approval.
- Never guess or create a Desktop task. `CODEX_PAGER_THREAD_ID` must identify
  the exact existing task that receives pager messages, and
  `CODEX_PAGER_TASK_TITLE` must match its exact unique visible title.
- Accessibility setup is an explicit one-time user action. Normal status and
  test commands must not request the macOS permission prompt.
- Keep the shared gateway bearer token only in the mode-0600 secrets file or
  process environment; never put it in the plugin source or launchd plist.

## Checks

Run `python3 scripts/codex_pager_bridge.py --help` and the tests documented in
the plugin README. Inspect the JSON state file only when troubleshooting; it
contains the explicit task ID and any request or reply awaiting delivery.
