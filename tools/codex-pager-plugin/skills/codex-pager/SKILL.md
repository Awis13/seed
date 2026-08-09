---
name: codex-pager
description: Inspect and operate the local T-Lora Pager to Codex App Server bridge. Use when checking, binding, starting, stopping, or troubleshooting the Codex CODEX-room integration. Do not flash the pager or deploy/restart the Home Rig gateway without explicit user approval.
---

# Codex T-Lora Pager bridge

Use the repository scripts and service instructions in this plugin. Keep the
T-Embed desk pager out of development-agent workflows; it is reserved for home
timers and Home Assistant data.

## Safety

- Treat firmware upload, Home Rig deployment/restart, and plugin installation
  as separate state-changing operations that require explicit user approval.
- Never guess a Desktop task. The bridge either resumes
  `CODEX_PAGER_THREAD_ID`, resumes its persisted dedicated task, or creates a
  new dedicated task and saves that ID.
- The default Codex sandbox is `read-only` with approval policy `never`.

## Checks

Run `python3 scripts/codex_pager_bridge.py --help` and the tests documented in
the plugin README. Inspect the JSON state file only when troubleshooting; it
contains the dedicated task ID and any reply awaiting delivery.
