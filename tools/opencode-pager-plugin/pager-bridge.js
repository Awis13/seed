import { tool } from "@opencode-ai/plugin"
import { readFileSync } from "node:fs"
import { homedir } from "node:os"

function loadSecrets() {
  const path = process.env.OPENCODE_PAGER_SECRETS || `${homedir()}/.config/opencode/pager-bridge-secrets.json`
  try { return JSON.parse(readFileSync(path, "utf8")) } catch { return {} }
}

const SECRETS = loadSecrets()

const DEVICES = {
  tlora: {
    label: "T-Lora",
    base: process.env.PAGER_TLORA_URL || "http://192.168.1.116:8080",
    token: process.env.PAGER_TLORA_TOKEN || SECRETS.tloraToken || "",
    answerField: "reply",
    maxTitle: 40,
    maxBody: 96,
    asciiOnly: false,
    maxOptions: 0,
    maxOption: 15,
  },
}

const POLL_MS = 15000

// Гейтвей home-rig (:8325) — единая точка доставки. Пейджер может быть с
// выключенным WiFi (mesh-only в саду): uplink приходит в /opencode/inbox,
// ответ уходит через /notify-out (prefer_wifi → mesh fallback сам).
const GW = (process.env.OPENCODE_GW_URL || "http://192.168.1.138:8325").replace(/\/$/, "")
const GW_TOKEN = process.env.OPENCODE_GW_TOKEN || SECRETS.gatewayToken || ""

async function gwRequest(path, method = "GET", body, timeoutMs = 8000) {
  try {
    const res = await fetch(GW + path, {
      method,
      headers: {
        ...(GW_TOKEN ? { Authorization: `Bearer ${GW_TOKEN}` } : {}),
        ...(body ? { "Content-Type": "application/json" } : {}),
      },
      body: body ? JSON.stringify(body) : undefined,
      signal: AbortSignal.timeout(timeoutMs),
    })
    const text = await res.text()
    let json = null
    try { json = JSON.parse(text) } catch {}
    return { ok: res.ok, status: res.status, json, text }
  } catch (e) {
    return { ok: false, status: 0, json: null, text: String(e) }
  }
}

async function gwGet(path) { return gwRequest(path) }
async function gwAck(agent, ids) {
  return gwRequest(`/${agent}/inbox/ack`, "POST", { ids })
}

async function gwSend(body) {
  return gwRequest("/notify-out", "POST", body, 180000)
}

// Подписка сессии на пейджер: sessionID привязан к устройству, КАЖДОЕ новое
// сообщение Николая (изменение chosen/reply на любой карточке) инжектится
// в сессию. Канал живёт, пока сессия жива или его не снимут pager_clear —
// авто-закрытия по бездействию НЕТ.
const subs = new Map() // `${device}:${sessionID}` -> { device, sessionID, lastSeen: Map<id, value> }

function argvSessionID() {
  const envID = (process.env.OPENCODE_PAGER_SESSION || "").trim()
  if (envID) return envID
  for (let i = 0; i < process.argv.length; i++) {
    const arg = process.argv[i]
    if ((arg === "-s" || arg === "--session") && process.argv[i + 1]) return process.argv[i + 1]
    if (arg.startsWith("--session=")) return arg.slice("--session=".length)
  }
  return ""
}

function responseData(r) {
  if (Array.isArray(r)) return r
  return r && r.data !== undefined ? r.data : r
}

function isAscii(s) {
  return /^[\x20-\x7E]*$/.test(s)
}

function clampBytes(s, max) {
  const b = new TextEncoder().encode(s)
  if (b.length <= max) return s
  let out = new TextDecoder().decode(b.subarray(0, max))
  out = out.replace(/[\uFFFD\u0000-\u001F\u007F]/g, "")
  return out.slice(0, max)
}

function dev(name) {
  const d = DEVICES[name]
  if (!d) throw new Error(`unknown device '${name}' — OpenCode owns only tlora`)
  if (!d.token) throw new Error(`missing token for ${d.label}; configure the secure pager secrets file`)
  return d
}

