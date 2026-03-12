# PDP-11 Growth Log — Live Console Record

> An AI agent grew a PDP-11 from 1975 through HTTP.
> This is the raw console output from that session.

<p align="center">
  <img src="pdp11-40.jpg" width="480" alt="PDP-11/40" />
</p>

The PDP-11 runs 2.11BSD on SIMH. The seed binary was transferred via
uuencode, then the AI agent connected over HTTP and evolved the
firmware through three generations — with two watchdog rollbacks
along the way.

---

## 1. Boot

```
PDP-11 simulator V4.0-0 Current        git commit id: 627e6a6b

70Boot from ra(0,0,0) at 0172150
: ra(0,0)unix

2.11 BSD UNIX #19: Sun Jun 17 16:44:43 PDT 2012
    root@pdp11:/usr/src/sys/ZEKE

ra0: RA72  size=1953300
attaching de0 csr 174510
attaching lo0

phys mem  = 3932160
avail mem = 3553344
user mem  = 307200

March 19 00:17:50 init: configure system
erase, kill ^U, intr ^C
```

## 2. Network

```
# mount /dev/ra0g /usr
# ifconfig lo0 127.0.0.1
# ifconfig de0 10.11.0.2 netmask 255.255.255.0 up
# route add 0 10.11.0.1 1
add net 0: gateway 10.11.0.1
```

## 3. Deploy seed via uuencode

No SCP. No FTP. The only way to move a binary onto 2.11BSD
through the console is uuencode — paste 780 lines of ASCII-encoded
binary into the terminal:

```
# cat > /tmp/seed.uu << 'ZEOF'
begin 751 seed
M!P'Z3V07*!HH$@`````!``GPQN> 4> $`(`1D!T$`(@1R&4&`#<>``#B3P@4
M> 8\AE`@`W$D!G!0KF%2\`YAW*3_<)8#W`"P,#@`HW$+Q/EB7W":`>#A#?"6
...
<780 lines of uuencoded PDP-11 binary>
...
end
ZEOF
# uudecode /tmp/seed.uu
# ls -la /tmp/seed
-rwxr-xr-x  1 root        34900 Mar 19 00:20 /tmp/seed
# /tmp/seed 8080 &
```

## 4. Seed v0.2.0 starts

```
[auth] token: 255002d343fef59da115c5718a4f5051

  Seed v0.2.0 (2.11BSD PDP-11)
  Port: 8080  Token: 255002d3...

[ev] seed started port 8080
```

The AI agent connects. First attempts fail — it doesn't have
the token yet:

```
[ev] auth fail 192.168.1.180
[ev] auth fail 192.168.1.180
[ev] auth fail 192.168.1.180
[ev] auth fail 192.168.1.180
```

Agent reads the token from the console, retries. Connection established.

## 5. Bootstrap v0.2.1 — the 16-bit overflow

**Problem:** The seed accepts max 16KB body (MBODY=16384).
Gen 1 firmware is 26KB. It literally can't fit through the API.

**Solution:** Upload a minimal bootstrap that only changes MBODY.

First attempt — build fails:

```
[ev] src updated 13535 bytes
[ev] build FAIL
```

Agent fixes the source, tries again:

```
[ev] src updated 13059 bytes
[ev] build OK
[ev] apply: swapped, forking watchdog

  Bootstrap v0.2.1 (MBODY=32KB)
  Port: 8080  Token: 255002d3...

[ev] bootstrap port 8080
[watchdog] health OK, new firmware confirmed
```

But wait — MBODY=32768. On a PDP-11, `int` is 16 bits.
`32768` overflows to `-32768`. Every size comparison fails.
The bootstrap appears healthy but can't actually accept uploads.

## 6. Bootstrap v0.2.2 — overflow fixed

Agent discovers the bug. MBODY changed from 32768 to 30000
(fits in 16-bit signed int, max 32767):

```
[ev] src updated 13059 bytes
[ev] build OK
[ev] apply: watchdog

  Bootstrap v0.2.2 (MBODY=30KB)
  Port: 8080  Token: 255002d3...

[ev] bootstrap port 8080
```

Now the node can accept uploads up to 30KB. Gen 1 fits.

## 7. Generation 1: System Monitor v1.0.0

The agent writes 702 lines of K&R C — system monitoring endpoints.
Minified to 15,977 bytes to fit through the API:

```
[ev] src updated 15977 bytes
[ev] build OK
[ev] apply: watchdog

 Gen 1 v1.0.0
 System Monitor
 Port: 8080 Token: 255002d3...

[ev] gen1 started 8080
```

The PDP-11 is no longer a seed. It's a system monitor with 5
new endpoints: `/system/info`, `/system/processes`, `/system/disk`,
`/system/who`, `/system/logs`.

## 8. Failed Gen 2 attempt — watchdog saves the day

Someone tried to grow Gen 1 further. Uploaded progressively
larger firmware, testing the limits:

```
[ev] src 22         ← probe
[ev] src 4000       ← chunked upload
[ev] src 6000
[ev] src 7000
[ev] src 7100
[ev] src 7200
[ev] src 7300
[ev] src 7400
[ev] src 7450
[ev] src 7460       ← too large
[ev] src 4608
[ev] build FAIL     ← compiler error
```

Trimmed it down. Compiled. Applied:

```
[ev] src 4505
[ev] build OK
[ev] apply: swapped, forking watchdog
[tinyboot] 0.9.0 on :8080       ← new firmware booted...
[watchdog] FAIL 1/3              ← health check failed after 10s
                                    AUTOMATIC ROLLBACK
 Gen 1 v1.0.0                   ← Gen 1 restored
[ev] gen1 started 8080
```

**The watchdog worked.** Bad firmware ran for 10 seconds, failed the
health check, and the previous generation was automatically restored.
No manual intervention. No bricked device.

## 9. Second attempt — watchdog saves it again

```
[ev] reset lock                  ← apply lock cleared
[ev] src 4505
[ev] build OK
[ev] apply: swapped, forking watchdog
[tinyboot] 0.9.0 on :8080
[watchdog] FAIL 1/3              ← failed again, rolled back again

 Gen 1 v1.0.0                   ← Gen 1 restored
[ev] gen1 started 8080
```

Two failed growth attempts. Two automatic rollbacks.
Gen 1 survives and keeps running.

## 10. Current state — still alive

```
$ curl http://10.11.0.2:8080/health
{"ok":true, "uptime_sec":9448, "type":"node", "version":"1.0.0",
 "gen":1, "platform":"2.11BSD", "arch":"PDP-11"}
```

```
$ curl http://10.11.0.2:8080/system/info
{"version":"1.0.0", "gen":1, "uptime_sec":9482,
 "uptime":"3:32am  up  3:15,  0 user",
 "root_free_kb":4716, "usr_free_kb":293714,
 "process_count":9}
```

```
$ curl http://10.11.0.2:8080/system/processes
PID TTY TIME COMMAND
  0 ?   0:00 swapper
  1 ?   0:00  (init)
  4 co  0:02 - (sh)
282 co  0:00 seed 8080
```

```
$ curl http://10.11.0.2:8080/system/disk
Filesystem  1K-blocks     Used    Avail Capacity  Mounted on
root             7816     3100     4716    40%    /
/dev/ra0g      405505   111791   293714    28%    /usr
```

```
$ curl http://10.11.0.2:8080/system/logs
vmunix: RA82 size=1954000
vmunix: phys mem = 3932160
vmunix: avail mem = 3553344
vmunix: user mem = 307200
kernel security level changed from 0 to 1
```

```
$ curl http://10.11.0.2:8080/capabilities | jq .
{
  "type": "node",
  "version": "1.0.0",
  "seed": false,
  "gen": 1,
  "arch": "PDP-11",
  "os": "2.11BSD",
  "cpu_model": "PDP-11",
  "cpus": 1,
  "int_bits": 16,
  "mem_kb": 4096,
  "has_cc": true,
  "compiler": "cc (PCC)",
  "tty_devices": [
    "/dev/tty00", "/dev/tty01", "/dev/tty02", "/dev/tty03",
    "/dev/tty04", "/dev/tty05", "/dev/tty06", "/dev/tty07"
  ],
  "endpoints": [
    "/health", "/capabilities", "/config.md", "/events",
    "/firmware/version", "/firmware/source", "/firmware/build",
    "/firmware/build/logs", "/firmware/apply", "/firmware/apply/reset",
    "/system/info", "/system/processes", "/system/disk",
    "/system/who", "/system/logs"
  ]
}
```

---

## Timeline

| Step | Version | What happened |
|------|---------|---------------|
| Deploy | v0.2.0 | Seed binary transferred via uuencode, started on port 8080 |
| Bootstrap 1 | v0.2.1 | Increased MBODY. **Bug:** 32768 overflows 16-bit int to -32768 |
| Bootstrap 2 | v0.2.2 | Fixed MBODY to 30000. Body limit raised successfully |
| **Growth** | **v1.0.0** | **Gen 1: system monitor. 5 new endpoints. 702 lines of K&R C** |
| Gen 2 try | - | Compiled but health check failed. **Watchdog rollback #1** |
| Gen 2 retry | - | Same result. **Watchdog rollback #2** |
| Now | v1.0.0 | Gen 1 running stable. Two rollbacks survived. PDP-11 is alive |

## What this demonstrates

1. **The seed protocol works on hardware from 1975.** K&R C, 16-bit integers, no snprintf, PCC compiler. Same HTTP API.
2. **The watchdog works.** Two bad firmware uploads, two automatic rollbacks, zero manual intervention.
3. **16-bit bugs are real.** MBODY=32768 silently overflows. The agent discovered and fixed it.
4. **Growth is incremental.** Seed → bootstrap (raise limits) → Gen 1 (add features). Each generation compiles the next.
5. **The node is never bricked.** Even after failed firmware, the previous version is always restored.
