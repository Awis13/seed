# Codex T-Lora Pager plugin

This plugin connects the T-Lora Pager `CODEX` room to one explicit, existing
Codex Desktop task. Pager messages appear in that task's visible UI. The bridge
never guesses, creates, resumes, or starts a different task.

## Data path

1. T-Lora sends `C1|codex|...|u|...` over private MeshCore DM.
2. Home Rig stores the message in the isolated durable `/codex/inbox` queue.
3. The bridge records the queue item and asks the macOS companion to open the
   configured `codex://threads/<id>` URL, proves the focused task by its exact
   visible title, and submits through semantic Accessibility controls.
4. Only after the stable marker is durable in that task's rollout does the
   bridge acknowledge the gateway item.
5. The bridge follows that task's rollout JSONL from the pre-submit offset. A
   stable gateway-message marker binds the final answer to the submitted turn.
6. The final answer is posted to `/notify-out` as the CODEX C1 thread and door.

Pager Wi-Fi may be off. Both pager directions use the radio link to Home Rig;
only the Mac bridge and Home Rig gateway need LAN access.

## Build the companion

The helper uses only macOS AppKit and ApplicationServices:

```sh
tools/codex-pager-plugin/scripts/build_companion.sh
```

It controls only the `com.openai.codex` application. After deep-link navigation,
it requires the focused `AXWindow` title or represented-document value to equal
the configured task title. Descendant text, including sidebar entries, is never
accepted as task proof. It also requires independent prompt/message/composer
semantics on the writable editor, verifies the exact prompt value inserted by
the deep link, and uses
`AXConfirm` or a direct Send/Submit child within at most two composer ancestors.
It fails closed instead of using coordinates or global keystrokes. The task
title must therefore be unique, unchanged, and exposed as a focused-window AX
attribute by the installed Codex Desktop build. If that attribute is not
exposed, live E2E remains blocked rather than falling back to sidebar text.

## Accessibility setup

The bridge's normal `check` mode never opens the macOS permission prompt. For
the one-time setup, run this interactively from the same executable that
launchd will use:

```sh
tools/codex-pager-plugin/bin/codex-pager-companion request-accessibility
```

Approve the helper in **System Settings → Privacy & Security → Accessibility**,
then run `codex-pager-companion check`. Do not load the launch agent until the
check succeeds. Rebuilding the unsigned helper may require granting access
again; code signing is recommended for a permanent installation.

## Configuration

`CODEX_PAGER_THREAD_ID` is mandatory. Copy the task ID from the Codex task URL
or task metadata; it must be the exact task visible in Desktop.

- `CODEX_PAGER_THREAD_ID` — exact existing Codex task ID; required
- `CODEX_PAGER_TASK_TITLE` — exact unique visible task title used as the
  structural AX navigation proof; required
- `CODEX_PAGER_COMPANION` — helper executable path
- `CODEX_PAGER_SESSIONS` — rollout root, default `~/.codex/sessions`
- `CODEX_PAGER_GATEWAY` — default `http://192.168.1.138:8325`
- `CODEX_PAGER_GATEWAY_TOKEN` — Codex-scoped capability, or `gatewayToken` in
  the mode-0600 secrets file
- `CODEX_PAGER_SECRETS` — default `~/.config/codex-pager/secrets.json`
- `CODEX_PAGER_STATE` — default `~/.codex/codex-pager-bridge.json`
- `CODEX_PAGER_POLL_INTERVAL` — gateway polling interval, default `2` seconds
- `CODEX_PAGER_ROLLOUT_POLL_INTERVAL` — local rollout polling, default `0.5`
- `CODEX_PAGER_TURN_TIMEOUT` — answer timeout, default `1800` seconds

Pending work is tied to the configured task ID. Changing the ID while a message
is pending is rejected. A crash in the ambiguous submit window is recovered
only when the stable marker is present in the exact rollout; otherwise the
bridge refuses to submit a duplicate.

If the helper exits or times out after durable submit intent, the bridge waits
briefly for the rollout marker. Without a marker it records `state=ambiguous`,
does not submit again, and continues with later queue items. Resolve it manually
only after inspecting the configured task rollout: if the marker exists, leave
the item for automatic reconciliation; if it does not, reject the gateway item
with `POST /codex/inbox/reject` and remove the matching ambiguous row from the
bridge state while the bridge is stopped.

Replies carry `deliveryId=codex-reply:<gateway-message-id>`. The gateway stores
successful delivery IDs durably and derives the C1 message ID from that value,
and reserves each delivery key before transport. Concurrent calls cannot both
send. A restart with an unfinished reservation returns HTTP 409 with
`ambiguous=true` and never resends automatically; the bridge retains that reply
for manual resolution. This at-most-once policy avoids duplicates but can
require operator reconciliation after a crash between radio delivery and the
durable result write.

## Verify without UI or network access

```sh
tools/codex-pager-plugin/scripts/build_companion.sh
python3 -m unittest discover -s tools/codex-pager-plugin/tests -v
python3 -m py_compile tools/codex-pager-plugin/scripts/codex_pager_bridge.py
tools/codex-pager-plugin/bin/codex-pager-companion url \
  --thread 01900000-0000-7000-8000-000000000000 \
  --prompt 'Unicode test'
```

Tests and compilation do not request Accessibility, contact the gateway,
restart services, or modify firmware. Loading launchd and live pager E2E are
deliberately separate user-approved operations.
