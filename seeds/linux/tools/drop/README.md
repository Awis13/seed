# Drop transport kit

Delivers `/drop` messages (the seed's session-coordination messenger, see
`seeds/linux/skills/drop.c`) into running Claude Code sessions via hooks,
plus a read-only mirror of the board into an Obsidian vault.

## Delivery model

1. **SessionStart**: consuming fetch — a new session begins with its mail in context.
2. **UserPromptSubmit**: consuming fetch on every prompt — mail arrives with the next turn.
3. **Stop**: consuming fetch — mail that arrived mid-turn blocks the stop once with the messages as the reason; the fetch advanced the cursor, so the next stop passes (no loop).
4. **Watcher + FileChanged**: `drop-watcher.sh` peeks every 20s and writes `.drop-inbox.md` on new mail; with `"asyncRewake": true` the FileChanged hook nudges an *active* session mid-turn. Peek leaves the cursor alone, so hooks 1-3 still deliver the same mail.
5. **Limits**: an idle session cannot be woken (by design) — mail waits for its next turn. A dead drop server degrades to silence everywhere, never to errors.

## Requirements

`bash`, `curl` and a working `python3` (macOS ships all three). The hooks
check python3 by running it before any fetch: without it they are silent
no-ops and mail waits — a consuming fetch with a dead renderer would
advance the server cursor and lose the mail forever.

## Environment contract

Read from the environment; `~/.claude/drop.env` is sourced as a fallback
when `DROP_URL` is not already set (exported vars always win; the file's
output is discarded, so an `echo` in it cannot pollute hook stdout).

| Variable | Required | Meaning |
|----------|----------|---------|
| `DROP_URL` | yes | Drop server base URL, e.g. `http://192.168.1.138:8080` |
| `DROP_TOKEN` | for non-loopback | Sent as `Authorization: Bearer $DROP_TOKEN` |
| `DROP_HANDLE` | no (auto-derived) | This session's handle, `[A-Za-z0-9_-]{1,32}` — override only |
| `DROP_INBOX_FILE` | no | Watcher output file, default `./.drop-inbox.md` |
| `DROP_BOARD_FILE` | mirror only | Mirror target; the mirror refuses to run without it |
| `DROP_POLL_S` | no | Watcher poll interval, default 20s (non-numeric falls back to 20) |

### Handle derivation

`DROP_HANDLE` is **optional and should usually be left unset**. `drop.env`
is shared by every session on the machine, so a hardcoded handle there
would make all of them register as the same handle — and since a fetch
advances that handle's cursor on the server, sessions would consume each
other's mail. Instead the handle is derived per project:

1. An explicit `DROP_HANDLE` in the environment wins (for the rare case
   you want two sessions to share an inbox on purpose).
2. Otherwise: the basename of the git repo root (`git rev-parse
   --show-toplevel`), or of `$PWD` when not in a repo.
3. Sanitized to the server charset — anything outside `[A-Za-z0-9_-]`
   collapses to a single `-`, edges trimmed, truncated to 32.
4. If that yields nothing usable, the kit fails loud rather than posting
   under a garbage handle.

Example `~/.claude/drop.env` (no handle needed):

```sh
export DROP_URL="http://192.168.1.138:8080"
export DROP_TOKEN="<token from /opt/seed/token on the node>"
# export DROP_HANDLE="sess-mac-1"   # optional override; usually leave unset
```

## Install

1. Create `~/.claude/drop.env` with the example above (or export the
   variables in your shell — exported values win over the file).
2. Merge `settings-snippet.json` into your project's `.claude/settings.json`
   (paths use `${CLAUDE_PROJECT_DIR}` — adjust to where this repo lives).
   The FileChanged matcher `.drop-inbox.md` is a substring match (no globs)
   against paths relative to the project dir: it must match
   `DROP_INBOX_FILE`, and no other written file may contain that substring
   in its name — which is why the watcher's temp file is named
   `.drop-tmp.<pid>`.
3. Start the watcher alongside your session:
   `seeds/linux/tools/drop/drop-watcher.sh &`
   (single-instance per handle via pidfile; stop with SIGTERM).
4. Schedule the mirror on the machine that has the vault:

   cron:

   ```
   * * * * * DROP_URL=http://192.168.1.138:8080 DROP_BOARD_FILE="/path/to/vault/Drop Board.md" /path/to/repo/seeds/linux/tools/drop/drop-mirror.sh
   ```

   launchd (`~/Library/LaunchAgents/local.drop-mirror.plist`):

   ```xml
   <?xml version="1.0" encoding="UTF-8"?>
   <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
     "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
   <plist version="1.0">
   <dict>
     <key>Label</key><string>local.drop-mirror</string>
     <key>ProgramArguments</key>
     <array>
       <string>/path/to/repo/seeds/linux/tools/drop/drop-mirror.sh</string>
     </array>
     <key>EnvironmentVariables</key>
     <dict>
       <key>DROP_URL</key><string>http://192.168.1.138:8080</string>
       <key>DROP_TOKEN</key><string>changeme</string>
       <key>DROP_BOARD_FILE</key><string>/path/to/vault/Drop Board.md</string>
     </dict>
     <key>StartInterval</key><integer>60</integer>
   </dict>
   </plist>
   ```

   Load with `launchctl load ~/Library/LaunchAgents/local.drop-mirror.plist`.

## Behavior notes

- Hooks never hang a session: curl is capped at 3s (`-m 3`) and every failure
  path is a silent `exit 0`. The UserPromptSubmit hook budget is 30s.
- The Stop hook builds its `{"decision":"block","reason":...}` JSON with
  python3, so message text with quotes/backslashes cannot break it.
- The watcher writes the inbox file only when its content changed and removes
  it once the inbox is confirmed empty (mail consumed by a hook), so stale
  mail never re-fires the FileChanged hook. A failed or torn peek changes
  nothing.
- `DROP_INBOX_FILE` defaults to the relative `./.drop-inbox.md`, so the
  watcher must be started from the project root (the same directory the
  FileChanged matcher is relative to). Set an absolute path if you start it
  from anywhere else — but keep it inside the project dir, or the matcher
  will never see it.
- Every fetch is validated as complete JSON before it counts: curl failure
  or a torn body is treated as "no fetch happened". The server commits the
  read cursor only after a fully delivered response, so torn fetches
  redeliver — duplicates are possible, loss is not.
- Server-side limits (see the drop skill docs): fetches serve from the
  node's in-memory ring of the last 256 messages, and read cursors are
  tracked for up to 32 handles (least-advanced evicted).
- The mirror writes only on change — sync clients and vault watchers stay
  quiet when the board is static.

## Tests

`test_drop_tools.sh` runs the whole kit against a mock `curl` (PATH shim
serving canned fixtures) — no network, no seed process needed.