async function api(device, path, method = "GET", body) {
  const res = await fetch(device.base + path, {
    method,
    headers: {
      Authorization: `Bearer ${device.token}`,
      ...(body ? { "Content-Type": "application/json" } : {}),
    },
    body: body ? JSON.stringify(body) : undefined,
    signal: AbortSignal.timeout(8000),
  })
  const text = await res.text()
  let json
  try { json = JSON.parse(text) } catch { json = null }
  return { ok: res.ok, status: res.status, json, text }
}

function buildNotify(d, title, body, level, source, options) {
  const titleC = clampBytes(title, d.maxTitle)
  const bodyC = clampBytes(body, d.maxBody)
  if (d.asciiOnly) {
    for (const [name, v] of [["title", titleC], ["body", bodyC], ["source", source]]) {
      if (!isAscii(v)) throw new Error(`device ${d.label}: non-ASCII in ${name} — its panel has no glyphs, write plain English`)
    }
  }
  const msg = { level, title: titleC, body: bodyC, source }
  if (options && options.length) {
    if (!d.maxOptions) throw new Error(`device ${d.label}: options are not supported — T-Lora answers by keyboard, no buttons`)
    if (options.length > d.maxOptions) throw new Error(`max ${d.maxOptions} options on ${d.label}`)
    const opts = options.map((o) => {
      const oC = String(o).slice(0, d.maxOption)
      if (d.asciiOnly && !isAscii(oC)) throw new Error(`option '${o}' has non-ASCII — ${d.label} panel cannot draw it`)
      return oC
    })
    msg.options = opts
  }
  return msg
}

async function sendNotify(d, title, body, level, source, options, key) {
  const msg = buildNotify(d, title, body, level, source, options)
  if (key) msg.id = key
  const r = await api(d, "/notify", "POST", msg)
  if (!r.ok) throw new Error(`POST /notify failed (${r.status}): ${r.text}`)
  return r.json
}

export default async ({ client }) => {
  const source = process.env.PAGER_SOURCE || "opencode-pager"
  let polling = false

  async function answerValue(d, n) {
    // Возвращает текстовое значение ответа Николая на карточке, или null если не отвечено.
    if (d.answerField === "chosen") {
      const c = n.chosen
      if (typeof c === "number" && c >= 0 && Array.isArray(n.options) && c < n.options.length) {
        return `ANSWER ${n.options[c]}`
      }
      return null
    }
    const rep = n.reply
    if (typeof rep === "string" && rep.trim()) return `ANSWER ${rep.trim()}`
    return null
  }

  async function inject(sessionID, text) {
    try {
      await client.session.promptAsync({
        path: { id: sessionID },
        body: {
          parts: [{ type: "text", text }],
        },
      })
      return true
    } catch (e) {
      try {
        await client.app.log({ body: { service: "pager-bridge", level: "error", message: `inject failed: ${e.message}`, extra: { sessionID } } })
      } catch {}
      return false
    }
  }

  async function resolveActiveSessionID() {
    // An explicitly attached CLI session is authoritative. This avoids the old
    // startup fan-out where every historical session became a pager subscriber.
    const explicit = argvSessionID()
    if (explicit) return explicit

    // A server process has no -s. Only choose a fallback when the SDK reports
    // exactly one currently busy/retrying root session; ambiguity means "do not
    // consume the destructive gateway inbox".
    try {
      const [statusResult, listResult] = await Promise.all([
        client.session.status(),
        client.session.list(),
      ])
      const statuses = responseData(statusResult) || {}
      const sessions = responseData(listResult)
      if (!Array.isArray(sessions)) return ""
      const active = sessions.filter((s) =>
        s && !s.parentID && statuses[s.id] && statuses[s.id].type !== "idle"
      )
      return active.length === 1 ? active[0].id : ""
    } catch {
      return ""
    }
  }

  // Каждый агент читает только свои карточки. Иначе один reply получают
  // несколько сессий (например, OpenCode и Hermes одновременно).
  function isForeignChannel(n) {
    return (n.source || "").trim().toLowerCase() !== source.trim().toLowerCase()
  }

  // Первичный снимок подписки: запомнить текущие ответы как baseline,
  // чтобы НЕ инжектить старьё, а только новые сообщения.
  // Door-карточка `opencode-chat`: по клику на пейджере открывается тред
  // агента OPENCODE (тот же механизм, что у Hermes — `id=*-chat`).
  // source остаётся наш (`opencode-pager`), чтобы reply на карточку ловил
  // плагин; открытие треда резолвится прошивкой из префикса id.
  async function ensureChatDoor(d) {
    try {
      await sendNotify(d, "OPENCODE", "tread ready - type to talk", "info", source, undefined, "opencode-chat")
    } catch (e) {
      try { await client.app.log({ body: { service: "pager-bridge", level: "warn", message: `door failed: ${e.message}` } }) } catch {}
    }
  }

  // Дублируем текст ответа открыто-сессии в тред-историю на пейджере
  // (/agents/inbound agent=opencode) — ровно как Hermes-адаптер. Best-effort.
  async function mirrorToThread(d, text) {
    if (!text) return
    try {
      await api(d, "/agents/inbound", "POST", { agent: "opencode", text })
    } catch (e) {
      try { await client.app.log({ body: { service: "pager-bridge", level: "warn", message: `mirror failed: ${e.message}` } }) } catch {}
    }
  }

  async function seedSubscription(s, d) {
    const r = await api(d, "/notify")
    if (!r.ok || !r.json || !Array.isArray(r.json.notifications)) return
    for (const n of r.json.notifications) {
      if (isForeignChannel(n)) continue
      const v = await answerValue(d, n)
      if (v !== null) s.lastSeen.set(n.id, v)
    }
  }

  async function pollOnce() {
    if (polling) return
    polling = true
    try {
      // Гейтвей-канал (работает даже когда у пейджера WiFi OFF — гейтвей по LAN
      // всегда доступен). Пейджерные строки (agent=opencode) копятся в
      // /opencode/inbox; one T-Lora room has exactly one owning OpenCode session.
      try {
        let tloraSubs = [...subs.values()].filter((s) => s.device === "tlora")
        if (!tloraSubs.length) {
          const sessionID = await resolveActiveSessionID()
          if (sessionID) {
            await ensureSubscription("tlora", sessionID, { primeDevice: false })
            tloraSubs = [...subs.values()].filter((s) => s.device === "tlora")
          }
        }

        // GET /opencode/inbox drains the queue. Never call it until there is a
        // concrete destination, otherwise a mesh uplink is silently discarded.
        const gi = tloraSubs.length ? await gwGet("/opencode/inbox") : null
        if (gi && gi.ok && gi.json && Array.isArray(gi.json.items) && gi.json.items.length) {
          for (const item of gi.json.items) {
            const t = (item && item.text) || ""
            if (!t) continue
            const s = tloraSubs[0]
            const delivered = s && await inject(s.sessionID, `[pager tlora thread] OPENCODE — ${t}`)
            if (delivered && item.id) {
              const ack = await gwAck("opencode", [item.id])
              if (!ack.ok) throw new Error(`gateway ack failed (${ack.status}): ${ack.text}`)
            }
          }
        }
      } catch (e) {
        try { await client.app.log({ body: { service: "pager-bridge", level: "warn", message: `gw inbox: ${e.message}` } }) } catch {}
      }

      for (const [key, s] of subs) {
        const d = dev(s.device)
        let r
        try {
          r = await api(d, "/notify")
        } catch (e) {
          try { await client.app.log({ body: { service: "pager-bridge", level: "warn", message: `poll ${key}: ${e.message}` } }) } catch {}
          continue
        }
        if (!r.ok || !r.json || !Array.isArray(r.json.notifications)) continue
        const seen = new Set()
        for (const n of r.json.notifications) {
          if (isForeignChannel(n)) continue
          seen.add(n.id)
          let v
          try { v = await answerValue(d, n) } catch (e) { continue }
          if (v === null) continue
          if (s.lastSeen.get(n.id) === v) continue
          s.lastSeen.set(n.id, v)
          const label = n.title || n.id
          await inject(s.sessionID, `[pager ${s.device}] ${label} — ${v}`)
          try {
            await client.app.log({ body: { service: "pager-bridge", level: "info", message: `poll ${key}: ${n.id} ${v}` } })
          } catch {}
        }
        // Карточки, что исчезли с устройства, из lastSeen убрать — их ответы уже не всплывут.
        for (const id of [...s.lastSeen.keys()]) {
          if (!seen.has(id)) s.lastSeen.delete(id)
        }
        // Тред OPENCODE: тот же механизм, что карточки — пулим /agents, и если
        // Николай написал новое сообщение в тред (last_from_me=true, last изменился),
        // инжектим его в ЭТУ сессию через promptAsync. Счётчик session_msgs на
        // прошивке не всегда растёт для последней строки, поэтому сравниваем сам
        // текст last (надёжнее).
        try {
          const ar = await api(d, "/agents")
          const ag = ar.ok && ar.json && ar.json.agents
            ? ar.json.agents.find((x) => (x.id || "").toLowerCase() === "opencode")
            : null
          if (ag && typeof ag.last === "string" && ag.last.trim() &&
              ag.last_from_me === true && ag.last !== (s.treadLast || "")) {
            s.treadLast = ag.last
            await inject(s.sessionID, `[pager ${s.device} thread] OPENCODE — ${ag.last}`)
          } else if (ag && ag.last === (s.treadLast || "")) {
            // same — nothing new
          }
        } catch (e) {
          try { await client.app.log({ body: { service: "pager-bridge", level: "warn", message: `tread poll ${key}: ${e.message}` } }) } catch {}
        }
      }
    } finally {
      polling = false
    }
  }

  setInterval(pollOnce, POLL_MS)
  // Do not call client.session.* while the plugin factory is still loading:
  // OpenCode's SDK client is not ready yet and can leave `polling` stuck true.
  setTimeout(() => void pollOnce(), 1000)

  // Канал открывается обычным pager_ask/pager_tell (context.sessionID — правильная
  // сессия, никакого угадывания). Поллинг ниже пуллит треда OPENCODE для КАЖДОЙ
  // подписанной сессии и инжектит только новое от Николая — ровно одна
  // подписка на ту сессию, что реально открыла канал.

  async function ensureSubscription(deviceName, sessionID, { primeDevice = true } = {}) {
    const key = `${deviceName}:${sessionID}`
    if (subs.has(key)) return key
    if (deviceName === "tlora") {
      for (const [existingKey, existing] of [...subs.entries()]) {
        if (existing.device === "tlora") subs.delete(existingKey)
      }
    }
    const s = { device: deviceName, sessionID, lastSeen: new Map(), treadLast: "" }
    subs.set(key, s)
    if (!primeDevice) return key
    // Baseline треда: запомнить текущий last opencode, чтобы не инжектить
    // старое, а только новые сообщения от Николая.
    try {
      const d0 = dev(deviceName)
      const ar = await api(d0, "/agents")
      const ag = ar.ok && ar.json && ar.json.agents
        ? ar.json.agents.find((x) => (x.id || "").toLowerCase() === "opencode")
        : null
      if (ag && typeof ag.last === "string") s.treadLast = ag.last
    } catch {}
    // Прежде чем подписаться на tlora — показать дверь-тред OPENCODE на пейджере.
    if (deviceName === "tlora") await ensureChatDoor(dev(deviceName))
    try { await seedSubscription(s, dev(deviceName)) } catch {}
    return key
  }

  return {
    tool: {
      pager_ask: tool({
        description: "Send a question to Nikolay's T-Lora development pager and open its single owning OpenCode channel. T-Embed is reserved for Home Assistant and household timers.",
        args: {
          device: tool.schema.enum(["tlora"]).default("tlora").describe("Development pager; T-Embed is unavailable to OpenCode."),
          title: tool.schema.string().describe("Short title, max 40 UTF-8 bytes."),
          body: tool.schema.string().describe("One self-explanatory phrase, max 96 UTF-8 bytes."),
          level: tool.schema.enum(["info", "warn", "crit"]).default("warn").describe("warn = needs his decision; info = task done, waiting on him; crit = actually broken"),
          key: tool.schema.string().optional().describe("Optional id key to replace a previous page with the same key"),
        },
        async execute(args, context) {
          const deviceName = args.device ?? "tlora"
          const d = dev(deviceName)
          const r = await sendNotify(d, args.title, args.body, args.level, source, args.options, args.key)
          const id = r.id
          if (!id) throw new Error("pager returned no id")
          await ensureSubscription(deviceName, context.sessionID)
          const optsNote = " (keyboard reply)"
          return `asked on ${d.label} id=${id}${optsNote}. Channel open: every reply arrives in this session automatically.`
        },
      }),

      pager_tell: tool({
        description: "Send a one-way notification to Nikolay's pager (no question, no answer expected). For task milestones, progress, or 'done' notes — NOT for decisions. Returns message id.",
        args: {
          device: tool.schema.enum(["tlora"]).default("tlora").describe("Development T-Lora only"),
          title: tool.schema.string().describe("Short title, max 40 UTF-8 bytes."),
          body: tool.schema.string().describe("One phrase, max 96 UTF-8 bytes."),
          level: tool.schema.enum(["info", "warn", "crit"]).default("info").describe("info = done/notice; warn = needs attention; crit = broken"),
          key: tool.schema.string().optional().describe("Optional id key to replace a previous page with the same key"),
        },
        async execute(args, context) {
          const deviceName = args.device ?? "tlora"
          const d = dev(deviceName)
          // Ответ этой сессии на T-Lora → door-карточка opencode-chat (клик
          // открывает тред, как Hermes hermes-chat). Доставка через гейтвей
          // notify-out (prefer_wifi → mesh fallback сам), чтобы ответ дошёл
          // даже когда у пейджера WiFi OFF. Прямой /notify — fallback.
          const k = args.key || (deviceName === "tlora" ? "opencode-chat" : undefined)
          let sent = false
          if (deviceName === "tlora") {
            const g = await gwSend({
              level: args.level || "info",
              source: "opencode-pager",
              title: args.title,
              body: args.body,
              id: k,
              mode: "prefer_wifi",
            })
            sent = g.ok
          }
          if (!sent) {
            await sendNotify(d, args.title, args.body, args.level, source, undefined, k)
          }
          if (deviceName === "tlora") {
            // Открыть канал этой сессии для тред-инжекта (если ещё не открыт).
            await ensureSubscription("tlora", context.sessionID)
            // Ответ открыто-сессии на T-Lora → тоже в тред-историю (как Hermes).
            if (!sent) {
              try { await mirrorToThread(d, args.body) } catch {}
            }
          }
          return `told on ${d.label} id=${k || "gw"} via ${sent ? "gateway" : "direct"}`
        },
      }),

      pager_clear: tool({
        description: "Close the pager channel (and acknowledge the page): stops this session from receiving pager replies. By id, by key, or close the whole channel for this device/session (allMine). Use when the question is fully answered and you no longer want his messages to arrive.",
        args: {
          id: tool.schema.number().optional().describe("Numeric message id from pager_ask/pager_tell"),
          key: tool.schema.string().optional().describe("id key the page was sent with"),
          device: tool.schema.enum(["tlora"]).default("tlora").describe("Development T-Lora only"),
          allMine: tool.schema.boolean().default(false).describe("Close this session's channel on the device and acknowledge all pages"),
        },
        async execute(args, context) {
          const deviceName = args.device ?? "tlora"
          const d = dev(deviceName)
          if (args.allMine) {
            try { await api(d, "/notify/ack", "POST", { all: true }) } catch {}
            for (const k of [...subs.keys()]) {
              if (k.endsWith(`:${context.sessionID}`)) subs.delete(k)
            }
            return `channel closed on ${d.label}`
          }
          if (args.key) {
            const r = await api(d, "/notify")
            const rows = r.ok && r.json && Array.isArray(r.json.notifications) ? r.json.notifications : []
            const hit = rows.find((n) => n.key === args.key || (n.title || "").includes(args.key))
            if (hit) {
              try { await api(d, "/notify/ack", "POST", { id: hit.id }) } catch {}
              return `cleared by key ${args.key} (id=${hit.id}) on ${d.label}`
            }
            return `no page found for key ${args.key}`
          }
          if (typeof args.id === "number") {
            try { await api(d, "/notify/ack", "POST", { id: args.id }) } catch {}
            return `cleared id=${args.id} on ${d.label}`
          }
          throw new Error("pager_clear needs id, key, or allMine")
        },
      }),

      pager_status: tool({
        description: "Show which pager channels are currently open (session bound to pager, listening for his messages).",
        args: {},
        async execute() {
          if (!subs.size) return "no open pager channels"
          const rows = [...subs.values()].map((s) => `${s.device} (${s.sessionID}): ${s.lastSeen.size} messages seen`)
          return rows.join("\n")
        },
      }),
    },
  }
}
