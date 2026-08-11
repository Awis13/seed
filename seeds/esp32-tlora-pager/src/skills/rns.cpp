/*
 * skills/rns.cpp — Reticulum stack (microReticulum 0.5.0) + a TCP interface
 *
 * C1 brought the stack and the node identity up with no interface at all. C2
 * gives it one: a Reticulum TCPClientInterface written here, because
 * microReticulum 0.5.0 ships exactly one interface source file — the
 * Interface.h/.cpp base — and the udp_interface under examples/ is not even
 * exported by the library's PlatformIO manifest.
 *
 * Where the pieces live:
 *   rns/hdlc.h            framing codec, no Arduino deps, host-tested by
 *                         tools/test_rns_hdlc.sh
 *   rns/pktfilter.h       the inbound keep/drop decision, likewise pure and
 *                         host-tested (tools/test_rns_filter.sh)
 *   rns/annsched.h        when to announce, same shape, host-tested by
 *                         tools/test_rns_annsched.sh
 *   rns/inbox.h           the one-message handoff from the packet callback to
 *                         the loop task, and the payload sanitiser; same shape
 *                         again, host-tested by tools/test_rns_inbox.sh
 *   RnsTcpInterface       RNS::InterfaceImpl subclass; drain, framing and the
 *                         reconnect state machine all run on the loop task
 *   rns_destination       one IN/SINGLE destination, seed.pager, on the stored
 *                         identity — the address this node answers to. A packet
 *                         sent to it becomes a notification card; tools/rns-send
 *                         is the host side of that
 *   /rns.json (SPIFFS)    {enabled, host, port, peer} — gitignored, it names a host
 *                         behind the tunnel
 *   GET  /rns/status      stack + interface state, counters, last error, the
 *                         destination address and its announce timings, plus
 *                         three loop-task health fields (loop_stack_free_bytes,
 *                         drain_us_max, path_hashes), the prefilter's
 *                         announces_dropped / announces_kept and the receive
 *                         path's six data_* counters; ?reset=1 zeroes the
 *                         drain_us_max rolling max after it has been read
 *   POST /rns/config      upsert of the three fields above
 *
 * BEING ADDRESSABLE HAS A PRICE, and it is paid on the loop task. Once a
 * destination is registered, every path request for its hash that reaches this
 * node is answered by the library, inside the drain, with a full announce:
 * Transport::path_request_handler() finds the hash in _destinations and calls
 * local_destination.announce(..., true, ...), which signs with software Ed25519.
 * The prefilter cannot touch this — a path request is a DATA packet, and the
 * reply is outbound, where no filter runs.
 *
 * THIS IS NOT RATE LIMITED, and an earlier version of this comment claimed it
 * was. The claim was that _discovery_pr_tags dedupes the request with a 30 s
 * window. Both halves were wrong. _discovery_pr_tags is a GenerationalSet
 * bounded by COUNT — RNS_PR_TAGS_MAX, 32 entries (Transport.cpp) — with no time
 * component at all; the 30 s PR_TAG_WINDOW is a TTL on Destination::
 * _path_responses, a different structure serving a different purpose. And the
 * tag it dedupes on is chosen by the REQUESTER: Transport::request_path() fills
 * it with Identity::get_random_hash() when the caller does not supply one. So
 * the set suppresses a literal retransmission of one request and nothing else.
 * N path requests carrying N fresh tags are N announces and N signatures, at
 * whatever rate they arrive. Nor does the set hold 32 tags before it forgets:
 * GenerationalSet keeps two generations and rotates once the active one reaches
 * (max_size + 1) / 2, so its own header puts the effective window at roughly
 * max_size/2 to max_size — as few as 16 distinct tags here, after which even
 * the retransmissions come back.
 *
 * That is the same shape as the prefilter's own limitation — a field the sender
 * controls decides what we pay — and it is stated here for the same reason:
 * this is the cost of having an address, it cannot be prefiltered away, nothing
 * in this file throttles it, and announce_us_max is the number that would show
 * it happening.
 *
 * THE ANNOUNCE PREFILTER is the reason this file is not simply a TCP interface.
 * One accepted announce measured 221 ms of loop task against an 8 ms drain
 * budget, essentially all of it a software Ed25519 verification inside
 * Identity::validate_announce (no ECC accelerator on the ESP32-S3, no Ed25519
 * in mbedTLS). That cost cannot be reduced, so it is not paid: a filter
 * registered on Transport's inbound hook drops every announce that is not a
 * PATH_RESPONSE, ~350 lines of Transport::inbound() before the verification.
 * See the prefilter block further down for what the hook does and does not
 * permit.
 *
 * THE TICK IS NOW ON, and it brings four library behaviours with it that no
 * caller can switch off. They are accepted, not worked around:
 *   - Reticulum::loop() catches std::bad_alloc itself and calls ESP.restart()
 *     unconditionally;
 *   - Interface::send_outgoing() does the same around our send path;
 *   - RNG.loop() writes the entropy seed to NVS on an hourly timer;
 *   - Reticulum::jobs() runs clean_caches() on a 15 min timer (CLEAN_INTERVAL)
 *     and persist_data() on an hourly one (PERSIST_INTERVAL). Both reach the
 *     filesystem, and persist_data() -> Transport::write_path_table() opens
 *     with `while (_saving_path_table) { OS::sleep(0.2); }` under a 5 s
 *     timeout. Our own no-blocking discipline cannot see any of that: it is
 *     third-party code on the far side of one rns_stack.loop() call, with no
 *     hook to gate it on. -DRNS_PERSIST_PATHS=0 compiles the path-table half
 *     of it out (see below); what is left is a directory walk on a long timer.
 *
 * "The data path never allocates" would be a comfortable claim and it is not a
 * true one, so it is not the mitigation. What is true: every buffer in THIS
 * file is a fixed-size static, and one pending TX frame is the most that is
 * ever held. What is also true: the receive path allocates. RNS::Bytes(
 * g_rns_rx.buf, g_rns_rx.len) below copies each frame into a std::vector before
 * handle_incoming() is entered, and Transport::inbound() allocates a good deal
 * more behind that. -DRNS_PERSIST_HASHLIST=0 adds one allocation of its own to
 * that path and it is worth naming: with a working store, the packet hashlist's
 * put() stops short-circuiting on isValid() and BasicHeapStore::put()
 * (HeapStore.h) now sweeps the whole map for expired records and then inserts a
 * std::map node through ContainerAllocator, once per accepted packet, on the
 * loop task. It is bounded — the hashlist is capped at 100 records — but it is
 * not free, and it is the price of the dedupe the store was switched on for.
 * The bound on the exposure is therefore the drain budget — bytes, frames and
 * milliseconds per tick — and not an absence of allocation.
 *
 * Persistence: -DRNS_PERSIST_PATHS=0 and -DRNS_PERSIST_HASHLIST=0 in
 * platformio.ini turn the path store and the packet hashlist from
 * microStore::BasicFileStore into BasicHeapStore. This is a requirement of the
 * drain rather than a preference, but NOT for the reason this comment used to
 * give. The old claim — that at the default the announce path writes SPIFFS
 * from inside the drain loop — is wrong for the configuration this firmware
 * actually runs: Transport::start() calls init() on all three stores inside
 * `if (Reticulum::transport_enabled())` (Transport.cpp), and this node sets
 * transport_enabled(false). A FileStore with no filesystem has isValid() false,
 * so every put() returns false without touching flash. What the default
 * therefore bought was not a flash write but a broken store: `paths` would never
 * count and the packet hashlist would never dedupe anything. BasicHeapStore::
 * isValid() is unconditionally true, so at 0 they work. The flash hazard is real
 * but conditional: it is what the same code would do the day
 * transport_enabled(true) is set, and these flags defuse it in advance. The
 * reasoning is spelled out at the flags themselves.
 *
 * The THIRD store, known destinations, stays at the library default and stays
 * broken on purpose. It was switched to 0 as well and that had to be reverted:
 * a live BasicHeapStore evicts only when policy_max_recs > 0, which meant this
 * file had to call Identity::known_destinations_maxsize() before start() — on a
 * store whose init() the library never runs here — and that build panic-looped
 * on the device and was rolled back. What faulted is NOT established; see
 * platformio.ini, which says so and says why the setter cannot itself be it.
 * The flag is gone because the flag's own justification had
 * already been removed by the prefilter below: it existed because a failed
 * Identity::remember() logged an ERROR through Log.cpp's blocking Serial.flush()
 * once per accepted announce, and remember() is reached from
 * Identity::validate_announce(), which a dropped announce never gets to. Nothing
 * on this node reads the store. See platformio.ini for the full note.
 *
 * The identity below is the one thing that must survive a reboot, and this file
 * persists it without the library's help.
 *
 * Blocking is the central hazard, because arduino-esp32 3.3.9's NetworkClient
 * is only partly non-blocking:
 *   - connect()   blocks up to _timeout (3 s default) plus DNS  -> moved onto a
 *                 one-shot FreeRTOS task, with the connection state as the
 *                 baton; loop() returns immediately while the task owns it
 *   - write()     up to 10 retries around a 1 s select(), i.e. ~10 s worst case
 *                 -> not used at all; we call lwip_send(MSG_DONTWAIT) on the
 *                 socket ourselves and park the unsent tail
 *   - readBytes() loops on a 2 ms sleep until getTimeout() -> never called
 *                 (and the string this sentence avoids spelling is what
 *                 tools/test_task_unblock.py greps this file for)
 *   - available() (FIONREAD ioctl) and read(buf, n) (MSG_DONTWAIT) are safe and
 *     are the only read calls used here.
 *
 * Threading: microReticulum has no locks anywhere, so Transport::inbound must
 * only ever be entered from one task. handle_incoming() is therefore called
 * exclusively from RnsTcpInterface::loop(), which Reticulum::loop() drives from
 * our skill tick on the loop task. The connect task calls nothing in the
 * library — it touches the client socket and one atomic, and exits.
 *
 * WireGuard: the tunnel installs a default netif, so 10.66.0.0/24 needs no
 * bind. Tearing the tunnel down removes that netif and silently kills open
 * sockets, so the state of wg_is_up() is sampled when the link comes up and any
 * change to it drops the link for a clean reconnect.
 *
 * Filesystem: microStore's UniversalFileSystem resolves to the POSIX adapter on
 * ESP32 and would mount LittleFS; this board is SPIFFS, so the SPIFFS adapter
 * is selected via -DUSTORE_USE_SPIFFS. init() is called with reformatOnFail
 * FALSE on purpose — the default true does a probe write and calls
 * SPIFFS.format() if it fails, which would take /mesh_identity.id,
 * /gw_token.txt, /wg.json and the whole notify store with it. The adapter also
 * calls SPIFFS.begin(true, "") itself, so init() only runs once SPIFFS is
 * confirmed mounted: mounting with an empty base path and formatOnFail set is
 * the same partition loss by another route.
 *
 * Order matters: OS::storage_size()/storage_available() are called
 * unconditionally by start() and throw std::runtime_error when no filesystem is
 * registered, so registration comes first and the whole bring-up sits in a
 * try/catch (an escaping exception is a panic, not a degraded skill).
 *
 * Identity (X25519 + Ed25519, 64 private bytes) is stored as hex text at
 * /rns_identity.id through main.cpp's shared write_spiffs_file_atomic(),
 * matching how meshcore.cpp keeps /mesh_identity.id. The helper's name promises
 * more than it delivers and is not corrected here: it is write(tmp) ->
 * remove(path) -> rename(tmp, path), so a power cut between the remove and the
 * rename leaves the file missing rather than half-written. Missing is the safe
 * failure for both files this skill writes — a missing identity is regenerated
 * and a missing /rns.json leaves the interface configured off — but the word
 * "atomic" should not be read as a guarantee anywhere in this file.
 * Generation is gated on SPIFFS.exists():
 * a file that is present but unreadable or malformed is reported, never
 * overwritten.
 */

#include <microStore/Adapters/SPIFFSFileSystem.h>
#include <microReticulum.h>

#include <atomic>
#include <errno.h>
#include <esp_timer.h>   /* esp_timer_get_time() for the drain-time diagnostic */

/* Raw socket send, because NetworkClient::write() can sit for ~10 s.
 *
 * The #undef block below is a guard, not a fix: on this framework all 22 of
 * them are no-ops, and the comment they used to carry — that lwIP
 * macro-renames the BSD names onto lwip_* and would break the two-argument
 * NetworkClient::connect() call further down — was simply wrong. ESP-IDF builds
 * lwIP with LWIP_COMPAT_SOCKETS 0 and LWIP_POSIX_SOCKETS_IO_NAMES 0
 * (framework-arduinoespressif32-libs/esp32s3/include/lwip/port/include/
 * lwipopts.h), which compiles the whole `#define connect(s,name,namelen)
 * lwip_connect(...)` block out of lwip/sockets.h. The BSD names arrive instead
 * from Espressif's wrapper header of the same path
 * (.../include/lwip/include/lwip/sockets.h) as `static inline` FUNCTIONS, and a
 * free function never collides with the two-argument NetworkClient::connect()
 * member call in rns_connect_task() below. So nothing needs undoing, and
 * lwip_send() is called under its own name because that is its name.
 *
 * The block is kept because arduino-esp32's own NetworkClient.cpp carries it
 * and because the cost is zero. It would, however, be an INCOMPLETE guard if
 * LWIP_COMPAT_SOCKETS were ever flipped to 1: the same header also defines
 * read, write, close, fcntl, ioctl, readv and writev as macros under
 * LWIP_POSIX_SOCKETS_IO_NAMES, and none of those seven is listed here.
 *
 * The include sits here rather than at the top of main.cpp so the blast radius
 * is this file and whatever main.cpp defines after it. */
#include <lwip/sockets.h>
#undef accept
#undef bind
#undef closesocket
#undef connect
#undef getpeername
#undef getsockname
#undef getsockopt
#undef inet_ntop
#undef inet_pton
#undef ioctlsocket
#undef listen
#undef poll
#undef recv
#undef recvfrom
#undef recvmsg
#undef select
#undef send
#undef sendmsg
#undef sendto
#undef setsockopt
#undef shutdown
#undef socket

#include "rns/annsched.h"
#include "rns/hdlc.h"
#include "rns/inbox.h"
#include "rns/outbox.h"
#include "rns/pktfilter.h"

/* The other side of the room hook: claude_route_incoming(), the router this
 * file installs at init. Declared in its own header rather than reached for as
 * a bare extern, because the header is also where the loop-safety contract the
 * router promises is written down. skills/agents.cpp, which defines it,
 * includes the same header and is included by main.cpp BEFORE this file — but
 * this include does not lean on that: the header is #pragma once and pulls only
 * <string.h>/<stddef.h>, so it stands on its own wherever it lands. */
#include "../agents_chat_route.h"
#include "../lxmf_codec.h"
#include "../lxmf_route.h"
#include "../lxmf_reply.h"

/* The two wire constants rns/pktfilter.h mirrors, checked against the library's
 * own enums here — this is the only translation unit that sees both. A value
 * drifting in a future microReticulum breaks the firmware build instead of
 * quietly changing which packets the filter drops on the device. */
static_assert(RNS_PKT_TYPE_ANNOUNCE == (uint8_t)RNS::Type::Packet::ANNOUNCE,
              "rns/pktfilter.h ANNOUNCE does not match RNS::Type::Packet");
static_assert(RNS_PKT_CONTEXT_PATH_RESPONSE ==
                  (uint8_t)RNS::Type::Packet::PATH_RESPONSE,
              "rns/pktfilter.h PATH_RESPONSE does not match RNS::Type::Packet");

/* And the third wire constant, for the same reason: rns/inbox.h sizes its one
 * buffer at the largest plaintext a single encrypted packet can carry, and the
 * host test that drives that buffer cannot see the library. If ENCRYPTED_MDU
 * ever moves, this fails the firmware build instead of quietly truncating a
 * message on the device. Both implementations derive 383 from the same
 * arithmetic — RNS/Packet.py ENCRYPTED_MDU, microReticulum Type.h. */
static_assert(RNS_INBOX_PAYLOAD_MAX == (size_t)RNS::Type::Packet::ENCRYPTED_MDU,
              "rns/inbox.h payload ceiling does not match RNS::Type::Packet");

/* The same ceiling from the other direction, and here it is the one thing
 * standing between a caller and wasted public-key time: NOTHING in the library
 * checks the plaintext size before encrypting. Packet::pack() throws
 * std::length_error on the PACKED size, which is after Destination::encrypt()
 * has generated an ephemeral X25519 keypair and run the exchange — a tenth of a
 * second of loop task spent to produce an exception. rns/outbox.h refuses an
 * oversize message at the door instead, and it can only do that against the
 * right number. */
static_assert(RNS_OUTBOX_PAYLOAD_MAX == (size_t)RNS::Type::Packet::ENCRYPTED_MDU,
              "rns/outbox.h payload ceiling does not match RNS::Type::Packet");

/* ---- this node's own destination ----
 *
 * app_name and aspects together are the ADDRESS. Destination::hash() is
 * SHA256(SHA256("app_name.aspects")[:10] || identity.hash())[:16] — 16 bytes,
 * 32 hex characters, which is what `rnpath` and `rnprobe` take as input.
 * Changing either string changes the address, so neither may be edited casually
 * and neither may be squatted in advance: the LXMF delivery destination this
 * node will eventually want is "lxmf.delivery", it is a SEPARATE destination
 * with a SEPARATE address, and naming this one that today would have to be
 * undone the day LXMF arrives.
 *
 * A dot in app_name throws std::invalid_argument out of the constructor
 * (Destination.cpp); aspects is ONE dot-joined string here, unlike Python's
 * varargs form. */
#define RNS_DEST_APP_NAME "seed"
#define RNS_DEST_ASPECTS  "pager"
/* 16-byte destination hash -> 32 hex chars + NUL. */
#define RNS_DEST_ADDR_MAX 33

/* ---- the LXMF delivery destination ----
 *
 * A SECOND IN/SINGLE destination, and a SEPARATE address from seed.pager: this
 * is the one an LXMF client paths to and addresses a message at. app_name is
 * "lxmf" and aspects is "delivery" — the single dot-joined aspects string this
 * port takes (a dot in app_name would throw std::invalid_argument out of the
 * constructor). It is registered, announced and served exactly like seed.pager
 * above; because the (app_name, aspects) pair differs, its hash differs, so
 * Transport::register_destination() cannot collide with our own seed.pager. */
#define RNS_LXMF_APP_NAME "lxmf"
#define RNS_LXMF_ASPECTS  "delivery"

#define RNS_ID_PATH  "/rns_identity.id"
#define RNS_ID_TMP   "/rns_identity.tmp"
/* Identity::get_private_key() is X25519(32) + Ed25519(32). */
#define RNS_PRV_KEY_BYTES  (RNS::Type::Identity::KEYSIZE / 8)
/* Truncated hash is 16 bytes today; the buffer holds a full 32-byte hex too. */
#define RNS_HEXHASH_MAX    65

/* ---- TCP interface tunables ---- */

#define RNS_CFG_FILE  "/rns.json"
#define RNS_CFG_TMP   "/rns.tmp"
#define RNS_TCP_HOST_MAX 64
/* rnsd's default TCPServerInterface listen port. */
#define RNS_TCP_PORT_DEFAULT 4242
/* Interface name. It feeds toString(), which feeds get_hash(), which is the
 * key Transport dedupes and looks interfaces up by — so it is a compile-time
 * constant and must never grow the endpoint into it. */
#define RNS_TCP_IFACE_NAME "rns-tcp"
/* Largest frame accepted or emitted. Twice the RNS MTU (500) leaves room for
 * IFAC overhead and any future link-MTU growth without buffering a peer's
 * garbage: 1 KiB of RX frame + 2 KiB of TX scratch + 256 B of read chunk is
 * the whole memory cost of this interface. */
#define RNS_TCP_HW_MTU 1024
/* Frames at or below the RNS minimum header cannot be packets. */
#define RNS_TCP_FRAME_MIN RNS::Type::Reticulum::HEADER_MINSIZE
/* Announce pacing only; nothing measures the link. WiFi-order guess, same as
 * the reference TCP interface uses. */
#define RNS_TCP_BITRATE 10000000UL

#define RNS_TCP_CONNECT_TIMEOUT_MS 3000
#define RNS_TCP_CONNECT_TASK_STACK 4096
#define RNS_TCP_CONNECT_TASK_PRIO  1
#define RNS_TCP_BACKOFF_MIN_MS 3000UL
#define RNS_TCP_BACKOFF_MAX_MS 60000UL

/* Drain budget — three independent caps, all checked between reads. A single
 * read chunk is always parsed to the end (leftover bytes have nowhere to live),
 * so the frame cap is a floor, not a ceiling: one 256-byte chunk can carry a
 * few more short frames than RNS_TCP_DRAIN_FRAMES_MAX. */
#define RNS_TCP_READ_CHUNK       256
#define RNS_TCP_DRAIN_BYTES_MAX  1024
#define RNS_TCP_DRAIN_FRAMES_MAX 8
#define RNS_TCP_DRAIN_BUDGET_MS  8UL

/* The inbound message ring must hold everything ONE drain pass can deliver: the
 * drain runs to its frame budget inside a tick and the pickup runs once per
 * tick, so a shallower ring turns a burst into drops by construction. This is
 * the coupling that keeps the two numbers in step when either of them moves —
 * the frame budget is the cause and the ring depth is the consequence, and the
 * build is where that gets noticed rather than the device. */
static_assert(RNS_INBOX_SLOTS >= RNS_TCP_DRAIN_FRAMES_MAX,
              "rns/inbox.h ring is shallower than one drain pass");

/* lwip_send() attempts per flush before the tail waits for the next tick. */
#define RNS_TCP_TX_FLUSH_TRIES 4
/* A half-written frame leaves the peer's decoder mid-frame, so a tail that
 * will not drain cannot simply be dropped — the socket goes instead. */
#define RNS_TCP_TX_STALL_MS 2000UL

/* Skill tick period. Everything the stack does on its own is interval-gated
 * (Transport jobs 60 s, RNG hourly); only the drain wants to be prompt. */
#define RNS_TICK_MS 20UL

enum RnsConnState {
    RNS_CS_IDLE = 0,   /* no socket, nobody owns the client */
    RNS_CS_CONNECTING, /* the connect task owns the client — do not touch it */
    RNS_CS_CONNECTED,
    RNS_CS_FAILED      /* the connect task finished and lost */
};

/* ---- state ---- */
static bool rns_started = false;         /* Reticulum::start() returned */
static bool rns_identity_ok = false;
static bool rns_identity_created = false; /* generated this boot, not loaded */
static const char *rns_error = nullptr;   /* static literal, or NULL when fine */
static char rns_hexhash[RNS_HEXHASH_MAX] = {0};

/* File scope for lifetime clarity, not because a local would dangle:
 * OS::register_filesystem() stores a COPY of the wrapper and the wrapper holds
 * a shared_ptr to the adapter implementation, so the implementation would
 * survive a local going out of scope. Keeping the wrapper here means the one
 * we call init() on and the one the library holds are the same object. */
static microStore::FileSystem rns_filesystem{microStore::Adapters::SPIFFSFileSystem()};
static RNS::Reticulum rns_stack{RNS::Type::NONE};
static RNS::Identity rns_identity{RNS::Type::NONE};

/* The destination that makes this node addressable. File scope for the same
 * reason as rns_stack and rns_identity: Transport::register_destination() keeps
 * a COPY in its _destinations map and the announce path looks it up there, but
 * the announce schedule below calls announce() on this handle, so the handle has
 * to outlive bring-up.
 *
 * It is constructed with rns_identity, and that is not an incidental detail: the
 * Destination constructor treats a NONE identity on an IN/SINGLE destination as
 * a request to MINT ONE (Destination.cpp: "identity not provided, creating new
 * one"), silently, with no error and no persistence. The address would then
 * change on every boot and nothing would say so. Never construct this from
 * anything but the loaded identity, and never before rns_identity_ok.
 *
 * THE ADDRESS IS ONLY AS STABLE AS THE IDENTITY FILE, and there is a likelier
 * way to lose it than the NONE-identity mistake warned about above. If
 * /rns_identity.id does not exist, rns_identity_bring_up() mints a keypair and
 * tries to save it; when that write fails it sets rns_error to "identity
 * generated but could not be saved" and carries on with rns_identity_ok true.
 * That is the right call for the identity — the node still works this boot —
 * but it means the destination is built on a key that dies at reboot, so the
 * address in /rns/status is good until power is cut and then silently
 * different. Anything published as a permanent contact for this node has to be
 * checked against a boot that reports created:false and no error. */
static RNS::Destination rns_destination{RNS::Type::NONE};
static bool rns_dest_ok = false;
static char rns_dest_addr[RNS_DEST_ADDR_MAX] = {0};

/* The LXMF delivery destination. File scope and the same lifetime rules as
 * rns_destination above, including the one that matters most: it is built ONLY
 * from the loaded identity, never from a NONE identity — an IN/SINGLE
 * destination handed NONE silently mints a throwaway keypair (Destination.cpp)
 * and the address would move on every boot with nothing saying so. */
static RNS::Destination rns_lxmf_destination{RNS::Type::NONE};
static bool rns_lxmf_dest_ok = false;
static char rns_lxmf_dest_addr[RNS_DEST_ADDR_MAX] = {0};

/* ---- TCP interface state ----
 *
 * All of it is file scope so the ownership rules can be stated in one place.
 *
 * g_rns_cs is the baton between the loop task and the one-shot connect task.
 * While it reads RNS_CS_CONNECTING the connect task owns g_rns_client and the
 * loop task must not read or write the socket; every other value means the
 * loop task owns it and no connect task exists. It is a std::atomic rather
 * than a volatile int for the ordering, not the atomicity: the connect task
 * fills in the client's internals and g_rns_last_error before publishing the
 * new state, and the loop task must not see the state without them.
 *
 * g_rns_host/g_rns_port are written ONLY by the loop task, and only while no
 * connect task exists, so the task always reads a stable endpoint. POST
 * /rns/config runs on the AsyncTCP task and therefore only writes SPIFFS and
 * raises g_rns_cfg_dirty; the loop task re-reads the file. */
static NetworkClient g_rns_client;
static std::atomic<int> g_rns_cs{RNS_CS_IDLE};

static bool g_rns_cfg_ok = false;      /* a usable host is configured */
static bool g_rns_cfg_enabled = false; /* ...and the operator wants it up */
static volatile bool g_rns_cfg_dirty = false; /* raised by POST /rns/config */
static char g_rns_host[RNS_TCP_HOST_MAX] = "";
static uint16_t g_rns_port = RNS_TCP_PORT_DEFAULT;

/* THE PEER: the node this device talks to, as 32 hex characters, or empty.
 *
 * It is in /rns.json beside the endpoint and not in the chat skill's own
 * storage, because it is an RNS address and this file is where an RNS address
 * is validated, applied and published. It is DELIBERATELY NOT part of the
 * endpoint change detection in rns_cfg_load(): the host and port decide which
 * socket to hold open, and changing them drops a live link, while the peer only
 * decides where the next message is addressed. Restarting the interface for a
 * new peer would be a reconnect nobody asked for.
 *
 * WRITTEN by the loop task in rns_cfg_load(), READ by rns_peer_addr() on
 * whichever task typed or posted the message — so both sides go through
 * g_rns_tx_mux below rather than trusting a 33-byte array to be copied
 * atomically. GET /rns/status reads it from the loop-side snapshot, like every
 * other string here. */
static char g_rns_peer[RNS_OUTBOX_ADDR_HEX + 1] = "";

/* The producer lock the ring's header names as the caller's job.
 *
 * A SPINLOCK AND NOT A MUTEX, because of what it guards: the critical section
 * is one rns_envelope_build() and one rns_outbox_put(), i.e. bounded memcpys of
 * at most RNS_OUTBOX_PAYLOAD_MAX bytes with no call that can block, throw,
 * allocate or log. Microseconds. A FreeRTOS mutex would add a scheduler round
 * trip and a priority-inheritance dance to protect a memcpy; portENTER_CRITICAL
 * cannot deadlock here (nothing inside it takes another lock) and cannot invert
 * priority (nothing inside it yields).
 *
 * It also covers g_rns_peer, whose reader may be either task. */
static portMUX_TYPE g_rns_tx_mux = portMUX_INITIALIZER_UNLOCKED;

static char g_rns_last_error[64] = "";
static uint32_t g_rns_attempts = 0;    /* connect attempts since boot */
static uint32_t g_rns_downs = 0;       /* established links subsequently lost */
static uint32_t g_rns_backoff_ms = RNS_TCP_BACKOFF_MIN_MS;
static uint32_t g_rns_next_try_ms = 0;
static uint32_t g_rns_up_ms = 0;
static uint32_t g_rns_frames_in = 0;
static uint32_t g_rns_tx_dropped = 0;
static uint32_t g_rns_rx_errors = 0;   /* frames Transport::inbound threw on */
static bool g_rns_wg_seen = false;     /* wg_is_up() when the link came up */
static bool g_rns_mtu_warned = false;  /* the over-MTU event is emitted once */

/* Fixed buffers, sized once. Nothing in the data path allocates. */
static uint8_t g_rns_rdbuf[RNS_TCP_READ_CHUNK];
static uint8_t g_rns_framebuf[RNS_TCP_HW_MTU];
static uint8_t g_rns_txbuf[RNS_TCP_HW_MTU * 2 + 2];
static rns_hdlc_rx g_rns_rx;
/* One frame deep: bytes [g_rns_tx_sent, g_rns_tx_len) still owe the socket. */
static size_t g_rns_tx_len = 0;
static size_t g_rns_tx_sent = 0;
static uint32_t g_rns_tx_pending_ms = 0;

static RNS::Interface g_rns_iface{RNS::Type::NONE};

/* ---- loop-task health diagnostics ----
 *
 * Three numbers a phase-2 test wants to watch continuously, all produced on the
 * loop task and all read (never mutated live) by GET /rns/status.
 *
 * g_loop_stack_free_bytes is uxTaskGetStackHighWaterMark(NULL) sampled in the
 * RNS tick, i.e. the minimum free stack the loop task has ever had. On this
 * Arduino-ESP32 3.3.9 / ESP-IDF 5 build the return is in BYTES, not words:
 * .../framework-arduinoespressif32-libs/esp32s3/include/freertos/
 * FreeRTOS-Kernel/include/freertos/task.h documents it as "the minimum free
 * stack space there has been in bytes (as opposed to words in the standard
 * FreeRTOS documentation)". The unit is baked into the field name so no reader
 * has to guess the 4x. The high-water mark is already the worst case, so it is
 * stored and reported as-is.
 *
 * g_drain_us_max is the rolling maximum microseconds a single drain() pass has
 * spent on the loop task (esp_timer_get_time). GET /rns/status?reset=1 zeroes
 * it after the read so a fresh worst case can be measured over a chosen window.
 * It is a naturally aligned 32-bit scalar; the loop task's `if (dt > max)
 * max = dt` and the handler's reset can only ever cost one lost sample, the
 * same hazard class the other direct-read counters already accept.
 *
 * g_path_hash is a loop-side snapshot of the destination hashes in Transport's
 * path table (RNS::Transport::_new_path_table). That table is inserted into by
 * Transport::inbound() on the loop task, so iterating it from the AsyncTCP
 * status handler would race a concurrent insert and invalidate the iterator.
 * The snapshot is filled in rns_status_publish() — the same place, and the same
 * task, that already reads the table's size — and the handler serves ONLY this
 * buffer. It is bounded to RNS_PATH_HASH_MAX 16-byte hashes as 32-char hex;
 * g_path_hash_n is how many slots are filled and g_rns_snap.paths carries the
 * full count, published as path_hashes_total so a truncated array is visible. */
#define RNS_PATH_HASH_MAX 8
/* 16-byte truncated destination hash -> 32 hex chars + NUL. */
static char g_path_hash[RNS_PATH_HASH_MAX][33];
static volatile uint8_t g_path_hash_n = 0;
static uint32_t g_loop_stack_free_bytes = 0;
static uint32_t g_drain_us_max = 0;

/* ---- inbound packet prefilter ----
 *
 * Transport::inbound() offers one hook, set_filter_packet_callback, and calls it
 * roughly 350 lines before Identity::validate_announce(). That gap is the whole
 * point: an announce this node has no use for costs two field reads here
 * instead of an Ed25519 verification that measured 221 ms on this board.
 *
 * The decision itself is rns_filter_keep_packet() in rns/pktfilter.h — a pure
 * function of packet type and context, host-tested by tools/test_rns_filter.sh.
 * What lives here is only the plumbing, and every line of it is constrained:
 *
 *   - The hook is a BARE FUNCTION POINTER (Transport::Callbacks::filter_packet
 *     is `bool(*)(const Packet&)`), so there is nowhere to put state except file
 *     scope. Hence the two statics below rather than anything captured.
 *   - It runs inside the drain, on the loop task, once per inbound packet, so it
 *     must be O(1), allocate nothing and have no side effect beyond those two
 *     counters. Packet::packet_type() and Packet::context() are inline reads of
 *     the packet object's own fields; neither copies, allocates or throws.
 *   - The hook is FAIL-OPEN, but only for one kind of throw. Transport.cpp
 *     initialises `accept = true` and wraps the call in
 *     `catch (const std::exception&)`, so a std::exception out of here leaves
 *     the packet accepted and silently disables the filter — the safe
 *     direction. Anything NOT derived from std::exception escapes
 *     Transport::inbound() instead, and inbound() sets `_jobs_locked = true` on
 *     entry and clears it only on the normal way out; Transport::jobs() is
 *     guarded by `if (!_jobs_locked)`, so it would never run again for the rest
 *     of the boot. That is the real reason nothing in this function is allowed
 *     to be ABLE to throw, and it is a contract for whoever edits it next: the
 *     escape hatch below you is narrower than it looks.
 *   - A DROPPED PACKET NEVER ENTERS THE PACKET HASHLIST, because Transport only
 *     records the hash on the accept path. The same announce arriving twice is
 *     therefore judged twice, and the judgement must not differ — which is why
 *     the decision function reads nothing but its arguments and the counters
 *     below are written after it, never read by it.
 *
 * The counters are announce-specific on purpose: every non-announce packet is
 * kept unconditionally, so counting those would only measure traffic. kept +
 * dropped is the number of announces that reached the filter. */
static uint32_t g_rns_ann_dropped = 0;
static uint32_t g_rns_ann_kept = 0;

static bool rns_packet_filter(const RNS::Packet &packet) {
    uint8_t type = (uint8_t)packet.packet_type();
    uint8_t context = (uint8_t)packet.context();
    bool keep = rns_filter_keep_packet(type, context);
    if (type == RNS_PKT_TYPE_ANNOUNCE) {
        if (keep) {
            g_rns_ann_kept++;
        } else {
            g_rns_ann_dropped++;
        }
    }
    return keep;
}

/* ---- inbound data callback ----
 *
 * Destination::set_packet_callback() is what stands between "a packet addressed
 * to us was decrypted" and "a packet addressed to us was decrypted and thrown
 * away". Without it Destination::receive() unpacks the packet, verifies nothing
 * is listening and returns, so an inbound DATA packet would leave no trace at
 * all and the first thing anyone sent us would look identical to a packet that
 * never arrived.
 *
 * THE CALLBACK MAY NOT RAISE THE CARD, which is the whole shape of this file's
 * receive path. notify_ingest() calls time(NULL) (notify.cpp), takes the
 * notify_mux critical section (notify_push) and appends to the event ring
 * (event_add, main.cpp) — three things on the forbidden list below, in a
 * function that runs inside the drain with Transport::_jobs_locked set.
 * Persistence is a FOURTH cost and no longer a dirty flag: notify_push()
 * write-throughs the card to the history archive (notify_archive_put ->
 * history_enqueue, skills/history.cpp), which is a 0-tick xQueueSend onto the
 * archive's own write task. That one neither blocks nor touches SD on this
 * task, but it does spend ~1.3 KB of stack on the record and queue-item
 * temporaries — on a stack that is already deep inside Transport::inbound()
 * here. So the callback stores the bytes in
 * g_rns_inbox and returns, and rns_inbox_poll() raises the card from the loop
 * task after rns_stack.loop() has come back. Same deferral cluster as
 * g_rns_cfg_dirty further down and as skills/wg.cpp's request flags: the
 * forbidden context raises, the permitted one consumes. The ring's rules —
 * its depth, and what happens to a message that arrives with every slot full —
 * live in rns/inbox.h and are host-tested by tools/test_rns_inbox.sh.
 *
 * THREE PROPERTIES OF THE PAYLOAD THAT A READER WILL OTHERWISE ASSUME WRONG:
 *
 *   - IT IS NOT NUL-TERMINATED and its pointer DANGLES once the callback
 *     returns. Destination::receive() builds the plaintext as a local Bytes
 *     (Destination.cpp) and Bytes::data() hands back the vector's storage
 *     (Bytes.h), so the length has to be carried everywhere and `%s` on it is a
 *     bug, not a shortcut.
 *   - IT CAN NEVER BE EMPTY HERE, and that is a silent failure mode rather than
 *     a convenience. Destination::receive() calls this callback only inside
 *     `if (plaintext)`, and Bytes::operator bool is `_data && !_data->empty()`
 *     (Bytes.h), so a peer sending a zero-length message produces no callback,
 *     no counter, no card and no error — on either end. The sender sees a
 *     successful send. tools/rns-send refuses an empty payload for exactly this
 *     reason; anything else that ever talks to this destination has to as well.
 *   - ITS BYTES ARE ARBITRARY. notify_ingest() filters nothing, so a control
 *     byte reaches the screen renderer as-is and a NUL would end the card body
 *     early, and there is no guarantee the payload is valid UTF-8. It is passed
 *     through rns_text_sanitize() (rns/inbox.h) on the loop task before it
 *     becomes a card: newline kept, every other control byte and 0x7F rendered
 *     as '.', well-formed UTF-8 sequences copied whole, everything else '?'.
 *
 * WHAT AN INBOUND PACKET COSTS BEFORE THIS FUNCTION IS EVEN REACHED, because
 * the rest of this comment enumerates costs we avoid and that would be a
 * misleading list on its own. Destination::receive() hands every non-LINKREQUEST
 * packet matching our hash to decrypt() -> Identity::decrypt(), which runs an
 * X25519 exchange against the sender's ephemeral public key, an HKDF, and a
 * token decrypt — on the loop task, inside the drain, BEFORE anything has been
 * authenticated and regardless of whether it ever will be. Anyone who knows the
 * address can make us do that. Nothing in this file can decline it: the
 * prefilter runs on packet type and context, and a packet addressed to us is a
 * DATA packet it keeps by policy.
 *
 * THE BLIND SPOT THAT FOLLOWS FROM THAT, stated because it would otherwise be
 * read out of /rns/status backwards: Identity::decrypt() swallows its own
 * failures and returns empty, and Destination::receive() only calls this
 * callback when the plaintext is non-empty. So a flood of garbage addressed to
 * our hash costs an X25519 per packet and leaves data_rx at ZERO. data_rx
 * counts packets that decrypted, not packets that arrived; the flood would show
 * up in iface.rx and drain_us_max, and nowhere else.
 *
 * THE SAME BUDGET AS THE PREFILTER APPLIES, for the same reasons, and the
 * constraints are worth restating rather than referring to:
 *
 *   - Destination::Callbacks::packet is `void(*)(const Bytes&, const Packet&)`,
 *     a BARE FUNCTION POINTER, so there is nowhere for state to live except file
 *     scope. Both arguments are const references and MUST STAY references, but
 *     not for the reason an earlier version of this comment gave: it claimed a
 *     by-value Bytes was "a heap copy of the packet payload", and it is not.
 *     Bytes.h defines COW unconditionally (Bytes.h:62) and Bytes::assign() is a
 *     shared_ptr copy — "shared_ptr copy only — O(1), no heap allocation"
 *     (Bytes.h:247-251). Nor can such a local free anything on the way out:
 *     Destination::receive()'s own plaintext outlives this callback and holds
 *     the other reference, so the count goes 2 -> 1 and nothing is released.
 *     What a by-value local DOES cost is an atomic increment and decrement on
 *     the loop task, and one live hazard that is worth the rule on its own:
 *     the copy is non-exclusive, so the first MUTATION of it — an append, a
 *     resize, anything that is not a read — calls Bytes::exclusiveData(), which
 *     is `new Data()` plus a reserve and a full copy of the payload
 *     (Bytes.cpp), inside the drain, and which throws std::runtime_error after
 *     logging an ERROR through Log.cpp's blocking Serial.flush() when the
 *     allocation fails. A reference cannot be mutated by accident; a by-value
 *     local invites exactly that edit. tools/test_task_unblock.py's allowlist is
 *     what keeps the argument a reference.
 *   - It runs on the loop task, inside the 8 ms drain, reached from
 *     handle_incoming() -> Transport::inbound() -> Destination::receive().
 *     So: no logging (Log.cpp ends every emitted line with a blocking
 *     Serial.flush()), no String, no allocation, no SPIFFS, no clock.
 *   - It must not be ABLE to throw, though the consequence is narrower here than
 *     at the prefilter and this file should not overstate it. Destination::
 *     receive() already wraps the callback in `catch (const std::exception&)`
 *     and only logs (Destination.cpp), so a std::exception out of here is
 *     absorbed one frame up. What is NOT absorbed is anything not derived from
 *     std::exception: that unwinds out of Destination::receive() into
 *     Transport::inbound(), which sets _jobs_locked = true on entry and clears
 *     it only on the normal way out, leaving Transport::jobs() disabled for the
 *     rest of the boot. The rule stands; the reason it stands is the second
 *     case, not the first.
 *
 * A memcpy into a fixed buffer and a handful of scalar stores satisfy all of
 * that. They all live in rns_inbox_put(), which is why the body below is one
 * line.
 *
 * The Packet is deliberately unused. Nothing in it names the sender — a SINGLE
 * destination packet carries the DESTINATION hash in its header, not a source —
 * so the card cannot say who sent it, and this file will not guess. Anyone who
 * knows the address can raise a card on this screen; that is a property of the
 * address, and a display name belongs to LXMF rather than to a bare packet.
 */

/* The messages in flight, the pickup scratch and the card they become. All
 * three are fixed-size statics: same rule as the interface buffers above, and
 * the loop task's stack is shared with the drain and with Transport::inbound(),
 * which is deep enough already without ~600 bytes of locals once per tick.
 *
 * The ring is exactly one drain pass deep — RNS_INBOX_SLOTS is checked against
 * RNS_TCP_DRAIN_FRAMES_MAX by a static_assert above — because one tick can admit
 * that many packets and the pickup runs once per tick, so anything shallower
 * answers an ordinary burst with drops. See rns/inbox.h for the sizing and for
 * what happens when even a whole pass is not enough. */
static rns_inbox g_rns_inbox;
static uint8_t g_rns_inbox_payload[RNS_INBOX_PAYLOAD_MAX];
static char g_rns_card_body[NOTIFY_BODY_LEN];
/* The message as the ROOM sees it, and the reason it is a second buffer rather
 * than the card body: the card is cut to what the screen paints — about 185
 * codepoints — and may carry a "[+N B]" prefix, and a room that stored THAT
 * would be a conversation with the ends of its messages missing and a marker
 * for the screen inside its text. A room is the durable copy, so it gets the
 * whole text, sanitised but uncut. Sized at the payload ceiling plus a
 * terminator, because the sanitiser never writes more bytes than it reads. */
static char g_rns_room_text[RNS_INBOX_PAYLOAD_MAX + 1];
/* Cards raised, and cards whose body could not hold the whole message. The
 * second one is not optional bookkeeping: a payload can be 383 bytes and the
 * card paints far less than that, so without this a long message would lose its
 * tail with nothing anywhere saying it had. The card itself carries the same
 * fact — see rns_inbox_poll(), which is also where the limit that matters is
 * explained. It is NOT notify's 240-byte storage: using that number was the bug
 * that shipped a truncation marker at an offset the renderer never reaches. */
static uint32_t g_rns_cards = 0;
static uint32_t g_rns_card_cut = 0;

/* ---- the inbound envelope ----
 *
 * WHAT THESE COUNT, and why "not an envelope" is a first-class outcome rather
 * than a failure. The peer answers in the agreed format and those payloads are
 * taken apart; tools/rns-send and every sender that predates the format send
 * bare text, and those are shown exactly as they arrive. Both are normal, so
 * both are counted, and the pair is what tells an operator which kind of sender
 * is on the other end of a card they are looking at.
 *
 * g_rns_env_raw_why is the last reason a payload was not read as an envelope.
 * It is a pointer to a static literal out of rns_envin_reason() — never
 * formatted, never owned — and it answers the one question the counter cannot:
 * "my peer says it sent an envelope, so why is the framing on my screen?".
 *
 * g_rns_env_raw_why IS LAST-REASON-WINS, which is exactly wrong for the case
 * worth seeing. tools/rns-send is in ordinary use and every one of its packets
 * writes "not an envelope: plain text", so a single BAD_FROM or BAD_SESSION
 * from a probing sender — the shapes this field exists to surface — is
 * overwritten by the next ordinary message and never seen. So the malformed
 * shapes get a slot of their own that plain text cannot overwrite. Four bytes,
 * and it is the difference between a diagnostic and a decoration.
 *
 * g_rns_env_from is the sender address of the last envelope, and it is HELD
 * FOR REPLYING, not for reading: a packet to a SINGLE destination carries no
 * source field, so this string is the only thing that makes an answer possible,
 * and it is exactly what POST /rns/send takes as `to`. It is deliberately NOT
 * on the card — 32 hex characters of address are 32 characters of message the
 * screen does not show — and it is published in GET /rns/status instead, which
 * is where something that intends to answer will look for it.
 *
 * ⚠ AND IT IS WHATEVER THE SENDER TYPED. THIS COMMENT IS THE CONTRACT ANY
 * REPLY FEATURE WILL BE BUILT AGAINST, so it says the dangerous half plainly: a
 * DATA packet to a SINGLE destination carries no source field and no signature,
 * and the parser proves only that these 32 characters are HEXADECIMAL. Nothing
 * anywhere ties them to the node that sent the packet. This device announces
 * its address every 30 minutes, so anyone who has heard that announce can send
 * `1|<somebody else's address>|beacon|ok`, and the first thing that answers
 * data_from would encrypt a reply to a node the user has never talked to —
 * addressed by an attacker, at a time of the attacker's choosing. Treat this
 * field as a HINT the user confirms, never as provenance. The card's own
 * caveat is the same one from the other side: anyone who knows this address can
 * raise a card on this screen.
 *
 * ⚠ AND IT CAN TEAR, differently from every other string in the snapshot. The
 * rule everywhere else is that a reader crossing a copy sees mixed characters,
 * which is ugly and detectable. Two ADDRESSES that interleave produce a third
 * address that is still 32 valid hex characters — a well-formed hash belonging
 * to nobody, which no consumer can tell from a real one. Nothing auto-replies
 * today, so it is left as it is; anything that starts replying without a human
 * in the loop needs the address carried with the message rather than read from
 * this slot afterwards. */
/* ...AND A THIRD OUTCOME THAT IS NEITHER, which is why it gets a counter of its
 * own rather than being folded into the pair above. A CONTROL FRAME — an
 * envelope whose session is the reserved single byte '*' — is not a chat
 * message and not a malformed packet: the peer is telling this device which
 * rooms are live, and the payload is a room list. It raises no card, so
 * data_cards cannot see it; it parses cleanly, so data_raw must not claim it;
 * and counting it as an ordinary envelope would make data_envelopes climb while
 * nothing appears on the screen, which is the confusing shape this counter
 * exists to prevent. THE THREE ARE DISJOINT: every payload the pickup takes
 * increments exactly one of env_parsed, env_control and env_raw, so
 * "the node is talking to us and we ignore it" reads differently from "nothing
 * arrives". */
static uint32_t g_rns_env_parsed = 0;   /* payloads read as a chat envelope */
static uint32_t g_rns_env_control = 0;  /* ...read as a control frame */
static uint32_t g_rns_env_raw = 0;      /* payloads shown exactly as they came */
static const char *g_rns_env_raw_why = nullptr;
/* ...and the last reason that was NOT "plain text", kept where the common case
 * cannot overwrite it. */
static const char *g_rns_env_bad_why = nullptr;
static char g_rns_env_from[RNS_OUTBOX_ADDR_HEX + 1] = {0};

/* ---- the room router ----
 *
 * A FUNCTION POINTER, NULL UNTIL SOMEBODY SETS IT. The skill that owns the
 * conversations is what decides where a message lands, and its helper does not
 * exist in this tree yet — so this side calls through a pointer instead of
 * naming a symbol nobody has written. Null is not a degraded mode: it is the
 * behaviour that shipped before the hook existed, a card and nothing else.
 * The contract (loop task, no synchronous SD, no blocking, called once per
 * parsed envelope) is written out in rns/outbox.h next to the typedef.
 *
 * g_rns_env_refuse_reason is whatever the router wrote on its last false
 * return. Its meaning belongs to the router; this file only carries it to
 * GET /rns/status, because a message that did not reach a room is invisible
 * everywhere else — the card still appears, so nothing on the screen says the
 * conversation never got it. */
static rns_room_router g_rns_room_router = nullptr;
static uint32_t g_rns_env_routed = 0;    /* the router accepted the message */
static uint32_t g_rns_env_refused = 0;   /* ...and these it refused */
static int g_rns_env_refuse_reason = 0;

/* Set once at init by the skill that owns the rooms. Not static: the caller is
 * another translation-unit-in-name-only included elsewhere in this build, and a
 * setter it cannot name is a hook that does not exist. */
void rns_set_room_router(rns_room_router fn) {
    g_rns_room_router = fn;
}

static void rns_data_callback(const RNS::Bytes &data, const RNS::Packet &packet) {
    (void)packet;
    rns_inbox_put(&g_rns_inbox, data.data(), data.size());
}

/* ---- the LXMF delivery inbox ----
 *
 * The lxmf.delivery destination's own receive path, and it obeys EXACTLY the
 * discipline rns_data_callback documents above — this is a second callback on
 * the loop task inside the same 8 ms drain, reached the same way
 * (Transport::inbound() -> Destination::receive()). So the callback is a memcpy
 * into a dedicated ring and nothing else: no logging, no String, no allocation,
 * no SPIFFS, no clock, no mutex, and the RNS::Bytes argument stays a reference
 * for the refcount/COW reason spelled out above. The LXMF parse — msgpack today,
 * an Ed25519 verify in a later commit, tens of milliseconds either way — runs in
 * rns_lxmf_inbox_poll() on the loop task AFTER rns_stack.loop() returns, never
 * here and never in the drain.
 *
 * The ring is a second rns_inbox: same 383-byte slot (the LXMF message rides
 * inside one encrypted single packet, so its plaintext shares the ENCRYPTED_MDU
 * ceiling) and same "keep the first, count the drop" overflow policy. The parsed
 * message is a file-scope static rather than a poll-stack local because sizeof
 * (LxmfMsg) is ~1 KB and the loop task's stack is shared with the drain. */
static rns_inbox g_rns_lxmf_inbox;
static uint8_t g_rns_lxmf_payload[RNS_INBOX_PAYLOAD_MAX];
static LxmfMsg g_rns_lxmf_msg;
/* Disjoint from the three seed.pager receive outcomes (env_parsed/control/raw):
 * data_lxmf_ok is a payload that parsed as LXMF, data_lxmf_bad one that reached
 * the destination and did NOT — the "device alive but did not understand"
 * signal. data_lxmf_rx (the packets that arrived) is g_rns_lxmf_inbox.received. */
static uint32_t g_rns_lxmf_ok = 0;
static uint32_t g_rns_lxmf_bad = 0;
/* Where the parsed messages WENT, split the way the seed.pager path splits
 * data_cards from data_envelopes: data_lxmf_cards is a parsed message routed to
 * a notification card (the default, plain-client path), data_lxmf_rooms one
 * carrying a thread and handed to the chat router instead. They partition the
 * data_lxmf_ok total, so a gap between them and it is a message that parsed but
 * whose card the notify store refused. Loop-task-written scalars, like the pair
 * above. */
static uint32_t g_rns_lxmf_cards = 0;
static uint32_t g_rns_lxmf_rooms = 0;
/* LXMF replies built, signed and handed to the send ladder (C4). Disjoint from
 * every other counter: it is the OUT side, the mirror of data_lxmf_rooms on the
 * IN side. Loop-task- OR AsyncTCP-written (rns_send_lxmf_reply runs on whichever
 * task the reply came from) but a plain uint32_t is safe here — the increment is
 * a single naturally-aligned store and GET /rns/status reads it single-copy. */
static uint32_t g_rns_lxmf_tx = 0;

/* The room -> last-LXMF-sender map. Set by rns_lxmf_inbox_poll() when a message
 * routes into a room, cleared by rns_inbox_poll() when a seed.pager envelope
 * lands in one, read by rns_lxmf_reply_target() from the reply path. Guarded by
 * g_rns_tx_mux: the set/clear run on the loop task and the lookup can run on the
 * AsyncTCP task (POST /agents/send), so it is a two-task structure like the
 * outbox. The critical sections are a strcmp over 8 short names and a 16-byte
 * copy — short enough for the same spinlock the outbox producer uses. */
static LxmfOriginTable g_rns_lxmf_origin;

static void rns_lxmf_data_callback(const RNS::Bytes &data, const RNS::Packet &packet) {
    (void)packet;
    rns_inbox_put(&g_rns_lxmf_inbox, data.data(), data.size());
}

/* ---- outbound state ----
 *
 * The mirror of the inbox, and it is not symmetric. Receiving hands a payload
 * from one context of the loop task to another; sending hands a payload from
 * ANOTHER TASK to the loop task, and then spends real time resolving where it
 * goes before it can be encrypted.
 *
 * WHY THE SEND MAY NOT HAPPEN IN THE HTTP HANDLER. POST /rns/send answers on
 * the AsyncTCP task. A send from there would call Identity::recall(), which
 * walks Transport's path table, and Transport::outbound(), which spins on
 * _jobs_running and then sets _jobs_locked — both of them structures the loop
 * task mutates inside rns_stack.loop(). It would also pay for an X25519 key
 * exchange on the socket task. So the handler validates, builds the envelope
 * and enqueues; the loop task does everything else. rns/outbox.h holds the ring
 * and the memory ordering that makes a two-task handoff safe.
 *
 * WHY A STATE MACHINE AND NOT A CALL. A send to a destination we have no path
 * for DOES NOT FAIL — it goes nowhere, silently. Transport::outbound() looks
 * the destination up in the path table, and with no entry it falls through to
 * the broadcast branch and emits a HEADER_1 packet on every interface; the hub
 * on the other end of our TCP socket has no reason to forward it and drops it
 * without a word. receipt_send() still returns a receipt, because a receipt
 * means "an interface accepted it" and one did. That is the single most
 * important fact on this path and the reason this block exists at all: the
 * resolve is not an optimisation before the send, it is the PRECONDITION for
 * the send, and nothing below may call receipt_send() unless
 * Transport::has_path() said yes AND Identity::recall() handed back a real
 * identity.
 *
 * Neither of those can be waited for. Transport::request_path() returns void,
 * this port has no await_path() and its announce handlers explicitly skip path
 * responses, so there is no completion to hook. Polling has_path() across ticks
 * is the whole mechanism — and a busy-wait would be worse than untidy, because
 * the loop task is what drains the socket, so blocking here prevents the frame
 * carrying the path response from ever being read. Hence: one small step per
 * tick, request the path once, retry on a throttle, give up with a reason.
 *
 * WHY THE RESOLVED HANDLES ARE CACHED. Identity::recall() is expensive AND
 * noisy on this node, for a reason specific to this port. The ordinary
 * known-destinations store is a BasicFileStore whose init() sits behind
 * transport_enabled, which is false here, so remember()/recall() through it
 * always fail. What makes recall() work at all is the library's
 * RNS_IDENTITY_ANNOUNCE_RECALL divergence: it digs the destination's last
 * announce out of the path table, which means decoding a msgpack record,
 * unpacking a Packet, and then calling remember() — which fails again and emits
 * an ERRORF, and Log.cpp ends every emitted line with a blocking
 * Serial.flush() on the calling task. So the Destination built from it is held
 * for the send.
 *
 * CACHING THE SUCCESS IS NOT ENOUGH, and the first version of this file got
 * that wrong. Holding the resolve behind g_rns_out_resolved bounds recall() to
 * one call per message only when it SUCCEEDS: when has_path() is true and
 * recall() returns NONE, every rung re-enters and pays for it again. That is
 * reachable without anybody misbehaving — has_path() is
 * Persistence::exists(), a key probe that never decodes, while recall()'s
 * failure path is a full _new_path_table.get(), i.e. the msgpack decode and the
 * Packet unpack — so a path entry whose stored announce will not reconstruct
 * keeps the cheap probe true and buys seven expensive failures on the task that
 * owns the socket drain.
 *
 * So a second latch, g_rns_out_recalled, bounds the ATTEMPTS rather than the
 * successes. It is cleared by exactly one event: firing request_path(). That is
 * deliberate and it is the reason the latch is not simply "once per message" —
 * the answer recall() gives can only change when a fresh announce for that
 * destination arrives, a path response IS such an announce, and asking for one
 * is the only way this node can cause it. So the bound is TWO recalls per
 * message: one that discovers the problem, and one after we have asked for the
 * thing that would fix it. Never seven.
 *
 * ONE MESSAGE IN FLIGHT AT A TIME. The queue is not a pipeline: a message can
 * occupy the ladder for a minute, so overlapping them would multiply the path
 * requests without making anything arrive sooner. g_rns_out_busy is what says
 * one is being worked on.
 *
 * An OUT destination is NOT registered with Transport — register_destination()
 * takes the IN branch only (Transport.cpp) — so constructing one per message is
 * safe and cannot collide with our own IN destination's hash. */
static rns_outbox g_rns_outbox;
/* The message being worked on, as fixed statics for the reason the inbox's
 * are: the loop task's stack is shared with the drain and with
 * Transport::inbound(). */
static char g_rns_out_to[RNS_OUTBOX_ADDR_HEX + 1] = {0};
static uint8_t g_rns_out_payload[RNS_OUTBOX_PAYLOAD_MAX];
static uint16_t g_rns_out_len = 0;
static bool g_rns_out_busy = false;         /* a message is on the ladder */
static uint8_t g_rns_out_tries = 0;         /* attempts spent on it */
static uint32_t g_rns_out_last_try_ms = 0;
static bool g_rns_out_path_asked = false;   /* request_path() fired once */
/* recall() has been ATTEMPTED since the last path request. Not "since the
 * message started": see CACHING THE SUCCESS IS NOT ENOUGH above for why the
 * clear is tied to request_path() and what the bound therefore is. */
static bool g_rns_out_recalled = false;
static bool g_rns_out_resolved = false;     /* g_rns_out_dest is usable */
static RNS::Destination g_rns_out_dest{RNS::Type::NONE};
/* Why the current message has not gone out yet. It becomes the published
 * failure reason if the ladder runs out, which is what turns "it vanished" into
 * something an operator can read. */
static const char *g_rns_out_why = nullptr;

/* Published counters. Written by the loop task, read by GET /rns/status: the
 * same naturally-aligned single-copy-atomic arrangement g_rns_ann_us_last and
 * friends already use. The queued depth and the failure reason go through the
 * snapshot instead — the depth because it is derived from two atomics the
 * AsyncTCP task also writes to, and the reason because it is a char array the
 * loop task rewrites in place, exactly like g_rns_last_error. */
static uint32_t g_rns_sent = 0;          /* an interface accepted the packet */
static uint32_t g_rns_send_failed = 0;   /* messages given up on, with a reason */
static uint32_t g_rns_send_us_last = 0;
static uint32_t g_rns_send_us_max = 0;
/* The LAST failure, kept after the message it belonged to is gone.
 *
 * A BUFFER AND NOT A `const char *`, and the difference is the whole value of
 * the field. A pointer can only ever hold a literal chosen from a fixed list,
 * so a std::length_error out of pack(), a std::invalid_argument and a
 * std::bad_alloc all arrive at the operator as one indistinguishable sentence —
 * on a board whose console nobody is watching, which is the only place e.what()
 * would otherwise go. It also carries the DESTINATION, because several messages
 * can be queued and "a send failed" does not say which one.
 *
 * Written only by the loop task, in one place (rns_send_retire), and read only
 * through the snapshot. Empty until something has actually failed. */
static char g_rns_send_error[96] = {0};

/* ---- offering a message: the ONE producer path ----
 *
 * THERE IS EXACTLY ONE OF THESE AND THERE MUST BE. POST /rns/send used to hold
 * the validate-build-enqueue sequence inline, and it was the only way to send.
 * It is not any more: a line typed in a chat room goes out over Reticulum too,
 * and that call arrives from skills/agents.cpp. Giving the room its own copy of
 * the three steps would be a second set of rules — a second address check, a
 * second envelope builder, a second ceiling — that nothing forces to stay a
 * copy of the first, and rns/outbox.h opens by explaining what a drifting
 * emitter costs. So the sequence lives here and both callers reach it.
 *
 * IT MAY RUN ON EITHER TASK, which is the other reason it is one function.
 * POST /rns/send and POST /agents/send answer on AsyncTCP; the keyboard path
 * runs on the loop task. The ring underneath is single-producer, so the
 * spinlock is what makes "either task" true — see THERE IS NOW MORE THAN ONE
 * PRODUCER TASK in rns/outbox.h. The section covers the build as well as the
 * put, and not because the build touches shared state (it does not, `env` is on
 * the caller's stack): it costs nothing to include and it keeps the rule
 * readable as "one task is inside the producer at a time" rather than as a
 * subtlety about which statements count.
 *
 * IT REACHES NOTHING IN THE STACK. No Transport, no Identity, no Destination,
 * no allocation, no logging, no clock — exactly the constraints the HTTP
 * handler was already written under, now enforced for both callers by there
 * being one body to read. */
typedef enum {
    RNS_TX_OK = 0,
    RNS_TX_NO_ADDR,  /* this node has no address of its own to sign the envelope with */
    RNS_TX_BAD_TO,   /* the destination is not 32 hex characters */
    RNS_TX_BUILD,    /* rns_envelope_build() refused; its reason is carried out */
    RNS_TX_FULL      /* the ring is full: four messages are already waiting */
} rns_tx_result;

/* `why` receives a SHORT STATIC LITERAL on every failure — short because its
 * destination is a chat line on a 320-pixel screen, static because there is
 * nothing on this path allowed to allocate. `out_len` is the envelope size, for
 * the HTTP handler's reply. Both may be NULL. */
static rns_tx_result rns_tx_offer(const char *to_hex, const char *session,
                                  const char *text, const char **why,
                                  uint16_t *out_len) {
    uint8_t env[RNS_OUTBOX_PAYLOAD_MAX];
    uint16_t len = 0;
    rns_env_result r;
    bool queued;

    if (why) *why = nullptr;
    if (out_len) *out_len = 0;

    /* The envelope carries THIS node's address so the peer can answer, and
     * without it there is nothing worth sending: a packet to a SINGLE
     * destination has no source field, so a message from an unaddressed node is
     * one the recipient can read and never reply to. */
    if (!rns_dest_ok || !rns_addr_valid(rns_dest_addr)) {
        if (why) *why = "no address yet";
        return RNS_TX_NO_ADDR;
    }
    if (!rns_addr_valid(to_hex)) {
        if (why) *why = "bad peer address";
        return RNS_TX_BAD_TO;
    }

    portENTER_CRITICAL(&g_rns_tx_mux);
    r = rns_envelope_build(rns_dest_addr, session, text, env, sizeof(env), &len);
    queued = (r == RNS_ENV_OK) && rns_outbox_put(&g_rns_outbox, to_hex, env, len);
    portEXIT_CRITICAL(&g_rns_tx_mux);

    if (r != RNS_ENV_OK) {
        if (why) *why = rns_env_reason(r);
        return RNS_TX_BUILD;
    }
    if (!queued) {
        if (why) *why = "outbox full";
        return RNS_TX_FULL;
    }
    if (out_len) *out_len = len;
    return RNS_TX_OK;
}

/* Declared in rns/outbox.h. The room's way in, and a thin one on purpose: the
 * decision about what "available" means belongs to the offer above, so a caller
 * that has a peer address and a line of text needs to know nothing else. */
bool rns_send_envelope(const char *to_hex, const char *session,
                       const char *text, const char **why) {
    return rns_tx_offer(to_hex, session, text, why, nullptr) == RNS_TX_OK;
}

/* Declared in rns/outbox.h. False — and an empty buffer — when no peer is
 * configured, which is the failure a user can actually fix and therefore the
 * one a room reports first. */
bool rns_peer_addr(char *out, size_t cap) {
    bool ok;
    if (!out || cap == 0) return false;
    portENTER_CRITICAL(&g_rns_tx_mux);
    ok = g_rns_peer[0] != '\0' && strlen(g_rns_peer) < cap;
    if (ok) memcpy(out, g_rns_peer, strlen(g_rns_peer) + 1);
    portEXIT_CRITICAL(&g_rns_tx_mux);
    if (!ok) out[0] = '\0';
    return ok;
}

/* ---- LXMF-origin reply (TLORA-LXMF C4) ----------------------------------- *
 *
 * The IN half (rns_lxmf_inbox_poll) records which room last received an LXMF
 * message and from whom; these are the OUT half a chat room uses to answer that
 * sender AS LXMF instead of over the seed.pager envelope. Declared in
 * rns/outbox.h for the reason rns_send_envelope() is: the header is the contract
 * skills/agents.cpp (compiled first) and skills/rns.cpp both include.
 *
 * rns_lxmf_reply_target() answers "does this room owe its reply to an LXMF
 * sender?" — true plus the 16-byte destination when so. It takes g_rns_tx_mux
 * because the map is written by the loop task and this runs on either the loop
 * task (a keyboard reply) or the AsyncTCP task (POST /agents/send). */
bool rns_lxmf_reply_target(const char *room, uint8_t out[LXMF_HASH_LEN]) {
    if (!room || !room[0]) return false;
    bool found;
    portENTER_CRITICAL(&g_rns_tx_mux);
    found = lxmf_origin_lookup(&g_rns_lxmf_origin, room, out) != 0;
    portEXIT_CRITICAL(&g_rns_tx_mux);
    return found;
}

/*
 * Build, sign and enqueue an LXMF reply to `dest` (the original sender's 16-byte
 * lxmf.delivery hash, from rns_lxmf_reply_target). Returns true when the packed
 * message was handed to the send ladder (g_rns_outbox) — NOT delivered; the
 * outcome is GET /rns/status, like every other send. On false, *why (nullable)
 * names the one fault a caller can act on.
 *
 * WHERE THE WORK RUNS. Everything here is RAM plus software crypto: the C1
 * encode, one SHA-256 (Identity::full_hash) and one Ed25519 sign
 * (rns_identity.sign), then a spinlock-guarded rns_outbox_put(). It NEVER
 * touches the Reticulum stack and NEVER blocks on the network — the loop task
 * resolves the path and encrypts in rns_send_poll(), exactly as for a seed.pager
 * envelope. The sign (tens of ms) runs on the CALLER's task: the loop task for a
 * keyboard reply (ui_reply_submit runs OUTSIDE rns_stack.loop()'s 8 ms drain —
 * the same placement class as the announce sign) and the AsyncTCP task for POST
 * /agents/send (crypto only, no shared stack state). It is never in the drain,
 * and the sign is done BEFORE the spinlock, never inside it.
 *
 * FULL-PACKED ON THE WIRE, dest hash included: that is what this port's C3
 * receive parser consumes (nothing prepends the destination hash to the callback
 * plaintext here). Standard LXMF opportunistic omits the 16 leading bytes and
 * the receiver prepends its own destination hash; making the two interoperate is
 * a coordinated receive+send change and a separate ticket. Full-packed keeps the
 * reply consistent with C1/C3 and round-trips device<->device.
 *
 * REUSES the seed.pager send ring, which is payload-agnostic: it carries bytes
 * to a 32-hex SINGLE destination and rns_send_poll() recall()s the identity,
 * builds an OUT/SINGLE Destination and sends. The peer's lxmf.delivery
 * destination is announced (C2 announces ours; a peer does likewise), so recall
 * resolves it. If it is not yet announced the ladder retries and gives up with a
 * reason — the graceful drop this path wants, for free. */
bool rns_send_lxmf_reply(const uint8_t *dest, const char *text,
                         const char **why) {
    if (why) *why = nullptr;
    if (!dest || !text || !text[0]) { if (why) *why = "empty reply"; return false; }
    if (!rns_started || !rns_identity_ok) { if (why) *why = "rns not ready"; return false; }
    if (!rns_lxmf_dest_ok) { if (why) *why = "no lxmf address"; return false; }

    /* OUR OWN lxmf.delivery hash is the reply's source, so the peer can answer. */
    uint8_t src[LXMF_HASH_LEN];
    try {
        const RNS::Bytes h = rns_lxmf_destination.hash();
        if (h.size() < LXMF_HASH_LEN || !h.data()) { if (why) *why = "no lxmf address"; return false; }
        memcpy(src, h.data(), LXMF_HASH_LEN);
    } catch (...) { if (why) *why = "no lxmf address"; return false; }

    /* Build the bounded single-packet reply (empty title, no fields). Stack
     * locals, not statics: this can be entered from two tasks at once, so a
     * shared scratch would race. `scratch` is reused for the sign input and then
     * the packed wire (the sign input is copied into a Bytes before the reuse),
     * which keeps the frame to one LxmfMsg plus one 400-byte buffer. */
    LxmfMsg m;
    lxmf_reply_build(&m, dest, src, (double)time(nullptr), text);

    uint8_t scratch[400];   /* >= max(sign-input 320, wire 368) for a 256B body */
    size_t slen = 0;
    if (lxmf_encode_sign_input(&m, scratch, sizeof(scratch), &slen) != LXMF_OK) {
        if (why) *why = "reply too long";
        return false;
    }

    uint8_t sig[LXMF_SIG_LEN];
    try {
        /* signed_part = hashed_part + full_hash(hashed_part); Ed25519 sign it. */
        RNS::Bytes signed_part(scratch, slen);
        RNS::Bytes mhash = RNS::Identity::full_hash(RNS::Bytes(scratch, slen));
        signed_part.append(mhash);
        RNS::Bytes s = rns_identity.sign(signed_part);
        if (s.size() < LXMF_SIG_LEN || !s.data()) { if (why) *why = "sign failed"; return false; }
        memcpy(sig, s.data(), LXMF_SIG_LEN);
    } catch (...) { if (why) *why = "sign failed"; return false; }

    size_t wlen = 0;
    if (lxmf_encode_packed(&m, sig, scratch, sizeof(scratch), &wlen) != LXMF_OK) {
        if (why) *why = "reply too long";
        return false;
    }

    /* dest hash -> the 32-hex address the outbox validates and the ladder sends
     * to (the peer's lxmf.delivery SINGLE destination). */
    char to_hex[RNS_OUTBOX_ADDR_HEX + 1];
    static const char HEXD[] = "0123456789abcdef";
    for (int i = 0; i < LXMF_HASH_LEN; i++) {
        to_hex[i * 2]     = HEXD[dest[i] >> 4];
        to_hex[i * 2 + 1] = HEXD[dest[i] & 0x0F];
    }
    to_hex[RNS_OUTBOX_ADDR_HEX] = '\0';

    bool queued;
    portENTER_CRITICAL(&g_rns_tx_mux);
    queued = rns_outbox_put(&g_rns_outbox, to_hex, scratch, wlen);
    portEXIT_CRITICAL(&g_rns_tx_mux);
    if (!queued) { if (why) *why = "outbox full"; return false; }

    g_rns_lxmf_tx++;
    return true;
}

/* ---- announce state ----
 *
 * The schedule itself is rns_announce_due() in rns/annsched.h — pure, host
 * tested by tools/test_rns_annsched.sh. What lives here is the state it reads
 * and the measurement it is worth.
 *
 * WHY THE TIMING IS INSTRUMENTED. Destination::announce() is fully synchronous
 * on the calling task: it builds the announce, signs it with software Ed25519
 * (rweather — the same library whose verify measured 221 ms on this chip) and
 * hands it to Transport::outbound() before returning. All of that lands on the
 * loop task, in the same tick that owns the socket drain. Nobody has measured
 * the sign side on this board; a signature is normally cheaper than a
 * verification, but "normally" is not a number and this file does not guess at
 * numbers. announce_us_last and announce_us_max are how that number gets known.
 *
 * These are plain uint32_t written on the loop task and read by GET /rns/status
 * on the AsyncTCP task, which is the same naturally-aligned single-copy-atomic
 * arrangement g_rns_attempts and g_rns_downs already use — not the snapshot,
 * because unlike the path table there is no container here to iterate and
 * nothing to be seen half-written. rns_dest_addr is written once during bring-up
 * and never again. */
static bool g_rns_announced = false;    /* an announce has been ATTEMPTED */
static bool g_rns_ann_was_online = false;
/* An offline->online transition is owed an announce. Sticky: set by
 * rns_announce_edge_latch(), cleared by NOTHING except an announce firing. This
 * is what makes RNS_ANNOUNCE_RECONNECT_MIN_MS a deferral rather than a filter —
 * the edge itself lasts one tick, so a floor that read the raw transition would
 * throw away every reconnect that happened within a minute of an announce and
 * push it out to the full 30 min interval. See rns/annsched.h. */
static bool g_rns_ann_edge_pending = false;
static uint32_t g_rns_ann_last_ms = 0;
static uint32_t g_rns_ann_sent = 0;     /* announce() returned without throwing */
static uint32_t g_rns_ann_us_last = 0;
static uint32_t g_rns_ann_us_max = 0;

/* ---- status snapshot ----
 *
 * GET /rns/status answers on the AsyncTCP task, and every interesting number it
 * wants is mutated by the loop task: Transport's path table is an std::map that
 * Transport::inbound() inserts into (sizing it under a concurrent insert is
 * undefined, not merely stale), the base-class rx/tx counters are non-atomic
 * increments, and g_rns_last_error is a char array that snprintf rewrites in
 * place — a reader crossing that write can see it without a terminator.
 *
 * Nothing is locked. The loop task publishes this snapshot once per tick and
 * the handler serves only the snapshot, so the worst case is RNS_TICK_MS of
 * staleness, which is nothing next to an HTTP poll. The strings are copied with
 * a memcpy that stops one byte short of the end, and that last byte is never
 * written after this initialiser: a reader that crosses the copy can therefore
 * see a mix of old and new characters, but never an unterminated array.
 *
 * The remaining direct reads in rns_status_json() — g_rns_attempts, g_rns_downs
 * and friends — are naturally aligned 32-bit scalars, single-copy atomic on
 * this core, and carry no such hazard. */
struct RnsStatusSnap {
    const char *state;
    bool online;
    uint32_t interfaces;
    uint32_t paths;
    uint32_t up_age_s;
    uint32_t rx, tx, rxbytes, txbytes;
    /* Prefilter counters. They are written by rns_packet_filter() on the loop
     * task and would be single-copy atomic to read directly, like g_rns_attempts
     * — they are copied here anyway so that "the status handler reads the
     * snapshot" stays a rule with no exceptions worth arguing about. */
    uint32_t ann_dropped, ann_kept;
    /* Outbound queue depth. It is derived from two std::atomic counters, one of
     * which the AsyncTCP task writes, so it is sampled on the loop task like
     * everything else here rather than computed inside the handler. */
    uint32_t send_queued;
    /* Offers the ring had no room for. This USED to be read straight out of
     * g_rns_outbox.refused by the handler, on the argument that the AsyncTCP
     * task was the only thing that ever incremented it. That argument died with
     * the second producer: a line typed on the keyboard is offered from the loop
     * task, so the counter is now published from the loop side like the rest. */
    uint32_t send_refused;
    /* Why the last message was given up on, empty if none ever was. A char
     * array the loop task rewrites in place, so it is copied here under the
     * same rule as last_error below: the copy stops one byte short and that
     * last byte is never written after this initialiser, so a reader crossing
     * the copy can see mixed characters but never an unterminated array. */
    char send_error[96];
    /* The inbound envelope, published under the same rule as the prefilter
     * counters above: the five counters would be single-copy atomic to read
     * directly, and they are copied here anyway so the handler has one source.
     * env_control is the third of the disjoint outcomes — a frame whose session
     * is the reserved '*', which raises no card — and it is published for the
     * reason the pair is: nothing on the screen can say the peer is talking to
     * this device about its rooms.
     * The last two are not scalars at all and have no choice — `env_raw_why` is
     * a pointer the loop task reassigns and `env_from` is a char array it
     * rewrites in place, which is exactly the hazard send_error is copied for.
     * env_reason is the ROUTER's own code, carried and not interpreted. */
    uint32_t env_parsed, env_control, env_raw, env_routed, env_refused;
    int env_reason;
    const char *env_raw_why;
    const char *env_bad_why;
    char env_from[RNS_OUTBOX_ADDR_HEX + 1];
    uint16_t port;
    char host[RNS_TCP_HOST_MAX];
    /* The configured peer, copied under the same one-byte-short rule as host:
     * the loop task rewrites g_rns_peer in place when /rns.json changes. */
    char peer[RNS_OUTBOX_ADDR_HEX + 1];
    char last_error[64];
};
static RnsStatusSnap g_rns_snap = {"off", false, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, {0},
                                   0, 0, 0, 0, 0, 0, nullptr, nullptr, {0},
                                   RNS_TCP_PORT_DEFAULT, {0}, {0}, {0}};

/* Declared in rns/outbox.h, and defined HERE rather than beside rns_tx_offer()
 * because this is the first line at which the snapshot exists.
 *
 * The link as the loop task last published it: the socket is connected AND the
 * interface reports itself online, which is the same pair GET /rns/status calls
 * `iface.online`. It reads the SNAPSHOT rather than g_rns_iface because the
 * caller may be the AsyncTCP task and the Interface object is the loop task's;
 * a bool the loop task stores is a single-copy-atomic read, and one tick of
 * staleness is nothing against a send that can take a minute. */
bool rns_link_up(void) {
    return g_rns_snap.online;
}

/* ---- configuration ---- */

/* Hostname or dotted quad; the same conservative charset /wg/peer accepts. */
static bool rns_valid_host(const char *host) {
    if (!host || !host[0]) return false;
    size_t n = strlen(host);
    if (n >= RNS_TCP_HOST_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return false;
    }
    return true;
}

/* Re-read /rns.json into the endpoint the connect task will use. Returns true
 * when the endpoint or the enable flag changed, which is the caller's cue to
 * drop an established link. Loop task only. */
static bool rns_cfg_load() {
    char prev_host[RNS_TCP_HOST_MAX];
    snprintf(prev_host, sizeof(prev_host), "%s", g_rns_host);
    uint16_t prev_port = g_rns_port;
    bool prev_enabled = g_rns_cfg_enabled;

    g_rns_cfg_ok = false;
    g_rns_cfg_enabled = false;
    g_rns_host[0] = '\0';
    g_rns_port = RNS_TCP_PORT_DEFAULT;

    /* Built locally and published in ONE locked store at the end, rather than
     * cleared here and filled later: a reader crossing the middle of that would
     * see no peer at all and report "no peer address" for a device that has
     * one. */
    char peer[RNS_OUTBOX_ADDR_HEX + 1] = "";

    String json = read_spiffs_file(RNS_CFG_FILE);
    JsonDocument doc;
    if (json.length() > 0 &&
        deserializeJson(doc, json) == DeserializationError::Ok) {
        String host = doc["host"] | "";
        long port = doc["port"] | (long)RNS_TCP_PORT_DEFAULT;
        if (rns_valid_host(host.c_str()) && port > 0 && port <= 65535) {
            snprintf(g_rns_host, sizeof(g_rns_host), "%s", host.c_str());
            g_rns_port = (uint16_t)port;
            g_rns_cfg_ok = true;
            /* Absent "enabled" means yes, matching /wg.json: a file that names
             * an endpoint was written to be used. */
            g_rns_cfg_enabled = doc["enabled"] | true;
        }
        /* The peer is read OUTSIDE the host branch on purpose: it is a
         * destination address, not part of the endpoint, and a node whose
         * /rns.json has a bad host still has a valid peer to address. Validated
         * with the same rns_addr_valid() POST /rns/send puts a caller's `to`
         * through — a peer that is not 32 hex characters is no peer, because
         * Bytes::assignHex() would decode the typo to a DIFFERENT hash and
         * every message would be encrypted into the void. */
        const char *p = doc["peer"] | "";
        if (rns_addr_valid(p)) snprintf(peer, sizeof(peer), "%s", p);
    }

    portENTER_CRITICAL(&g_rns_tx_mux);
    memcpy(g_rns_peer, peer, sizeof(g_rns_peer));
    portEXIT_CRITICAL(&g_rns_tx_mux);

    /* The peer is NOT in this comparison. See g_rns_peer: the return value is
     * "drop the live link and redial", and a new peer address is no reason to. */
    return (g_rns_cfg_enabled != prev_enabled) || (g_rns_port != prev_port) ||
           (strcmp(g_rns_host, prev_host) != 0);
}

/* ---- connect task ----
 *
 * The whole reason this task exists: NetworkClient::connect() resolves DNS and
 * then blocks in the connect handshake for up to _timeout. On the loop task
 * that is a visible UI stall and a missed keyboard scan; here it costs one
 * short-lived 4 KB stack. The task calls nothing in microReticulum and touches
 * no shared state beyond the client, the error string and the baton. */
static void rns_connect_task(void *arg) {
    (void)arg;
    g_rns_client.setConnectionTimeout(RNS_TCP_CONNECT_TIMEOUT_MS);
    int ok = g_rns_client.connect(g_rns_host, g_rns_port);
    if (ok == 1) {
        /* RNS packets are small and latency-sensitive; Nagle would sit on
         * them waiting for company that is not coming. */
        g_rns_client.setNoDelay(true);
        g_rns_last_error[0] = '\0';
        g_rns_cs.store(RNS_CS_CONNECTED);
    } else {
        g_rns_client.stop();
        /* No endpoint in the text: a 63-character host would not fit and
         * iface.host / iface.port already carry it in /rns/status. */
        snprintf(g_rns_last_error, sizeof(g_rns_last_error),
                 "connect refused or unreachable");
        g_rns_cs.store(RNS_CS_FAILED);
    }
    vTaskDelete(NULL);
}

/* ---- the interface ---- */

class RnsTcpInterface : public RNS::InterfaceImpl {
public:
    RnsTcpInterface() : RNS::InterfaceImpl(RNS_TCP_IFACE_NAME) {
        _IN = true;
        _OUT = true;
        _bitrate = RNS_TCP_BITRATE;
        _HW_MTU = RNS_TCP_HW_MTU;
        _online = false;
    }

protected:
    /* No socket is opened here. WiFi is not necessarily up when skills
     * register, and a failed dial at boot would be a blocking wait in setup().
     * Bring-up is decided in loop(), the same deferral meshcore.cpp uses for
     * the radio. */
    virtual bool start() override {
        _online = false;
        rns_hdlc_rx_init(&g_rns_rx, g_rns_framebuf, sizeof(g_rns_framebuf),
                         RNS_TCP_FRAME_MIN);
        return true;
    }

    virtual void stop() override { link_down("stopped"); }

    /* Transport::detach_interfaces() on shutdown. Same teardown; idempotent
     * because link_down() only logs when the link was actually up. */
    virtual void detach() override { link_down("detached"); }

    virtual void loop() override;
    virtual bool send_outgoing(const RNS::Bytes &data) override;

    /* Deliberately constant. get_hash() is Identity::full_hash({toString()}),
     * and that hash is what Transport dedupes on registration and looks up by
     * later — an endpoint in this string would change the interface's identity
     * every time /rns.json is edited.
     *
     * The flip side, and the reason this class is a singleton in practice: a
     * SECOND instance would produce the same string and therefore the same
     * hash, and Transport::register_interface() silently skips an interface
     * whose hash is already in the table — no error, no return value, the
     * interface simply never carries traffic. Worse, deregister_interface()
     * erases by hash with remove_if, so tearing one down would remove both.
     * Anything that ever wants two of these has to make the name distinct
     * first, and accept that the hash then changes with it. */
    virtual std::string toString() const override {
        return "TCPInterface[" RNS_TCP_IFACE_NAME "]";
    }

private:
    void link_down(const char *why);
    void begin_connect();
    void drain();
    void tx_flush();
};

void RnsTcpInterface::link_down(const char *why) {
    /* The connect task owns the socket. Let it finish and publish; the next
     * tick tears down from RNS_CS_FAILED or reconnects. This guard is what
     * makes link_down() safe to call from stop() and detach() too. */
    if (g_rns_cs.load() == RNS_CS_CONNECTING) return;

    if (_online) {
        g_rns_downs++;
        event_add("rns tcp down: %s", why);
    }
    _online = false;
    snprintf(g_rns_last_error, sizeof(g_rns_last_error), "%s", why);
    /* Safe from the loop task only because every caller has already checked
     * that no connect task owns the client. */
    g_rns_client.stop();
    rns_hdlc_rx_reset(&g_rns_rx);
    g_rns_tx_len = 0;
    g_rns_tx_sent = 0;
    g_rns_tx_pending_ms = 0;
    g_rns_cs.store(RNS_CS_IDLE);
    g_rns_next_try_ms = millis() + g_rns_backoff_ms;
    if (g_rns_backoff_ms < RNS_TCP_BACKOFF_MAX_MS)
        g_rns_backoff_ms *= 2;
    if (g_rns_backoff_ms > RNS_TCP_BACKOFF_MAX_MS)
        g_rns_backoff_ms = RNS_TCP_BACKOFF_MAX_MS;
}

void RnsTcpInterface::begin_connect() {
    g_rns_attempts++;
    /* Publish the baton BEFORE the task exists, so there is no window in which
     * a running task is not visible as the owner. */
    g_rns_cs.store(RNS_CS_CONNECTING);
    if (xTaskCreate(rns_connect_task, "rns_conn", RNS_TCP_CONNECT_TASK_STACK,
                    nullptr, RNS_TCP_CONNECT_TASK_PRIO, nullptr) != pdPASS) {
        /* Nothing ran, so taking the baton back is unambiguous. */
        snprintf(g_rns_last_error, sizeof(g_rns_last_error),
                 "connect task could not be created");
        g_rns_cs.store(RNS_CS_FAILED);
    }
}

/* Push whatever the socket will take right now. lwip_send with MSG_DONTWAIT
 * either moves bytes or returns EAGAIN immediately — no select, no retry loop,
 * no 10 s stall. A short write parks the tail for the next tick. */
void RnsTcpInterface::tx_flush() {
    if (g_rns_tx_sent >= g_rns_tx_len) return;

    int sock = g_rns_client.fd();
    if (sock < 0) {
        /* Reachable without any error surfacing on our side:
         * NetworkClient::available() and read() both call stop() internally
         * when the rxBuffer faults (NetworkClient.cpp), which closes the fd
         * under a parked tail. Clearing the tail quietly here would lose a
         * frame that send_outgoing() already reported as sent, so it counts as
         * a drop and the link goes down for a clean redial — link_down() clears
         * the tail on the way. */
        g_rns_tx_dropped++;
        link_down("socket disappeared");
        return;
    }

    for (int i = 0; i < RNS_TCP_TX_FLUSH_TRIES && g_rns_tx_sent < g_rns_tx_len;
         i++) {
        int n = lwip_send(sock, g_rns_txbuf + g_rns_tx_sent,
                          g_rns_tx_len - g_rns_tx_sent, MSG_DONTWAIT);
        if (n > 0) {
            g_rns_tx_sent += (size_t)n;
            continue;
        }
        /* lwIP defines EWOULDBLOCK as EAGAIN, so one test covers both. */
        if (n < 0 && errno == EAGAIN) break;
        /* Both failure exits count the tail they are about to discard, and both
         * count it exactly once regardless of who called tx_flush() — which is
         * why send_outgoing() only reports the link state and does not count a
         * drop of its own. */
        g_rns_tx_dropped++;
        link_down("socket refused the write");
        return;
    }

    if (g_rns_tx_sent >= g_rns_tx_len) {
        g_rns_tx_len = 0;
        g_rns_tx_sent = 0;
        g_rns_tx_pending_ms = 0;
    } else if (g_rns_tx_pending_ms == 0) {
        g_rns_tx_pending_ms = millis();
    }
}

/* Read at most RNS_TCP_READ_CHUNK bytes at a time and hand every complete
 * frame straight to Transport. available() is a FIONREAD ioctl and read() uses
 * MSG_DONTWAIT, so neither can wait; the three budgets bound how much work one
 * tick may do with the data they return. */
void RnsTcpInterface::drain() {
    /* Rolling max of the whole pass, measured with a scope guard rather than an
     * inline t0/dt pair because drain() has an early return partway through the
     * inner loop — that path still did handle_incoming() work worth timing, and
     * a guard credits every exit exactly once without editing any existing
     * statement. esp_timer_get_time() is microseconds. */
    struct DrainTimer {
        uint64_t t0 = (uint64_t)esp_timer_get_time();
        ~DrainTimer() {
            uint32_t dt = (uint32_t)((uint64_t)esp_timer_get_time() - t0);
            if (dt > g_drain_us_max) g_drain_us_max = dt;
        }
    } drain_timer;

    uint32_t started = millis();
    size_t bytes = 0;
    int frames = 0;

    /* Reported for the previous pass rather than at the point of rejection, so
     * a link_down() partway through this one cannot swallow it.
     *
     * Nothing this stack generates can exceed _HW_MTU today: Type::Reticulum::
     * MTU is 500, and neither AUTOCONFIGURE_MTU nor FIXED_MTU is set on this
     * interface, so Transport applies no link-MTU upgrade to it. But
     * Link::validate_request() takes a peer's signalled MTU without clamping it
     * to the receiving interface's HW_MTU, so the condition is not structurally
     * impossible — and a dropped frame is otherwise visible only as a counter
     * nobody is watching. Say it out loud, once.
     *
     * This node now owns a destination, which used to be the condition on a TODO
     * here to clamp an inbound link MTU ourselves. No code is needed: the
     * library already clamps it. Transport::inbound() computes nh_mtu from the
     * receiving interface only when it sets AUTOCONFIGURE_MTU or FIXED_MTU, and
     * RnsTcpInterface sets neither — so nh_mtu is Type::Reticulum::MTU, 500,
     * below our HW_MTU of 1024, and a link established through this interface
     * cannot negotiate its way past the frame buffer. The destination declines
     * link requests outright besides (accepts_links(false) at bring-up). */
    if (g_rns_rx.rejected_long != 0 && !g_rns_mtu_warned) {
        g_rns_mtu_warned = true;
        event_add("rns frame over mtu");
    }

    /* _online is re-tested every pass: handle_incoming() below can re-enter the
     * stack and come back out through link_down(). */
    while (_online &&
           bytes < RNS_TCP_DRAIN_BYTES_MAX &&
           frames < RNS_TCP_DRAIN_FRAMES_MAX &&
           (millis() - started) < RNS_TCP_DRAIN_BUDGET_MS) {
        int avail = g_rns_client.available();
        if (avail <= 0) break;

        size_t want = (size_t)avail;
        if (want > RNS_TCP_READ_CHUNK) want = RNS_TCP_READ_CHUNK;
        int n = g_rns_client.read(g_rns_rdbuf, want);
        if (n <= 0) break;
        bytes += (size_t)n;

        /* The chunk is parsed to the end regardless of the frame budget: the
         * decoder keeps reassembly state, but bytes we hand back have nowhere
         * to be stored. */
        size_t off = 0;
        while (off < (size_t)n) {
            size_t used = 0;
            int got = rns_hdlc_rx_feed(&g_rns_rx, g_rns_rdbuf + off,
                                       (size_t)n - off, &used);
            off += used;
            if (!got) break;
            frames++;
            g_rns_frames_in++;
            try {
                handle_incoming(RNS::Bytes(g_rns_rx.buf, g_rns_rx.len));
            } catch (const std::bad_alloc &) {
                /* Genuine OOM: let it reach Reticulum::loop(), which restarts
                 * the board. Swallowing it here would only hide the cause. */
                throw;
            } catch (const std::exception &) {
                g_rns_rx_errors++;
            } catch (...) {
                g_rns_rx_errors++;
            }
            /* handle_incoming() re-enters the stack, and the stack can come
             * back out through our send_outgoing() -> tx_flush() -> link_down():
             * the client is stopped, the decoder is reset and the backoff is
             * armed. Feeding the remainder of this chunk into that reset decoder
             * would hand Transport frames from an interface it has just been
             * told is down, so the drain stops here and the next tick starts
             * from the reconnect. */
            if (!_online) return;
        }
    }
}

/* Called by Transport on the loop task. Never blocks, never allocates, and
 * refuses rather than corrupting the stream when a tail is still owed. */
bool RnsTcpInterface::send_outgoing(const RNS::Bytes &data) {
    if (g_rns_cs.load() != RNS_CS_CONNECTED || !_online) {
        g_rns_tx_dropped++;
        return false;
    }
    if (data.size() == 0 || data.size() > (size_t)_HW_MTU) {
        g_rns_tx_dropped++;
        return false;
    }

    /* One frame of TX depth. If the previous frame is still half on the wire,
     * give it one more push and then drop this one — HDLC has no way to
     * interleave, and dropping a packet is what a Reticulum interface is
     * allowed to do. */
    if (g_rns_tx_sent < g_rns_tx_len) {
        tx_flush();
        /* tx_flush() can take the link down on a write error, which clears the
         * tail — re-read the baton rather than reading "tail gone" as "ready". */
        if (g_rns_tx_sent < g_rns_tx_len ||
            g_rns_cs.load() != RNS_CS_CONNECTED) {
            g_rns_tx_dropped++;
            return false;
        }
    }

    size_t n = rns_hdlc_encode(data.data(), data.size(), g_rns_txbuf,
                               sizeof(g_rns_txbuf));
    if (n == 0) {
        g_rns_tx_dropped++;
        return false;
    }
    g_rns_tx_len = n;
    g_rns_tx_sent = 0;
    g_rns_tx_pending_ms = 0;

    tx_flush();

    /* The flush comes first because it can fail: a first lwip_send() that
     * errors, or an fd that has already vanished, takes the link down and
     * clears the tail. Returning true there would be a lie with consequences —
     * Transport::transmit() would report sent = true and Transport::outbound()
     * would go on to call interface.sent_announce() for an announce that never
     * left the board. So the answer is the link state after the attempt, and
     * the base class's counters (ours to maintain; nothing else touches them)
     * are only credited when that answer is yes. A short write is still a yes:
     * the tail is parked and the socket is up, so the frame is committed. */
    bool sent = (g_rns_cs.load() == RNS_CS_CONNECTED) && _online;
    if (sent) handle_outgoing(data);
    return sent;
}

/* Driven by Reticulum::loop() from the skill tick, i.e. always the loop task.
 * Every branch returns promptly; the only work with any duration is drain(),
 * which carries its own budget. */
void RnsTcpInterface::loop() {
    int cs = g_rns_cs.load();

    /* The connect task owns the client. Nothing else is safe to do. */
    if (cs == RNS_CS_CONNECTING) {
        _online = false;
        return;
    }

    /* Config edits land here, never in the HTTP handler. */
    if (g_rns_cfg_dirty) {
        g_rns_cfg_dirty = false;
        bool changed = rns_cfg_load();
        /* An operator who has just pointed the interface at a working host
         * should not sit out a backoff that grew to a minute against the
         * previous one. Any POST that leaves a usable endpoint behind rearms
         * the ladder at its minimum and clears the wait. */
        if (g_rns_cfg_ok) {
            g_rns_backoff_ms = RNS_TCP_BACKOFF_MIN_MS;
            g_rns_next_try_ms = millis();
        }
        if (changed && cs == RNS_CS_CONNECTED) {
            link_down("configuration changed");
            return;
        }
    }

    if (cs == RNS_CS_FAILED) {
        _online = false;
        g_rns_client.stop();
        g_rns_cs.store(RNS_CS_IDLE);
        g_rns_next_try_ms = millis() + g_rns_backoff_ms;
        /* Announce the delay that was just armed, not the one after it: the
         * event used to print the already-doubled value and was a whole step
         * ahead of the timer an operator was actually waiting on. */
        event_add("rns tcp connect failed, retry in %lus",
                  (unsigned long)(g_rns_backoff_ms / 1000UL));
        if (g_rns_backoff_ms < RNS_TCP_BACKOFF_MAX_MS)
            g_rns_backoff_ms *= 2;
        if (g_rns_backoff_ms > RNS_TCP_BACKOFF_MAX_MS)
            g_rns_backoff_ms = RNS_TCP_BACKOFF_MAX_MS;
        return;
    }

    if (cs == RNS_CS_CONNECTED) {
        if (!_online) {
            /* First tick after the connect task won. */
            _online = true;
            g_rns_up_ms = millis();
            g_rns_backoff_ms = RNS_TCP_BACKOFF_MIN_MS;
            g_rns_wg_seen = wg_is_up();
            rns_hdlc_rx_reset(&g_rns_rx);
            g_rns_tx_len = 0;
            g_rns_tx_sent = 0;
            g_rns_tx_pending_ms = 0;
            event_add("rns tcp up %s:%u", g_rns_host, (unsigned)g_rns_port);
        }

        if (!g_rns_cfg_enabled) {
            link_down("disabled");
            return;
        }
        if (WiFi.status() != WL_CONNECTED) {
            link_down("wifi lost");
            return;
        }
        /* A tunnel restart pulls the netif out from under an open socket
         * without any error surfacing on it. The transition is the signal. */
        if (wg_is_up() != g_rns_wg_seen) {
            link_down("wireguard state changed");
            return;
        }
        if (!g_rns_client.connected()) {
            link_down("peer closed the socket");
            return;
        }

        tx_flush();
        if (g_rns_tx_pending_ms != 0 &&
            (millis() - g_rns_tx_pending_ms) > RNS_TCP_TX_STALL_MS) {
            link_down("outbound frame stalled");
            return;
        }
        drain();
        return;
    }

    /* RNS_CS_IDLE — the cheap path, and the one taken on every tick when the
     * interface is switched off. */
    _online = false;
    if (!g_rns_cfg_enabled || !g_rns_cfg_ok) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if ((int32_t)(millis() - g_rns_next_try_ms) < 0) return;
    begin_connect();
}

static const char *rns_link_state_name() {
    if (!g_rns_cfg_ok || !g_rns_cfg_enabled) return "off";
    int cs = g_rns_cs.load();
    if (cs == RNS_CS_CONNECTING) return "connecting";
    if (cs == RNS_CS_CONNECTED) return "connected";
    /* RNS_CS_FAILED lives for a single tick before the poll converts it into
     * an armed backoff, so what an operator would actually catch here is
     * RNS_CS_IDLE. Report the honest thing: idle before the first attempt,
     * failed once something has gone wrong and we are waiting to retry. */
    if (g_rns_last_error[0]) return "failed";
    return "idle";
}

/* Publish everything GET /rns/status must not read live. Loop task only, once
 * per tick — see the RnsStatusSnap comment for why each of these cannot be
 * touched from the AsyncTCP task. The two string copies deliberately stop one
 * byte short of their destination, leaving the terminator this file wrote at
 * startup permanently in place. */
static void rns_status_publish() {
    g_rns_snap.state = rns_link_state_name();
    g_rns_snap.interfaces = (uint32_t)RNS::Transport::get_interfaces().size();
    g_rns_snap.paths = (uint32_t)RNS::Transport::new_path_table().size();

    bool up = (g_rns_cs.load() == RNS_CS_CONNECTED);
    g_rns_snap.online = up && g_rns_iface && g_rns_iface.online();
    g_rns_snap.up_age_s =
        (up && g_rns_up_ms) ? (uint32_t)((millis() - g_rns_up_ms) / 1000UL) : 0UL;

    if (g_rns_iface) {
        g_rns_snap.rx = (uint32_t)g_rns_iface.rx();
        g_rns_snap.tx = (uint32_t)g_rns_iface.tx();
        g_rns_snap.rxbytes = (uint32_t)g_rns_iface.rxbytes();
        g_rns_snap.txbytes = (uint32_t)g_rns_iface.txbytes();
    }

    /* Prefilter counters, published like everything else the handler reads. */
    g_rns_snap.ann_dropped = g_rns_ann_dropped;
    g_rns_snap.ann_kept = g_rns_ann_kept;

    /* Outbound queue: how many messages are waiting, and why the last one that
     * did not make it did not make it. Both published here for the same reason
     * as everything above — the depth reads two atomics the AsyncTCP task also
     * writes, and the reason pointer is reassigned by this task. */
    g_rns_snap.send_queued = rns_outbox_depth(&g_rns_outbox);
    /* UNDER THE PRODUCER LOCK, and moving the read to this task is not what
     * makes it safe — that would only swap which task was racing which. The
     * counter is incremented inside rns_outbox_put(), which is inside the
     * spinlock, so the lock is the only thing that makes this read ordered
     * against it. It is one aligned uint32_t and the section is a load. */
    portENTER_CRITICAL(&g_rns_tx_mux);
    g_rns_snap.send_refused = g_rns_outbox.refused;
    portEXIT_CRITICAL(&g_rns_tx_mux);
    memcpy(g_rns_snap.send_error, g_rns_send_error,
           sizeof(g_rns_snap.send_error) - 1);

    /* The inbound envelope: how many payloads were read as one, how many were
     * shown raw and why, and what a room router did with them. All written by
     * rns_inbox_poll() a few lines earlier in the same tick. The address copy
     * stops one byte short for the reason send_error's does — that last byte is
     * never written after the initialiser, so a reader crossing this memcpy can
     * see mixed characters but never an unterminated array. */
    g_rns_snap.env_parsed = g_rns_env_parsed;
    g_rns_snap.env_control = g_rns_env_control;
    g_rns_snap.env_raw = g_rns_env_raw;
    g_rns_snap.env_routed = g_rns_env_routed;
    g_rns_snap.env_refused = g_rns_env_refused;
    g_rns_snap.env_reason = g_rns_env_refuse_reason;
    g_rns_snap.env_raw_why = g_rns_env_raw_why;
    g_rns_snap.env_bad_why = g_rns_env_bad_why;
    memcpy(g_rns_snap.env_from, g_rns_env_from,
           sizeof(g_rns_snap.env_from) - 1);

    g_rns_snap.port = g_rns_port;
    memcpy(g_rns_snap.host, g_rns_host, sizeof(g_rns_snap.host) - 1);
    memcpy(g_rns_snap.peer, g_rns_peer, sizeof(g_rns_snap.peer) - 1);
    memcpy(g_rns_snap.last_error, g_rns_last_error,
           sizeof(g_rns_snap.last_error) - 1);

    /* Loop-task health, sampled here so GET /rns/status never touches the loop
     * task's live structures. See the diagnostics block above for the units. */
    g_loop_stack_free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);

    /* Snapshot up to RNS_PATH_HASH_MAX destination hashes from the path table.
     * new_path_table() hands back a const reference, but its begin()/end() are
     * non-const (the iterator lazily decodes each record), so a const_cast is
     * needed to walk it — this is the same container the library itself iterates
     * as `for (const auto& path : _new_path_table)`. Loop task only, so no
     * Transport::inbound() insert runs concurrently. The full count stays in
     * g_rns_snap.paths (read just above). A decode fault must not take the tick
     * down, so the walk is guarded. */
    uint8_t pn = 0;
    try {
        auto &pt = const_cast<RNS::Persistence::NewPathTable &>(
            RNS::Transport::new_path_table());
        for (const auto &path : pt) {
            if (pn >= RNS_PATH_HASH_MAX) break;
            std::string hx = path.key.toHex();
            snprintf(g_path_hash[pn], sizeof(g_path_hash[pn]), "%s", hx.c_str());
            pn++;
        }
    } catch (...) {
        /* report whatever was gathered before the fault */
    }
    g_path_hash_n = pn;
}

/* ---- identity ---- */

/* Bytes::assignHex() validates NOTHING: its digit arithmetic maps every input
 * byte to some value and the only length handling is a truncation to an even
 * count, so a corrupted ASCII byte would decode to a different key instead of
 * failing. Every character is checked before it reaches the decoder. */
static bool rns_is_hex(const String &s) {
    for (size_t i = 0; i < s.length(); i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

/* Load /rns_identity.id, or mint one when the file does not exist. The branch
 * is decided by SPIFFS.exists(), never by content: read_spiffs_file() returns
 * "" both for an absent file and for a failed open/read, so deciding on the
 * string would regenerate — and overwrite — a key we merely failed to read. */
static void rns_identity_bring_up() {
    if (SPIFFS.exists(RNS_ID_PATH)) {
        String hex = read_spiffs_file(RNS_ID_PATH);
        hex.trim();
        if (hex.length() == 0) {
            rns_error = "stored identity is present but unreadable";
            return;
        }
        if (hex.length() != RNS_PRV_KEY_BYTES * 2) {
            rns_error = "stored identity has the wrong length";
            return;
        }
        if (!rns_is_hex(hex)) {
            rns_error = "stored identity is not hex";
            return;
        }
        RNS::Bytes prv;
        prv.assignHex(hex.c_str());
        if (prv.size() != RNS_PRV_KEY_BYTES) {
            rns_error = "stored identity failed to decode";
            return;
        }
        RNS::Identity loaded(false);      /* false: do not generate keys */
        if (!loaded.load_private_key(prv)) {
            rns_error = "stored identity rejected by the stack";
            return;
        }
        rns_identity = loaded;
    } else {
        RNS::Identity fresh;              /* default ctor generates a keypair */
        String out = fresh.get_private_key().toHex().c_str();
        if (!write_spiffs_file_atomic(RNS_ID_PATH, RNS_ID_TMP, out)) {
            /* The key is live in RAM but will not survive a reboot; say so
             * rather than pretending the identity is persistent. */
            rns_error = "identity generated but could not be saved";
        }
        rns_identity = fresh;
        rns_identity_created = true;
    }

    rns_identity_ok = true;
    strncpy(rns_hexhash, rns_identity.hexhash().c_str(), sizeof(rns_hexhash) - 1);
    rns_hexhash[sizeof(rns_hexhash) - 1] = '\0';
}

/* ---- HTTP ---- */

static void rns_status_json(JsonDocument &doc) {
    doc["ready"] = rns_started;
    doc["identity"] = rns_identity_ok;
    if (rns_identity_ok) {
        doc["hash"] = rns_hexhash;
        doc["created"] = rns_identity_created;
    } else {
        doc["hash"] = (const char *)nullptr;
    }

    /* THE ADDRESS. `hash` above is the IDENTITY hash and this is the
     * DESTINATION hash; they are different values and neither substitutes for
     * the other. This one is Destination::hash().toHex() — 32 lowercase hex
     * characters, no separators — which is exactly the form `rnpath` and
     * `rnprobe` take on the command line. It is null until the address string
     * has actually been rendered, which needs the destination, which needs the
     * identity — so a node reporting identity:false reports no address either.
     * Note that it is keyed on the STRING and not on rns_dest_ok: those two can
     * differ for exactly one reason (a throw out of toHex(), see the bring-up
     * block), and in that case the honest report is a live destination with an
     * address we failed to render, not a missing destination.
     *
     * announced/announces_sent/announce_us_* are the announce schedule made
     * visible: `announced` is "we have attempted at least one announce", which
     * is deliberately not the same as announces_sent — a send that threw still
     * stamps the schedule so it cannot retry every 20 ms (see
     * rns_announce_poll). announce_us_last and announce_us_max are the
     * measurement this destination was added to take: Destination::announce()
     * signs with software Ed25519 on the loop task, inside the tick that owns
     * the 8 ms drain, and nothing on this board had measured the sign side.
     * Read announce_us_max against iface.drain_us_max.
     *
     * THE FIRST SIX data_* KEYS ARE THE RECEIVE PATH MADE DIAGNOSABLE, and
     * each one answers a question the others cannot (the envelope's own keys
     * follow them, with their own note):
     *   data_rx          payloads the packet callback was handed. Still counts
     *                    packets that DECRYPTED, not packets that arrived — see
     *                    the blind spot above.
     *   data_dropped     messages refused because one was still waiting for the
     *                    loop task. The overflow policy in rns/inbox.h is keep
     *                    the first, count the drop; this is that count, and it
     *                    is the difference between a message that was never sent
     *                    and a message this node threw away.
     *   data_last_len    length in bytes of the most recent payload OFFERED,
     *                    accepted or dropped. It is what a sender compares
     *                    against what it sent.
     *   data_oversize    payloads longer than the 383-byte ceiling, stored short.
     *                    Structurally impossible through a single packet; a
     *                    non-zero value means an assumption in this file is
     *                    wrong, which is why it is published rather than
     *                    asserted.
     *   data_cards       payloads that became a notification card.
     *   data_card_cut    cards whose body could not hold the whole message.
     *
     * All of these are naturally aligned scalars written by the loop task and
     * read here exactly like iface.attempts — 32-bit but for data_last_len,
     * which is a 16-bit aligned load and single-copy atomic on this core just
     * the same. rns_dest_addr is written once during bring-up. */
    doc["address"] = rns_dest_addr[0] ? rns_dest_addr : (const char *)nullptr;
    doc["announced"] = g_rns_announced;
    doc["announces_sent"] = (unsigned long)g_rns_ann_sent;
    doc["announce_us_last"] = (unsigned long)g_rns_ann_us_last;
    doc["announce_us_max"] = (unsigned long)g_rns_ann_us_max;
    doc["data_rx"] = (unsigned long)g_rns_inbox.received;
    doc["data_dropped"] = (unsigned long)g_rns_inbox.dropped;
    doc["data_last_len"] = (unsigned long)g_rns_inbox.last_len;
    doc["data_oversize"] = (unsigned long)g_rns_inbox.oversize;
    doc["data_cards"] = (unsigned long)g_rns_cards;
    doc["data_card_cut"] = (unsigned long)g_rns_card_cut;

    /* The lxmf.delivery destination, published beside seed.pager's `address`.
     *   lxmf_address   its 32-hex destination hash, or null if bring-up failed.
     *   data_lxmf_rx   packets the lxmf callback was handed (inbox.received).
     *   data_lxmf_ok   ...that then parsed as an LXMF message.
     *   data_lxmf_bad  ...that reached the destination and did NOT parse — the
     *                  "device alive but did not understand" signal, disjoint
     *                  from the three seed.pager outcomes above. Direct reads,
     *                  like data_cards: loop-task-written scalars, single-copy
     *                  atomic to read here. */
    doc["lxmf_address"] = rns_lxmf_dest_addr[0] ? rns_lxmf_dest_addr
                                                : (const char *)nullptr;
    doc["data_lxmf_rx"] = (unsigned long)g_rns_lxmf_inbox.received;
    doc["data_lxmf_ok"] = (unsigned long)g_rns_lxmf_ok;
    doc["data_lxmf_bad"] = (unsigned long)g_rns_lxmf_bad;
    /* Where the parsed messages went: cards + rooms partition data_lxmf_ok, so a
     * shortfall against it is a message that parsed but whose card was refused. */
    doc["data_lxmf_cards"] = (unsigned long)g_rns_lxmf_cards;
    doc["data_lxmf_rooms"] = (unsigned long)g_rns_lxmf_rooms;
    /* The OUT mirror of data_lxmf_rooms (C4): LXMF replies built, signed and
     * handed to the send ladder. Disjoint from every other counter. */
    doc["data_lxmf_tx"] = (unsigned long)g_rns_lxmf_tx;

    /* THE ENVELOPE MADE VISIBLE, and the pair at the top of it is the point:
     * a payload is either read as `1|<address>|<session>|<text>` or shown
     * exactly as it arrived, and both are ordinary. Which one is happening
     * decides what the screen looks like, so it cannot be left to be guessed
     * from a card.
     *   data_envelopes   payloads taken apart as a version-1 envelope and shown
     *                    as a message. Control frames are NOT in here.
     *   data_control     envelopes whose session was the reserved single byte
     *                    '*': the peer saying which rooms are live rather than
     *                    sending a message. They raise NO CARD — a room list is
     *                    not something to put on the screen — so this counter is
     *                    the only place they appear, and it is what tells "the
     *                    node is talking to us and we ignore it" apart from
     *                    "nothing arrives". This device never SENDS one:
     *                    rns_envelope_build() refuses the reserved session, on
     *                    purpose (see rns/outbox.h).
     *   data_raw         payloads shown as they came. tools/rns-send sends bare
     *                    text, so this counting up is not a fault on its own.
     *   data_raw_why     the last reason one was not an envelope — an unknown
     *                    version, a bad address, a bad session name, no framing
     *                    at all. Null until it has happened.
     *   data_malformed_why  the same, but ignoring "plain text". Bare-text
     *                    senders are ordinary and constant, so they would
     *                    otherwise keep the field above pinned to their own
     *                    reason and hide the single malformed probe it exists
     *                    to show.
     *   data_from        sender address of the last envelope, 32 hex
     *                    characters: what POST /rns/send takes as `to`. A
     *                    packet to a SINGLE destination carries no source, so
     *                    the envelope is the only place an answer can come
     *                    from, and this is that field kept where something
     *                    replying can read it. It is NOT on the card.
     *                    ⚠ IT IS ALSO UNAUTHENTICATED — no source field, no
     *                    signature, and the parser proves only that the
     *                    characters are hex. Anyone who has heard this node's
     *                    announce can put SOMEBODY ELSE'S address in it, so a
     *                    reply sent here on trust is a packet encrypted to a
     *                    node the user never talked to. A hint to confirm, not
     *                    a provenance; see g_rns_env_from for the whole note.
     *   data_routed      envelopes a room router accepted.
     *   data_route_refused  envelopes it refused. The card still went up, so
     *                    this counter is the only place a message that never
     *                    reached its conversation shows at all.
     *   data_route_reason   the router's own code from that last refusal,
     *                    carried and not interpreted. Zero until then.
     * The five counters and both strings come from the loop-side snapshot, for
     * the reason the whole snapshot exists. */
    doc["data_envelopes"] = (unsigned long)g_rns_snap.env_parsed;
    doc["data_control"] = (unsigned long)g_rns_snap.env_control;
    doc["data_raw"] = (unsigned long)g_rns_snap.env_raw;
    doc["data_raw_why"] = g_rns_snap.env_raw_why;
    doc["data_malformed_why"] = g_rns_snap.env_bad_why;
    doc["data_from"] = g_rns_snap.env_from[0] ? g_rns_snap.env_from
                                              : (const char *)nullptr;
    doc["data_routed"] = (unsigned long)g_rns_snap.env_routed;
    doc["data_route_refused"] = (unsigned long)g_rns_snap.env_refused;
    doc["data_route_reason"] = (long)g_rns_snap.env_reason;

    /* THE SEND PATH MADE VISIBLE, and this is the reason the keys exist rather
     * than a habit of publishing counters. A send to a peer this node cannot
     * resolve DOES NOT FAIL: Transport::outbound() takes the broadcast branch
     * with no path in the table, the packet is emitted as HEADER_1, the hub
     * drops it, and receipt_send() still hands back a receipt because an
     * interface did accept it. Nothing anywhere says the message went nowhere.
     * These six keys are what converts that silence into something a user can
     * read:
     *   send_queued      messages waiting for the loop task. Non-zero for a
     *                    minute at a time is normal while a path is resolved.
     *   send_refused     POSTs the ring had no room for. The caller was told
     *                    synchronously as well, so this is a rate signal.
     *   sent             packets an interface accepted. NOT deliveries — a
     *                    receipt proves only that much (see rns_send_poll).
     *   send_failed      messages given up on after the whole retry ladder.
     *   send_error       which destination was given up on and why, as a
     *                    sentence — including the text of an exception when one
     *                    was thrown, because on a board with nothing attached
     *                    the console is not a place a reason can go.
     *   send_us_last/max microseconds a SUCCESSFUL send spent. Sending does not
     *                    sign — no Ed25519 — but it DOES encrypt, and this port
     *                    caches nothing between packets: every send generates a
     *                    fresh ephemeral X25519 keypair, runs the exchange,
     *                    derives with HKDF and builds a token. Read these
     *                    against announce_us_max and iface.drain_us_max.
     *                    ONLY successful sends are timed, and that is not
     *                    tidiness: when Transport::outbound() returns false,
     *                    Packet::receipt_send() emits ERROR("No interfaces
     *                    could process the outbound packet") and Log.cpp ends
     *                    every emitted line with a blocking Serial.flush() —
     *                    inside the window. Timing the failure would publish a
     *                    115200-baud console flush as the cost of encryption.
     * send_queued and send_error come from the loop-side snapshot; sent,
     * send_failed and the two timings are naturally aligned scalars written by
     * the loop task, exactly like announce_us_last. send_refused USED to be
     * read at its source here, on the argument that this task was the only one
     * that ever wrote it; that stopped being true when a line typed on the
     * keyboard became a send, so it is now sampled on the loop task UNDER THE
     * PRODUCER SPINLOCK — the snapshot alone would only have moved the race.
     *
     *   peer             the node a message typed on this device is addressed
     *                    to — /rns.json's `peer`, 32 hex characters, null when
     *                    none is set. It is the FIRST thing to look at when the
     *                    chat room says it cannot send: without it the room has
     *                    nowhere to put a message and says so instead of
     *                    guessing a destination. Set it with POST /rns/config. */
    doc["send_queued"] = (unsigned long)g_rns_snap.send_queued;
    doc["send_refused"] = (unsigned long)g_rns_snap.send_refused;
    doc["peer"] = g_rns_snap.peer[0] ? g_rns_snap.peer : (const char *)nullptr;
    doc["sent"] = (unsigned long)g_rns_sent;
    doc["send_failed"] = (unsigned long)g_rns_send_failed;
    doc["send_error"] = g_rns_snap.send_error[0] ? g_rns_snap.send_error
                                                 : (const char *)nullptr;
    doc["send_us_last"] = (unsigned long)g_rns_send_us_last;
    doc["send_us_max"] = (unsigned long)g_rns_send_us_max;

    doc["transport"] = RNS::Reticulum::transport_enabled();
    doc["interfaces"] = (unsigned long)g_rns_snap.interfaces;
    /* The number that proves the peer's announces landed: it stays 0 until a
     * real RNS node is on the other end of the socket. It is an std::map size
     * that Transport::inbound() inserts into on the loop task, so it is read
     * from the snapshot and never from the table itself. */
    doc["paths"] = (unsigned long)g_rns_snap.paths;

    /* The destination hashes currently in the path table, so phase-2 can
     * confirm the learned path is to a known announcer. These come from the
     * loop-side snapshot g_path_hash (filled in rns_status_publish) and are
     * NEVER iterated live from this AsyncTCP handler — a concurrent
     * Transport::inbound() insert would invalidate the iterator. The array is
     * bounded to RNS_PATH_HASH_MAX; path_hashes_total is the full count (the
     * same number as `paths`), so a reader can tell a truncated array from a
     * complete one. */
    JsonArray phash = doc["path_hashes"].to<JsonArray>();
    uint8_t phn = g_path_hash_n;
    if (phn > RNS_PATH_HASH_MAX) phn = RNS_PATH_HASH_MAX;
    for (uint8_t i = 0; i < phn; i++) phash.add((const char *)g_path_hash[i]);
    doc["path_hashes_total"] = (unsigned long)g_rns_snap.paths;

    if (rns_error) doc["error"] = rns_error;

    JsonObject iface = doc["iface"].to<JsonObject>();
    iface["name"] = RNS_TCP_IFACE_NAME;
    iface["state"] = g_rns_snap.state;
    iface["configured"] = g_rns_cfg_ok;
    iface["enabled"] = g_rns_cfg_enabled;
    iface["host"] = g_rns_snap.host;
    iface["port"] = g_rns_snap.port;
    iface["hw_mtu"] = RNS_TCP_HW_MTU;
    iface["bitrate"] = (unsigned long)RNS_TCP_BITRATE;
    iface["attempts"] = g_rns_attempts;
    iface["drops"] = g_rns_downs;
    iface["backoff_ms"] = g_rns_backoff_ms;
    if (g_rns_snap.last_error[0]) iface["last_error"] = g_rns_snap.last_error;

    iface["online"] = g_rns_snap.online;
    iface["up_age_s"] = (unsigned long)g_rns_snap.up_age_s;

    /* Base-class counters (packets handed to and taken from Transport) next to
     * ours (wire frames and what never made it), so a mismatch points at the
     * framing rather than at the stack. */
    iface["rx"] = (unsigned long)g_rns_snap.rx;
    iface["tx"] = (unsigned long)g_rns_snap.tx;
    iface["rxbytes"] = (unsigned long)g_rns_snap.rxbytes;
    iface["txbytes"] = (unsigned long)g_rns_snap.txbytes;
    iface["frames_in"] = g_rns_frames_in;
    iface["tx_dropped"] = g_rns_tx_dropped;
    iface["rx_errors"] = g_rns_rx_errors;
    iface["frames_short"] = g_rns_rx.rejected_short;
    iface["frames_long"] = g_rns_rx.rejected_long;

    /* Loop-task health sampled in the RNS tick (rns_status_publish).
     * loop_stack_free_bytes is uxTaskGetStackHighWaterMark(NULL) — the
     * minimum-free-ever stack of the loop task, in BYTES on this ESP-IDF build
     * (see the diagnostics block for the header quote). drain_us_max is the
     * rolling max microseconds one drain() pass has taken; GET
     * /rns/status?reset=1 zeroes it after this read. */
    iface["loop_stack_free_bytes"] = (unsigned long)g_loop_stack_free_bytes;
    iface["drain_us_max"] = (unsigned long)g_drain_us_max;

    /* What the inbound prefilter did, and the only evidence that it fires at
     * all: announces_dropped is announces refused before Ed25519, announces_kept
     * is the PATH_RESPONSE ones that went through and paid for the verification.
     * Both come from the loop-side snapshot, never from the live counters.
     * Watch these against drain_us_max: dropped climbing with drain_us_max flat
     * near the 8 ms budget is the whole result. */
    iface["announces_dropped"] = (unsigned long)g_rns_snap.ann_dropped;
    iface["announces_kept"] = (unsigned long)g_rns_snap.ann_kept;

    /* The library's own counters next to the platform's, so /rns/status can be
     * diffed against /capabilities when the stack starts carrying traffic.
     * alloc_count/free_count read 0 under RNS_HEAP_ALLOCATOR — the library only
     * bumps them from the global operator new it declines to override here. */
    JsonObject mem = doc["mem"].to<JsonObject>();
    mem["rns_heap_size"] = RNS::Utilities::Memory::heap_size();
    mem["rns_heap_available"] = RNS::Utilities::Memory::heap_available();
    mem["alloc_count"] = RNS::Utilities::Memory::default_allocator_alloc();
    mem["free_count"] = RNS::Utilities::Memory::default_allocator_free();
    mem["free_heap"] = ESP.getFreeHeap();
    mem["free_psram"] = ESP.getFreePsram();
}

static const SkillEndpoint rns_endpoints[] = {
    {"GET", "/rns/status", "Reticulum stack, identity, destination address, TCP interface and counters"},
    {"POST", "/rns/config", "Set TCP interface {host, port, enabled} and the chat peer {peer}"},
    {"POST", "/rns/send", "Send one text to a peer: {to, session, text}"},
    {NULL, NULL, NULL}
};

static const char *rns_describe() {
    return "## Skill: rns\n\n"
           "Reticulum (microReticulum 0.5.0) running alongside MeshCore, with\n"
           "one interface: a TCP client that dials an RNS node over WiFi or\n"
           "the WireGuard tunnel. The SX1262 is still never touched — MeshCore\n"
           "owns the radio.\n"
           "The node identity (X25519 + Ed25519) lives in SPIFFS at\n"
           "`/rns_identity.id` as hex and is generated once on first boot.\n"
           "The endpoint lives in SPIFFS at `/rns.json` (not in git) as\n"
           "`{enabled, host, port}`, port defaulting to 4242.\n"
           "Framing is HDLC, matching what a Python `TCPServerInterface`\n"
           "speaks to a spawned client; there is no handshake or banner.\n"
           "The dial runs on a one-shot task so the loop never waits on it,\n"
           "reconnects back off from 3 s to 60 s, and the link is dropped and\n"
           "redialled when WiFi drops, when the peer closes, or when the\n"
           "WireGuard tunnel is restarted underneath the socket.\n"
           "`GET /rns/status` reports the stack, `iface.state`\n"
           "(off/idle/connecting/connected/failed), the endpoint, rx/tx\n"
           "counters, the last error and `paths` — the path-table size, which\n"
           "is what shows the peer's announces arriving.\n"
           "It also reports loop-task health sampled in the RNS tick:\n"
           "`iface.loop_stack_free_bytes` (the loop task's minimum-free stack in\n"
           "bytes), `iface.drain_us_max` (the longest single drain pass in\n"
           "microseconds, zeroed by `?reset=1`) and `path_hashes` — up to eight\n"
           "path-table destination hashes with `path_hashes_total` for the full\n"
           "count.\n"
           "The node is addressable: it owns one IN/SINGLE destination,\n"
           "`seed.pager`, built on the stored identity. `address` in\n"
           "`/rns/status` is its 32-hex-character destination hash — the\n"
           "argument `rnpath` takes — and is a DIFFERENT value from `hash`,\n"
           "which is the identity hash. It announces about 5 s after the\n"
           "interface comes online, then every 30 min while it stays online,\n"
           "and again on a reconnect but never more than once a minute — the\n"
           "TCP backoff resets on every successful connect, so a flapping\n"
           "peer would otherwise buy a signature every 3 s. It never\n"
           "announces while offline.\n"
           "`announce_us_last` and `announce_us_max` time each\n"
           "announce, which is synchronous and signs with software Ed25519 on\n"
           "the loop task. The destination declines link requests and keeps\n"
           "the default PROVE_NONE proof strategy — nothing here terminates a\n"
           "link yet, and both would run public-key work inside the drain.\n"
           "A packet addressed to it becomes a notification card. The callback\n"
           "runs inside the socket drain, where raising a card is not allowed,\n"
           "so it copies the payload into an eight-slot ring and the loop task\n"
           "drains that ring into cards after the stack has run. The ring is one\n"
           "whole drain pass deep, so a burst of eight frames is eight cards; a\n"
           "message arriving with every slot full is refused, not overwritten,\n"
           "and counted. Nothing rate-limits cards. The payload is at most 383\n"
           "bytes (the largest plaintext one encrypted packet carries), is not\n"
           "NUL-terminated, and may be any bytes at all: control bytes are\n"
           "rendered as `.`, malformed UTF-8 as `?`, and a message longer than\n"
           "the ~185 characters the card actually paints is cut and prefixed\n"
           "with the count of what did not fit. A\n"
           "zero-length message never arrives at all — the library skips the\n"
           "callback on empty plaintext, so the sender sees success and the\n"
           "device sees nothing. `data_rx`, `data_dropped`, `data_last_len`,\n"
           "`data_oversize`, `data_cards` and `data_card_cut` in\n"
           "`/rns/status` are the receive path's counters. The card cannot\n"
           "name the sender: a packet to a SINGLE destination carries no\n"
           "source, so anyone who knows the address can raise one.\n"
           "AN INBOUND PAYLOAD IS READ AS THE SAME ENVELOPE THIS NODE EMITS,\n"
           "by the same code in `rns/outbox.h` — one owner for the format, so\n"
           "the two halves cannot drift. A payload that parses contributes\n"
           "only its TEXT to the card; the session name goes in the title\n"
           "next to the byte count, and the sender address is kept for\n"
           "replying rather than shown. PARSING IS SOFT: anything that is not\n"
           "an envelope — bare text, an unknown version digit, a bad address\n"
           "or session name, a frame with no text — is shown exactly as it\n"
           "arrived and counted separately, because senders that predate the\n"
           "format exist and a message is never worth dropping over its\n"
           "shape. `data_envelopes`, `data_raw`, `data_raw_why`,\n"
           "`data_malformed_why` (the last reason that was not simply plain\n"
           "text, which bare-text senders would otherwise mask) and\n"
           "`data_from` report that split; `data_from` is the 32-hex address\n"
           "of the last envelope and is what `POST /rns/send` takes as `to`.\n"
           "IT IS NOT PROOF OF ANYTHING. The packet carries no source field\n"
           "and no signature, so the address is whatever the sender typed:\n"
           "anyone who has heard this node's announce can name a THIRD PARTY\n"
           "there, and a reply sent on trust is encrypted to a node the user\n"
           "never talked to. Confirm it before answering.\n"
           "ONE SESSION VALUE IS RESERVED: a session field of exactly `*`\n"
           "marks a CONTROL FRAME, the peer saying which rooms are live as a\n"
           "comma-separated list rather than sending a message. It is handed\n"
           "to the room router verbatim and raises NO CARD — a room list is\n"
           "not a message, and one card per refresh is exactly the clutter the\n"
           "list exists to remove — so `data_control` in `/rns/status` is\n"
           "where it shows. This device only READS them: the envelope builder\n"
           "refuses the reserved session, so no code path here can emit one.\n"
           "A parsed envelope is also offered ONCE to a room router — a\n"
           "function pointer another skill sets at init, null by default,\n"
           "which runs on the loop task and may not block or touch SD. The\n"
           "card is raised either way; `data_routed`, `data_route_refused`\n"
           "and `data_route_reason` are where a message that never reached a\n"
           "conversation shows up, since the screen cannot say so.\n"
           "The node owns a SECOND IN/SINGLE destination, `lxmf.delivery`, a\n"
           "separate address announced alongside `seed.pager` on the same\n"
           "schedule; `lxmf_address` in `/rns/status` is its 32-hex hash. An\n"
           "LXMF single packet addressed to it is copied off the drain into a\n"
           "ring of its own and PARSED on the loop task after the stack has run\n"
           "— never in the callback and never in the drain, because an LXMF\n"
           "verify is tens of milliseconds. It declines link requests and keeps\n"
           "PROVE_NONE for a sharper version of seed.pager's reason: this\n"
           "library proves inside the drain, so a delivery receipt would sign an\n"
           "Ed25519 there and is left for a later commit. `data_lxmf_rx` counts\n"
           "the packets it received, `data_lxmf_ok` those that parsed and\n"
           "`data_lxmf_bad` those that did not — the alive-but-did-not-\n"
           "understand signal.\n"
           "Inbound packets pass a filter registered on Transport before the\n"
           "stack starts: it drops announces not marked as a path response,\n"
           "before the Ed25519 signature is checked — a verification that\n"
           "costs about 220 ms of loop task against an 8 ms drain budget.\n"
           "Being addressable does not change that: a path request for this\n"
           "node arrives as a DATA packet, which the filter keeps, and the\n"
           "reply is an outbound announce the filter never sees. The mark is\n"
           "a header field the sender chooses, so this bounds the ordinary\n"
           "announce traffic this node has no use for and is not a defence\n"
           "against a peer that sets it. `iface.announces_dropped` and\n"
           "`iface.announces_kept` report what the filter did; everything\n"
           "that is not an announce is kept untouched.\n"
           "`mem.alloc_count` and `mem.free_count` are expected to read 0: the\n"
           "build uses the library's heap allocator, which leaves the global\n"
           "`operator new` — and therefore the counters — alone.\n"
           "The node also SENDS. `POST /rns/send` takes\n"
           "`{to, session, text}` and answers `accepted` without waiting: the\n"
           "handler runs on the AsyncTCP task, which may not touch Transport,\n"
           "so it validates, builds the envelope, queues it and returns. The\n"
           "loop task does the rest and the outcome is read from\n"
           "`GET /rns/status`, exactly as `POST /rns/config` already works.\n"
           "The wire format is agreed with the peer and is\n"
           "`1|<32 hex sender address>|<session>|<text>` — parsed by its FIRST\n"
           "THREE separators, so a `|` inside the text is an ordinary\n"
           "character. The sender address is this node's `address`, which is\n"
           "what lets the peer answer a packet that carries no source. The\n"
           "session name is `[A-Za-z0-9._-]`, at most 23 bytes, and may be\n"
           "empty (the peer then routes to its newest session). The envelope\n"
           "costs 36 bytes plus the session, so the text budget is\n"
           "`347 - len(session)` and anything longer is refused with a reason\n"
           "rather than encrypted first and thrown out afterwards.\n"
           "A SEND TO A PEER WE CANNOT RESOLVE DOES NOT FAIL ON ITS OWN: with\n"
           "no path in the table `Transport::outbound()` broadcasts a HEADER_1\n"
           "packet the hub drops in silence, and the receipt still says an\n"
           "interface accepted it. So the loop task refuses to send until\n"
           "`Transport::has_path()` and `Identity::recall()` both answer, asks\n"
           "for the path once, retries about every 10 s up to 7 times and then\n"
           "gives up with a reason. `send_queued`, `send_refused`, `sent`,\n"
           "`send_failed`, `send_error`, `send_us_last` and `send_us_max` in\n"
           "`/rns/status` are that path made visible. Sending signs nothing,\n"
           "but it encrypts with a fresh ephemeral X25519 keypair per packet\n"
           "and nothing is cached between sends — the two timings are what\n"
           "that costs on this chip.\n"
           "A LINE TYPED IN THE `claude` CHAT ROOM GOES OUT THE SAME WAY, and\n"
           "not through a second copy of it: the room calls\n"
           "`rns_send_envelope()`, which is `POST /rns/send`'s own\n"
           "validate-build-enqueue with the HTTP taken off. It is addressed to\n"
           "`peer` — a 32-hex destination in `/rns.json` beside the endpoint,\n"
           "set with `POST /rns/config {\"peer\":\"<32 hex>\"}` (an empty string\n"
           "clears it), reported as `peer` in `/rns/status`, and empty by\n"
           "default. That upsert still needs a `host`, stored or in the same\n"
           "body, because `/rns.json` without an endpoint is not a\n"
           "configuration — send both on a device that has never been\n"
           "configured. Changing the peer does not drop the link; only the\n"
           "endpoint does.\n"
           "The envelope carries THE ROOM'S OWN NAME as its session, so the\n"
           "peer's answer comes back to the room the message was typed in\n"
           "rather than to whichever room is newest.\n"
           "With no peer set, a link that is down, no address of our own, a\n"
           "text over the budget or a full queue, the room prints one short\n"
           "`(rns: ...)` line saying which and the message falls back to the\n"
           "old bridge/mesh path — a fallback that SUCCEEDS says so too, since\n"
           "it went out somewhere the user did not choose.\n"
           "THE OUTBOX RING NOW HAS TWO PRODUCER TASKS — AsyncTCP for the\n"
           "POSTs and the loop task for the keyboard — so every offer goes\n"
           "through one spinlocked helper; the ring itself is still\n"
           "single-producer at any instant, and its consumer is still the loop\n"
           "task alone.\n"
           "If bring-up fails the endpoint stays up and answers `ready:false`\n"
           "with a reason.\n\n"
           "| GET | /rns/status |\n"
           "| POST | /rns/config |\n"
           "| POST | /rns/send |\n";
}

static void rns_register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/rns/status"), HTTP_GET,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        rns_status_json(doc);
        /* ?reset=1 zeroes the drain-time rolling max — and only that — after it
         * has been read into the response, so a caller can start a fresh
         * worst-case window. A lost concurrent drain sample is acceptable for a
         * diagnostic; see the diagnostics block. */
        if (req->hasParam("reset") &&
            req->getParam("reset")->value() == "1") {
            g_drain_us_max = 0;
        }
        notify_send_json(req, 200, doc);
    });

    /* Upsert of /rns.json. This handler runs on the AsyncTCP task, so it
     * validates and writes the file and stops there — applying the endpoint
     * (and dropping a live link for it) belongs to the loop task, which picks
     * the change up through g_rns_cfg_dirty. Poll GET /rns/status for the
     * outcome. */
    server.on(AsyncURIMatcher::exact("/rns/config"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body || !body[0]) {
            free(body);
            notify_send_error(req, 400, "body required");
            return;
        }
        JsonDocument input;
        DeserializationError err = deserializeJson(input, body);
        free(body);
        if (err) {
            notify_send_error(req, 400, "bad json");
            return;
        }

        /* Merge onto whatever is stored, so a caller can flip `enabled`
         * without restating the endpoint. */
        JsonDocument cfg;
        String stored = read_spiffs_file(RNS_CFG_FILE);
        if (stored.length() > 0) deserializeJson(cfg, stored);

        if (!input["host"].isNull()) {
            String host = input["host"].as<String>();
            if (!rns_valid_host(host.c_str())) {
                notify_send_error(req, 400, "invalid host");
                return;
            }
            cfg["host"] = host;
        }
        if (!input["port"].isNull()) {
            long port = input["port"] | 0L;
            if (port < 1 || port > 65535) {
                notify_send_error(req, 400, "bad port");
                return;
            }
            cfg["port"] = port;
        } else if (cfg["port"].isNull()) {
            cfg["port"] = RNS_TCP_PORT_DEFAULT;
        }
        if (input["enabled"].is<bool>()) {
            cfg["enabled"] = input["enabled"].as<bool>();
        } else if (!cfg["enabled"].is<bool>()) {
            cfg["enabled"] = true;
        }
        /* The peer, validated by the SAME rns_addr_valid() POST /rns/send puts
         * a caller's `to` through — an address with a typo is not an error
         * anywhere downstream, it is a different destination, so it has to be
         * refused at the door or never. An explicit empty string clears it,
         * which is the only way to unset a field in a merging upsert. */
        if (!input["peer"].isNull()) {
            String peer = input["peer"].as<String>();
            if (peer.length() == 0) {
                cfg.remove("peer");
            } else if (!rns_addr_valid(peer.c_str())) {
                notify_send_error(req, 400, "peer must be 32 hex characters");
                return;
            } else {
                cfg["peer"] = peer;
            }
        }
        if (cfg["host"].as<String>().length() == 0) {
            notify_send_error(req, 400, "host required");
            return;
        }

        String out;
        serializeJson(cfg, out);
        if (!write_spiffs_file_atomic(RNS_CFG_FILE, RNS_CFG_TMP, out)) {
            notify_send_error(req, 500, "save failed");
            return;
        }
        g_rns_cfg_dirty = true;
        event_add("rns config saved");

        JsonDocument resp;
        resp["ok"] = true;
        resp["applying"] = true;
        resp["host"] = cfg["host"].as<String>();
        resp["port"] = cfg["port"].as<long>();
        resp["enabled"] = cfg["enabled"].as<bool>();
        resp["peer"] = cfg["peer"].isNull() ? String("") : cfg["peer"].as<String>();
        notify_send_json(req, 200, resp);
    }, NULL, handle_body_collect);

    /* POST /rns/send — put one text on a peer's screen.
     *
     * THIS HANDLER MAY NOT SEND, and that is the whole shape of it. It runs on
     * the AsyncTCP task; a send from here would reach Identity::recall() and
     * Transport::outbound(), which walk and lock structures the loop task
     * mutates inside rns_stack.loop(), and would pay for an X25519 key exchange
     * on the socket task while it was at it. So it does the three things that
     * are safe from any task — validate, build, enqueue — and returns.
     *
     * IT ANSWERS PROMPTLY WITH "accepted", NOT WITH AN OUTCOME. Resolving a
     * path can take a minute (see rns_send_poll), and holding an HTTP request
     * open for that is both a stalled AsyncTCP slot and a lie about what was
     * proven. The outcome is polled from GET /rns/status, exactly as POST
     * /rns/config already documents for the endpoint change.
     *
     * WHAT IS STILL ANSWERED SYNCHRONOUSLY is every refusal the caller can fix:
     * a `to` that is not 32 hex characters, a session name outside the agreed
     * charset, an empty text, a text over the budget, and a full queue. Those
     * are decided here precisely so the user learns about them from the request
     * that made them rather than from a counter.
     *
     * The values are read as const char* out of the parsed document rather than
     * as String: they are pointers into storage the document already owns, so
     * nothing on this path allocates beyond the parse itself, and nothing on it
     * logs. */
    server.on(AsyncURIMatcher::exact("/rns/send"), HTTP_POST,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        char *body = notify_take_body(req);
        if (!body || !body[0]) {
            free(body);
            notify_send_error(req, 400, "body required");
            return;
        }
        JsonDocument input;
        DeserializationError err = deserializeJson(input, body);
        free(body);
        if (err) {
            notify_send_error(req, 400, "bad json");
            return;
        }

        const char *to = input["to"] | "";
        const char *session = input["session"] | "";
        const char *text = input["text"] | "";

        /* VALIDATE, BUILD AND ENQUEUE ARE NOT INLINE HERE ANY MORE. They are
         * rns_tx_offer(), because the chat room's outgoing path needs the same
         * three steps and a second copy of them would be a second set of rules;
         * see the comment on that function. What stays in the handler is the
         * only thing that is HTTP's business — turning each refusal into the
         * status code and the sentence the caller can act on. */
        const char *why = nullptr;
        uint16_t len = 0;
        rns_tx_result tx = rns_tx_offer(to, session, text, &why, &len);
        if (tx != RNS_TX_OK) {
            /* 503 for the two conditions that are the device's state and will
             * pass, 400 for the two that are the request's shape and will not. */
            int code = (tx == RNS_TX_NO_ADDR || tx == RNS_TX_FULL) ? 503 : 400;
            notify_send_error(req, code, why);
            return;
        }

        JsonDocument resp;
        resp["ok"] = true;
        resp["accepted"] = true;
        resp["to"] = to;
        resp["bytes"] = len;
        resp["queued"] = (unsigned long)rns_outbox_depth(&g_rns_outbox);
        notify_send_json(req, 200, resp);
    }, NULL, handle_body_collect);
}

/* Announce this node's destination when rns/annsched.h says it is time.
 *
 * Loop task only, and deliberately AFTER rns_stack.loop(): the online flag it
 * reads is set by RnsTcpInterface::loop(), which Reticulum::loop() drives, so
 * running before it would act on last tick's link state and announce into a
 * socket that has already gone.
 *
 * The whole call is wrapped: Destination::announce() reaches Transport::
 * outbound(), Identity::sign() and our own send_outgoing(), and an exception out
 * of any of them would otherwise escape the skill tick into loop(). The stamps
 * are taken whether it threw or not, on purpose — a failing announce that left
 * g_rns_announced false would be re-attempted on EVERY tick, i.e. a software
 * Ed25519 signature every 20 ms. Backing off to the interval is the only sane
 * failure mode. announces_sent counts successes, so sent < attempts is visible
 * as announced:true with announces_sent behind the clock. */
static void rns_announce_poll() {
    if (!rns_dest_ok) return;

    bool online = (bool)g_rns_iface && g_rns_iface.online();
    uint32_t now = millis();

    /* Latch first, decide second. The transition is visible for exactly one
     * evaluation; the latch is what carries it forward when the floor refuses
     * it, so a reconnect is postponed to last_announce + the floor instead of
     * being dropped into the next 30 min interval. */
    g_rns_ann_edge_pending = rns_announce_edge_latch(
        g_rns_ann_edge_pending, online, g_rns_ann_was_online);

    if (rns_announce_due(now, g_rns_ann_last_ms, g_rns_announced, online,
                         g_rns_ann_edge_pending, g_rns_up_ms)) {
        g_rns_ann_last_ms = now;
        g_rns_announced = true;
        /* The ONLY place this is cleared. Not on a refusal, not on the link
         * going down again — the debt outlives both. */
        g_rns_ann_edge_pending = false;
        /* micros() rather than millis(): the number being measured is expected
         * to be a fraction of the 8 ms drain budget, and millis() cannot see
         * it. The unsigned subtraction is correct across the ~71 min micros()
         * rollover. Empty app_data — a display name belongs to LXMF. */
        /* The failure text is COPIED inside the catch and printed outside it.
         * Both halves of that are deliberate. Printed inside, Serial.printf()
         * would block on the UART — milliseconds at 115200 baud — inside a
         * window documented as the Ed25519 sign cost, quietly turning
         * announce_us_max into a measurement of the console. Held as a
         * `const char*` from e.what() and printed outside, it would be a
         * dangling pointer: the exception object is destroyed when the handler
         * exits. So the bytes are copied (cheap, RAM only, no UART) while the
         * exception is alive, and emitted after the clock has stopped.
         *
         * It is the COPY that makes this safe, not the storage: an automatic
         * buffer is enough, this function runs only on the loop task and never
         * re-enters, and 64 bytes of stack beats 64 bytes of permanent .bss
         * plus a reader wondering who else shares it. */
        char err[64];
        uint32_t t0 = micros();
        bool ok = false;
        bool failed = false;
        try {
            rns_destination.announce();
            /* CREATE IS NOT REACHABLE. Without an announce no sender can path to
             * lxmf.delivery, so this is mandatory, not decorative. It is announced
             * on the SAME schedule and inside the SAME timed window as seed.pager:
             * announce() is synchronous and signs with software Ed25519, so the
             * window now measures the pair, but both are off the drain and paced
             * to the 30 min interval (plus the reconnect edge), never per packet.
             * Empty app_data — an LXMF display-name announce is a later commit. */
            if (rns_lxmf_dest_ok) rns_lxmf_destination.announce();
            ok = true;
        } catch (const std::exception &e) {
            failed = true;
            snprintf(err, sizeof(err), "%s", e.what());
        } catch (...) {
            failed = true;
            snprintf(err, sizeof(err), "unknown exception");
        }
        uint32_t dt = (uint32_t)(micros() - t0);
        if (failed) Serial.printf("[rns] announce failed: %s\n", err);
        g_rns_ann_us_last = dt;
        if (dt > g_rns_ann_us_max) g_rns_ann_us_max = dt;
        if (ok) {
            g_rns_ann_sent++;
            /* Same "-" placeholder the boot line uses: the address string is
             * absent only when toHex() failed at bring-up, and an event ring
             * entry reading "rns announced  in 210us" would be the only record
             * of an announce that did happen. */
            event_add("rns announced %s in %luus",
                      rns_dest_addr[0] ? rns_dest_addr : "-", (unsigned long)dt);
        }
    }

    /* Stamped every tick, not only when an announce fired: this is what makes
     * the offline->online edge a one-tick event rather than a permanent state. */
    g_rns_ann_was_online = online;
}

/* Take whatever the packet callback left in the inbox and put it on the screen.
 *
 * Loop task only, and deliberately AFTER rns_stack.loop() — see skill_rns_poll().
 * The drain that fills the inbox runs INSIDE rns_stack.loop(), so a pickup ahead
 * of it is a pickup of last tick's messages: every delivery would sit an extra
 * RNS_TICK_MS on the shelf for no reason. rns_announce_poll() above documents
 * the same ordering for the same reason.
 *
 * IT DRAINS THE RING, it does not take one. A single tick can admit
 * RNS_TCP_DRAIN_FRAMES_MAX packets, and a pickup that took one per tick would
 * turn a burst into one card and seven drops however deep the ring was. The loop
 * is bounded by the ring itself — at most RNS_INBOX_SLOTS iterations, because
 * nothing can refill it from here.
 *
 * WHAT THIS IS ALLOWED TO DO THAT THE CALLBACK IS NOT: everything. It runs after
 * the stack has returned, outside the drain and outside Transport::inbound(), so
 * notify_ingest()'s time(NULL), critical section, event-ring append and archive
 * write-through are all ordinary work here. This is the entire point of the
 * deferral.
 *
 * WHAT THIS DOES NOT DO IS RATE-LIMIT ANYTHING. Eight cards in one tick is eight
 * buzzes, eight screen repaints and eight records queued to the history archive,
 * and nothing above bounds how often that can be provoked by anyone who knows the
 * address. That is a real gap and it is named here rather than papered over; it
 * is not this commit's to close. The archive side of it degrades by DROPPING, not
 * by blocking: HISTORY_WRITE_QUEUE_DEPTH is 16 (skills/history.cpp) and the
 * enqueue is a 0-tick send, so a burst faster than the SD write task loses
 * records into history_drops() while the cards themselves still reach the
 * screen.
 *
 * AND THE FLOOD TAKES THE EVIDENCE WITH IT. notify_ingest() appends one line to
 * the event ring per card, MAX_EVENTS is 64 (main.cpp), and eight cards per
 * 20 ms tick is 64 lines in about 160 ms — so a sustained flood overwrites the
 * ring faster than anyone can read it, including whatever the ring held about
 * the flood starting. The counters in GET /rns/status survive it, because they
 * are counters; /events does not. Anyone diagnosing this reaches for data_rx and
 * data_dropped, not the ring.
 *
 * THE RECEIVE PATH READS THE ENVELOPE THE SEND PATH EMITS, using the parser in
 * rns/outbox.h rather than a second copy of the rules. Until it did, this
 * function sanitised the whole payload onto the card, so a peer answering in
 * the agreed format produced a card reading "1|<32 hex>|<session>|<text>" with
 * the framing on display and the text pushed off the bottom by it. The three
 * questions that deferral was waiting on are answered here and nowhere else:
 *
 *   - A PAYLOAD THAT IS NOT AN ENVELOPE is shown exactly as it arrived and
 *     counted. tools/rns-send sends bare text and so does any peer predating
 *     the format, so this is an ordinary outcome and not a failure; the same
 *     goes for an unknown version, a bad address, a bad session name and a
 *     frame with no text. Nothing is ever dropped or blanked over its shape.
 *   - THE SESSION GOES IN THE TITLE, where it names the conversation without
 *     spending body the message needs.
 *   - THE SENDER ADDRESS IS NOT SHOWN AT ALL. It is kept for replying and
 *     published in GET /rns/status; see the caveat on g_rns_env_from, which is
 *     the part of this that a reply feature must not skip.
 *
 * The format still has exactly one owner, and it is rns/outbox.h. Nothing here
 * may take a payload apart itself: the rules would drift the first time one of
 * them moved, and the failure would be silent in both directions.
 *
 * THE CARD CANNOT HOLD THE WHOLE MESSAGE, and the cut is sized against WHAT THE
 * SCREEN PAINTS rather than against what notify can store. Those are different
 * numbers and using the wrong one is how the previous revision made a marker
 * nobody could ever see. notify's body holds 240 bytes, but
 * hw_ui_show_notify() paints at most RNS_CARD_ROWS rows of RNS_CARD_COLS
 * columns and then stops — there is no scroll on that card — so text past
 * ~RNS_CARD_VISIBLE_CHARS codepoints is stored and never drawn. The budget is in
 * CODEPOINTS because that is what the renderer counts: a byte budget would cut a
 * Cyrillic message at half the text the screen could show.
 *
 * THE MARKER GOES IN FRONT, not at the end, and that is the other half of the
 * same lesson. The renderer word-wraps by backing up to the last space in each
 * row, so how much of the last row is actually used is not predictable from
 * here; a marker at the end of the body can be pushed off the bottom by wrapping
 * alone. The first row is always painted. So a truncated card opens with
 * "[+N B] " and the count is exact: the second sanitising pass re-runs with the
 * marker's room already held back, so N is measured against the text that
 * actually stays rather than against a body the marker then overwrites. */
/* What hw_ui_show_notify() draws for the card body: max_rows 5 at scale 2, and
 * (PANEL_W 480 - MARGIN 12 - 4 - MARGIN 12) / (6 * 2) = 37 columns per row
 * (src/hw_ui.cpp). This is an UPPER BOUND and the budget does not model rows, so
 * cutting here never withholds a character the screen would have shown — but it
 * can leave more hidden than N admits. A body of "ab\n" repeated ends every row
 * after two characters: 383 bytes of it paints 19 characters while the card says
 * "+210 B", because 164 of the characters we kept are on rows six and beyond.
 * Space-heavy text wraps the same way, more mildly. N is honest about what the
 * BODY does not contain and cannot be honest about what the panel does not
 * paint; modelling wrap here would mean reimplementing hw_ui's line breaker
 * against a font, and every other notify source on this device shares the same
 * limitation. The "[+N B]" prefix is on row one either way, which is why it is
 * a prefix. tools/test_task_unblock.py pins the renderer's own constants so this
 * cannot drift out from under us silently. */
#define RNS_CARD_ROWS 5
#define RNS_CARD_COLS 37
#define RNS_CARD_VISIBLE_CHARS (RNS_CARD_ROWS * RNS_CARD_COLS)
/* The room held back for the marker in BOTH budgets. The longest marker is
 * "[+383 B] " at 9 characters, since N can never exceed the payload ceiling;
 * the reservation is 12 rather than 9 so the format string can gain a character
 * without silently eating into the visible budget it was balanced against. The
 * slack costs three characters of a truncated card and nothing on a whole one.
 * RNS_CARD_CUT_MARK_MAX is the separate buffer the marker is formatted into. */
#define RNS_CARD_CUT_RESERVE  12
#define RNS_CARD_CUT_MARK_MAX 16

/* Take whatever the lxmf.delivery callback left, PARSE it off the drain, and
 * ROUTE the result onto the screen. This is the RECEIVE MILESTONE: a message a
 * phone sent lands as a card here.
 *
 * Loop task only, and after rns_stack.loop() for the same reason rns_inbox_poll()
 * below is: the drain that fills this ring runs inside that call, so a pickup
 * ahead of it is one tick stale. What this adds that the seed.pager pickup does
 * not is the parse — lxmf_parse() walks hostile msgpack, tens of milliseconds,
 * which is exactly the work the callback was forbidden and the whole reason it is
 * HERE and not in the drain. C1's counters still move: a clean parse bumps
 * data_lxmf_ok, a malformed one data_lxmf_bad.
 *
 * WHERE A CLEAN MESSAGE GOES is a pure function of its fields — lxmf_route_plan()
 * (src/lxmf_route.h), host-tested apart from the firmware. The DEFAULT, and the
 * milestone, is a plain client (Retichat) with only title+content: it raises a
 * notification card, source "lxmf" and a derived replace-in-place key so a resend
 * updates in place rather than stacking. meta.sev picks the severity, meta.src
 * the source label, meta.key the card key; a thread routes to a room INSTEAD of a
 * card. The renderer is ignored here — every card is plain in v1; a micron-page
 * body is a later ticket.
 *
 * THE TEXT IS SANITISED THE SEED.PAGER WAY. The codec bounds title/content, but
 * they are still arbitrary bytes; rns_text_sanitize() strips control bytes and
 * bounds length into the SAME g_rns_card_body / g_rns_room_text buffers the
 * seed.pager pickup uses, the card body against the screen's character budget and
 * the room text uncut. This never rate-limits: like rns_inbox_poll(), a burst is
 * a buzz per message and that gap is named there, not closed here. */
static void rns_lxmf_inbox_poll() {
    uint16_t len = 0;
    while (rns_inbox_take(&g_rns_lxmf_inbox, g_rns_lxmf_payload,
                          sizeof(g_rns_lxmf_payload), &len)) {
        LxmfReason rr = lxmf_parse(g_rns_lxmf_payload, len, &g_rns_lxmf_msg);
        if (rr != LXMF_OK) { g_rns_lxmf_bad++; continue; }
        g_rns_lxmf_ok++;

        LxmfRoute route;
        lxmf_route_plan(&g_rns_lxmf_msg, &route);

        /* A THREAD ROUTES TO A ROOM, INSTEAD of a card — see lxmf_route.h. The
         * router runs ON THIS TASK, after rns_stack.loop(), so it may not block
         * or touch SD synchronously; with none installed the message is shown
         * nowhere, exactly as a control frame is in rns_inbox_poll(). The content
         * is sanitised uncut into the room's own buffer, the seed.pager way. */
        if (route.kind == LXMF_ROUTE_ROOM) {
            if (g_rns_room_router) {
                int reason = 0;
                rns_text_sanitize((const uint8_t *)g_rns_lxmf_msg.content,
                                  g_rns_lxmf_msg.content_len,
                                  g_rns_room_text, sizeof(g_rns_room_text), 0);
                if (g_rns_room_router(route.room, g_rns_room_text, &reason)) {
                    g_rns_lxmf_rooms++;
                    /* REMEMBER WHO TO ANSWER (C4). The reply path keys the origin
                     * map by the RESOLVED room name — the same sanitised string
                     * the router stored as the session — so a reply typed here
                     * finds it; route.room is the raw thread bytes, so sanitise
                     * with the router's own filter (agents_route_sanitize, the
                     * byte-for-byte twin of agents_session_sanitize). Guarded by
                     * g_rns_tx_mux because the reply lookup can run on AsyncTCP. */
                    char rkey[AGENT_ROUTE_NAME_CAP];
                    agents_route_sanitize(route.room, rkey, sizeof(rkey));
                    if (rkey[0]) {
                        portENTER_CRITICAL(&g_rns_tx_mux);
                        lxmf_origin_set(&g_rns_lxmf_origin, rkey,
                                        g_rns_lxmf_msg.source_hash);
                        portEXIT_CRITICAL(&g_rns_tx_mux);
                    }
                }
            }
            continue;
        }

        /* THE DEFAULT: a card. Title and content are sanitised the seed.pager
         * way — control bytes stripped, the body cut to the screen's character
         * budget — into a title scratch and g_rns_card_body. An empty title still
         * makes a card; notify_ingest() supplies its own placeholder. */
        char title[NOTIFY_TITLE_LEN];
        rns_text_sanitize((const uint8_t *)g_rns_lxmf_msg.title,
                          g_rns_lxmf_msg.title_len,
                          title, sizeof(title), 0);
        rns_text_sanitize((const uint8_t *)g_rns_lxmf_msg.content,
                          g_rns_lxmf_msg.content_len,
                          g_rns_card_body, sizeof(g_rns_card_body),
                          RNS_CARD_VISIBLE_CHARS);
        if (notify_ingest(route.level, route.source,
                          title[0] ? title : "lxmf",
                          g_rns_card_body, route.key) != 0)
            g_rns_lxmf_cards++;
    }
}

static void rns_inbox_poll() {
    uint16_t len = 0;

    while (rns_inbox_take(&g_rns_inbox, g_rns_inbox_payload,
                          sizeof(g_rns_inbox_payload), &len)) {
        /* WHAT THE CARD IS BUILT FROM. An envelope contributes its TEXT and
         * nothing else — the version digit, the 32-character address and the
         * session name are plumbing, and they were on the screen until this
         * commit. Anything that is not an envelope contributes the whole
         * payload, exactly as it arrived, which is what it did before the
         * parser existed and what tools/rns-send still relies on.
         *
         * BOTH POINT INTO g_rns_inbox_payload. The view carries pointers and
         * lengths into the buffer the pickup just filled; nothing is copied and
         * nothing is allocated, and the sanitiser below is still the only thing
         * standing between a stranger's bytes and the renderer. */
        rns_envelope_view ev;
        rns_envin_result er = rns_envelope_parse(g_rns_inbox_payload, len, &ev);
        const uint8_t *src = g_rns_inbox_payload;
        size_t src_len = len;
        char session[RNS_OUTBOX_SESSION_MAX + 1];
        /* A CONTROL FRAME IS NOT A MESSAGE, so it does not become a card. Its
         * payload is the peer's list of live rooms, and a card per list would
         * be a new piece of junk on the screen every time the peer refreshes —
         * the opposite of what the list is for. The router is the only consumer
         * it has; the counter is where it shows up for a human. */
        bool control = false;

        session[0] = '\0';
        if (er == RNS_ENVIN_OK) {
            control = rns_session_is_control_n(ev.session, ev.session_len);
            if (control) g_rns_env_control++;
            else g_rns_env_parsed++;
            src = (const uint8_t *)ev.text;
            src_len = ev.text_len;
            /* Both lengths are bounded by the parser — 32 for the address, 23
             * for the session — and both buffers are sized from the same
             * constants, so the terminator lands inside them by construction. */
            memcpy(session, ev.session, ev.session_len);
            session[ev.session_len] = '\0';
            memcpy(g_rns_env_from, ev.from, ev.from_len);
            g_rns_env_from[ev.from_len] = '\0';
        } else {
            /* NOT AN ERROR AND NOT A DROP: the payload is shown as it came and
             * the reason is kept for GET /rns/status. See READING THE ENVELOPE
             * in rns/outbox.h for why every non-OK outcome ends up here. */
            g_rns_env_raw++;
            g_rns_env_raw_why = rns_envin_reason(er);
            /* A SHAPE THAT TRIED TO BE AN ENVELOPE gets a slot plain text
             * cannot overwrite: with tools/rns-send in ordinary use the field
             * above reads "plain text" permanently, and the one probe with a
             * bad address is exactly what it was supposed to show. */
            if (er != RNS_ENVIN_NO_FRAME) g_rns_env_bad_why = g_rns_env_raw_why;
        }

        /* ---- the card, for everything that IS a message ----
         *
         * A CONTROL FRAME STOPS HERE AND THAT IS THE WHOLE POINT OF THE GUARD.
         * Everything below builds a notification out of the payload, and a
         * control frame's payload is a list of room names: on the screen it
         * would be a card of plumbing arriving as often as the peer refreshes
         * its list, which is more junk than the list was sent to remove. The
         * frame is not lost by skipping the card — the router below is its
         * consumer and data_control is where it shows for a human — and with no
         * router installed it is shown nowhere at all, deliberately: it is not
         * a message, so there is nothing a card could honestly say about it.
         *
         * EVERY OTHER PAYLOAD STILL RAISES ONE, envelope or not, routed or not.
         * `control` is the only condition here, and it can only be true for a
         * payload that parsed. */
        if (!control) {
            /* The return value is bytes written AND input bytes consumed — one
             * number, by construction (see rns/inbox.h). So `kept < src_len` is
             * exactly "something did not fit". */
            size_t kept = rns_text_sanitize(src, src_len,
                                            g_rns_card_body,
                                            sizeof(g_rns_card_body),
                                            RNS_CARD_VISIBLE_CHARS);

            if (kept < src_len) {
                g_rns_card_cut++;
                /* Second pass, with the marker's room held back in BOTH budgets, so
                 * the count is of input the card genuinely does not carry. */
                kept = rns_text_sanitize(src, src_len,
                                         g_rns_card_body + RNS_CARD_CUT_RESERVE,
                                         sizeof(g_rns_card_body) -
                                             RNS_CARD_CUT_RESERVE,
                                         RNS_CARD_VISIBLE_CHARS -
                                             RNS_CARD_CUT_RESERVE);
                char mark[RNS_CARD_CUT_MARK_MAX];
                int mn = snprintf(mark, sizeof(mark), "[+%u B] ",
                                  (unsigned)(src_len - kept));
                /* snprintf returns what it WOULD have written, so an upper bound is
                 * the clause that matters — but the bound is the RESERVATION, not
                 * the size of `mark`. Those differ today (12 against 16) and the
                 * larger one is not safe: the memmove below moves the text down to
                 * offset mn, and any mn past the reservation moves it UP, past the
                 * end of the body by mn - RNS_CARD_CUT_RESERVE bytes. Bounding by
                 * sizeof(mark) leaves that overflow three characters away from a
                 * constant whose own comment invites tuning, and it was reachable by
                 * lowering the reservation alone. The else branch below already
                 * handles a marker that does not fit, so this is the whole fix. */
                if (mn > 0 && mn <= RNS_CARD_CUT_RESERVE) {
                    /* Close the gap between the reservation and the marker's real
                     * length. Overlapping and downward, hence memmove; +1 carries
                     * the terminator the sanitiser wrote. */
                    memmove(g_rns_card_body + mn,
                            g_rns_card_body + RNS_CARD_CUT_RESERVE, kept + 1);
                    memcpy(g_rns_card_body, mark, (size_t)mn);
                } else {
                    /* The marker would not fit its own buffer. Show the truncated
                     * body rather than a corrupted one; data_card_cut and the title
                     * still record that this happened. */
                    memmove(g_rns_card_body,
                            g_rns_card_body + RNS_CARD_CUT_RESERVE, kept + 1);
                }
            }

            /* THE TITLE IS WHERE THE SESSION GOES. It names the conversation this
             * message belongs to, which is the one piece of the envelope a reader
             * needs and the one piece a body cannot carry without becoming
             * plumbing again; a card from an unnamed session, and a card that was
             * never an envelope, read exactly as they did before.
             *
             * The byte count stays on EVERY card, not only a truncated one: it is
             * the number to compare against what the sender says it sent, and it is
             * the only place the length survives once the body has been sanitised
             * and possibly cut. It counts the TEXT for an envelope and the whole
             * payload for anything else — the same bytes the card is built from, so
             * it stays comparable with the "[+N B]" marker, which is measured
             * against exactly that.
             *
             * 48 bytes: "RNS " + 23 of session + ' ' + 3 digits + " B" is 34, and
             * snprintf truncates rather than overflows in any case. */
            char title[48];
            if (session[0])
                snprintf(title, sizeof(title), "RNS %s %u B", session,
                         (unsigned)src_len);
            else
                snprintf(title, sizeof(title), "RNS %u B", (unsigned)src_len);

            /* notify_ingest() returns 0 when the store refused the card, and that is
             * not given its own counter: data_cards falling behind
             * data_rx - data_dropped is the same information without a third number
             * to keep in step. */
            if (notify_ingest("info", "rns", title, g_rns_card_body, NULL) != 0)
                g_rns_cards++;
        }

        /* ---- and then, at most once, the room ----
         *
         * AFTER THE CARD, DELIBERATELY. The card is the outcome that must
         * happen whatever else does; a router that throws its work away, or
         * that somebody sets to a bad pointer, must not be able to swallow the
         * message on the way to the screen.
         *
         * ONLY FOR A PARSED ENVELOPE. A bare-text payload has no session to
         * route by and no sender to answer, so there is nothing to place.
         *
         * A CONTROL FRAME COMES THROUGH HERE TOO, and it is the one case where
         * "after the card" means there was no card. Its session is the single
         * byte '*' and it is handed over EXACTLY AS IT ARRIVED — the copy above
         * is a memcpy of the parsed field and nothing sanitises it — because
         * the router branches on that byte before its own name sanitiser runs.
         * Anything done to it here would be a room named after a control
         * marker.
         *
         * THE ROOM GETS THE WHOLE TEXT, sanitised into its own buffer with no
         * character cap: the card's budget is what the screen paints and has
         * nothing to do with what a conversation should store. Same sanitiser,
         * because the bytes are just as arbitrary here.
         *
         * The router runs ON THIS TASK, right here, after rns_stack.loop() —
         * so it may not touch SD synchronously and may not block. That contract
         * is written out next to the typedef in rns/outbox.h. */
        if (er == RNS_ENVIN_OK && g_rns_room_router) {
            int reason = 0;
            rns_text_sanitize(src, src_len, g_rns_room_text,
                              sizeof(g_rns_room_text), 0);
            if (g_rns_room_router(session, g_rns_room_text, &reason)) {
                g_rns_env_routed++;
                /* THIS ROOM'S ORIGIN IS NOW SEED.PAGER (C4): forget any LXMF
                 * sender so a reply here reverts to the seed.pager envelope.
                 * Keyed by the resolved (sanitised) room name, exactly as the
                 * LXMF set is, so the same room clears the same slot. */
                char rkey[AGENT_ROUTE_NAME_CAP];
                agents_route_sanitize(session, rkey, sizeof(rkey));
                if (rkey[0]) {
                    portENTER_CRITICAL(&g_rns_tx_mux);
                    lxmf_origin_clear(&g_rns_lxmf_origin, rkey);
                    portEXIT_CRITICAL(&g_rns_tx_mux);
                }
            } else {
                /* A REFUSAL IS NOT ALLOWED TO BE SILENT. The card went up
                 * either way, so nothing on the screen says the conversation
                 * never received this message; the counter and the router's own
                 * reason code in GET /rns/status are the only place it shows. */
                g_rns_env_refused++;
                g_rns_env_refuse_reason = reason;
            }
        }
    }
}

/* Retire the message on the ladder, successfully or not, and free what it held.
 *
 * The Destination is released explicitly rather than left to be overwritten by
 * the next message: it holds a shared_ptr to an Object carrying the peer's
 * public key, and a message that fails at 3 a.m. should not keep one alive
 * until the next send happens. */
static void rns_send_retire(bool ok, const char *why) {
    if (ok) {
        g_rns_sent++;
    } else {
        g_rns_send_failed++;
        /* WITH THE ADDRESS IN FRONT. Four messages can be queued and the ladder
         * takes them one at a time, so "no path to the destination" on its own
         * does not say WHICH destination, and the message it belonged to is
         * gone by the time anybody polls. */
        snprintf(g_rns_send_error, sizeof(g_rns_send_error), "%s: %s",
                 g_rns_out_to[0] ? g_rns_out_to : "-",
                 why ? why : "send failed");
    }
    g_rns_out_busy = false;
    g_rns_out_resolved = false;
    g_rns_out_path_asked = false;
    g_rns_out_recalled = false;
    g_rns_out_tries = 0;
    g_rns_out_len = 0;
    g_rns_out_why = nullptr;
    g_rns_out_dest = RNS::Destination(RNS::Type::NONE);
}

/* Ask the network where this destination is, ONCE per message.
 *
 * A path request is an outbound packet of its own, so repeating it on every
 * rung would answer a quiet peer with traffic instead of patience. There is
 * nothing to wait on either: request_path() returns void, this port has no
 * await_path(), and its announce handlers explicitly skip path responses.
 *
 * IT ALSO CLEARS THE RECALL LATCH, and that pairing is the whole reason this is
 * a function rather than three lines inline. The only event that can change
 * what Identity::recall() answers is a fresh announce for the destination, a
 * path response IS such an announce, and this call is the only way this node
 * can ask for one. So the latch is released exactly here and nowhere else: the
 * message gets a second, informed recall after the request, and never a third.
 * The flag is set before the call so a throw out of request_path() cannot leave
 * it asking again on every rung. */
static void rns_send_ask_path(const RNS::Bytes &hash) {
    if (g_rns_out_path_asked) return;
    g_rns_out_path_asked = true;
    g_rns_out_recalled = false;
    RNS::Transport::request_path(hash);
}

/* Move the outbound message one step, and no further than one step.
 *
 * Loop task only, and deliberately AFTER rns_stack.loop() — the same ordering
 * rns_announce_poll() and rns_inbox_poll() document, for a sharper reason here.
 * The path response this function is waiting for arrives through the socket
 * drain that runs INSIDE rns_stack.loop(), so a step taken before it is a step
 * taken against last tick's path table: every message would spend one extra
 * RNS_TICK_MS on each rung of the ladder for nothing.
 *
 * THE ORDER IS RESOLVE, THEN SEND, AND IT IS NOT NEGOTIABLE. Without a path in
 * the table Transport::outbound() falls through to the broadcast branch, emits
 * a HEADER_1 packet on every interface, and the hub on the other end of our TCP
 * socket drops it without a word. receipt_send() returns a receipt all the same
 * — a receipt means "an interface accepted it", never "it was delivered" — so
 * the send reports success and the message is simply gone. Every guard below
 * exists to make that outcome unreachable: nothing calls receipt_send() unless
 * Transport::has_path() has said yes AND Identity::recall() has handed back a
 * usable identity.
 *
 * NOTHING HERE BLOCKS. request_path() returns void, this port has no
 * await_path() and no path-response callback, so polling across ticks is the
 * only mechanism there is — and blocking would be self-defeating, because this
 * task is the one that drains the socket the response has to arrive through.
 * The ladder itself is rns_outbox_next() in rns/outbox.h, host tested.
 *
 * ONE recall() PER MESSAGE. See the outbound state block for why it is
 * expensive here: it reconstructs the identity from the announce in the path
 * table and then calls remember(), which fails against a store that was never
 * initialised and emits an ERRORF — and every emitted line ends in a blocking
 * Serial.flush() on this task. So the Destination it produces is cached in
 * g_rns_out_dest and g_rns_out_resolved, and a retry after a successful resolve
 * does not pay for it again.
 *
 * The whole attempt is wrapped for the reason rns_announce_poll()'s is:
 * assignHex, recall, the Destination constructor, pack()'s encryption and
 * Transport::outbound() are all reachable from here and a throw out of any of
 * them would otherwise escape the skill tick into loop(). */
static void rns_send_poll() {
    /* No rns_started guard: skill_rns_poll() is the only caller and it has
     * already returned on an unstarted stack. A second copy here would read as
     * a condition somebody had thought about rather than a line nothing can
     * reach.
     *
     * Pick the next message up. Only one is worked on at a time: a message can
     * hold the ladder for a minute, and overlapping them would multiply path
     * requests without making anything arrive sooner. */
    if (!g_rns_out_busy) {
        if (!rns_outbox_take(&g_rns_outbox, g_rns_out_to, sizeof(g_rns_out_to),
                             g_rns_out_payload, sizeof(g_rns_out_payload),
                             &g_rns_out_len))
            return;
        g_rns_out_busy = true;
        g_rns_out_tries = 0;
        g_rns_out_last_try_ms = millis();
        g_rns_out_path_asked = false;
        g_rns_out_recalled = false;
        g_rns_out_resolved = false;
        /* NOT a placeholder like "queued": nothing can read this before the
         * first attempt has run and set a real one, and a string that can never
         * be published is a string that will one day be published by accident.
         * rns_send_retire() has the fallback for the impossible case. */
        g_rns_out_why = nullptr;
    }

    uint32_t now = millis();
    rns_outbox_action act = rns_outbox_next(now, g_rns_out_last_try_ms,
                                            g_rns_out_tries);
    if (act == RNS_OUTBOX_WAIT) return;
    if (act == RNS_OUTBOX_GIVE_UP) {
        /* The reason is the one the last attempt left, so "gave up" always says
         * WHAT it gave up on: a path that never arrived, an identity the path
         * table could not reconstruct, or an interface that took nothing. */
        Serial.printf("[rns] send to %s gave up: %s\n", g_rns_out_to,
                      g_rns_out_why ? g_rns_out_why : "unknown");
        event_add("rns send failed: %s",
                  g_rns_out_why ? g_rns_out_why : "unknown");
        rns_send_retire(false, g_rns_out_why);
        return;
    }

    /* An attempt is spent whether it reaches the send or stops at the resolve.
     * Stamped BEFORE the work for the reason the announce stamps its schedule
     * first: a step that throws must back off to the throttle rather than
     * retry on every 20 ms tick. */
    g_rns_out_tries++;
    g_rns_out_last_try_ms = now;

    bool sent = false;
    bool failed = false;
    uint32_t dt = 0;
    char err[64];

    try {
        RNS::Bytes hash;
        hash.assignHex(g_rns_out_to);

        /* EVERY ATTEMPT, NOT ONLY THE UNRESOLVED ONES. This used to live inside
         * the resolve block, which made the invariant this file states — that
         * nothing sends unless has_path() has said yes — true once per message
         * rather than once per send: a rung whose send took no receipt re-entered
         * at the packet build with a path that could since have gone, and the
         * next packet would have been broadcast into silence. Nothing culls the
         * path table in this port today and the per-entry TTL is a week, so this
         * is not reachable now; a guard that is only correct because of a
         * setting two layers down is not a guard. It costs a
         * Persistence::exists() — a key probe, no decode — per rung. */
        if (!RNS::Transport::has_path(hash)) {
            g_rns_out_why = "no path to the destination";
            rns_send_ask_path(hash);
            return;
        }

        if (!g_rns_out_resolved) {
            RNS::Identity peer{RNS::Type::NONE};
            /* THE LATCH BOUNDS THE ATTEMPTS, NOT THE SUCCESSES. g_rns_out_
             * resolved alone would only stop a SUCCESSFUL recall from repeating;
             * a failing one re-enters on every rung, and its failure path is the
             * expensive one — _new_path_table.get(), so a msgpack decode and a
             * Packet unpack, where has_path() above was a bare exists(). See
             * CACHING THE SUCCESS IS NOT ENOUGH in the outbound state block.
             * Set before the call, so a throw cannot leave it clear. */
            if (g_rns_out_recalled) {
                g_rns_out_why = "no identity for the destination";
            } else {
                g_rns_out_recalled = true;
                peer = RNS::Identity::recall(hash);
                if (!peer) {
                    /* A path with no recoverable announce behind it. Worth its
                     * own reason: it is a different fault from "never heard of
                     * it" and points at the path table rather than at the
                     * network. */
                    g_rns_out_why = "no identity for the destination";
                }
            }
            if (!peer) {
                rns_send_ask_path(hash);
                return;
            }
            /* OUT destinations are not registered with Transport (the IN branch
             * is the only one that inserts), so this cannot collide with our
             * own destination and needs no deregistration. */
            g_rns_out_dest = RNS::Destination(peer, RNS::Type::Destination::OUT,
                                              RNS::Type::Destination::SINGLE,
                                              hash);
            g_rns_out_resolved = true;
        }

        /* A FRESH Packet PER ATTEMPT: receipt_send() throws std::logic_error on
         * a Packet that has already been sent, so a retry may never reuse one.
         *
         * The clock starts here and not above on purpose. What is being
         * measured is the ENCRYPTION — pack() calls Destination::encrypt(),
         * which generates an ephemeral X25519 keypair, runs the exchange,
         * derives with HKDF and builds a token, with nothing cached between
         * packets because this port has no ratchets. Including recall() would
         * make the number a measurement of the path table instead, and
         * including the Serial.printf below would make it a measurement of the
         * console at 115200 baud — which is why the reporting is outside the
         * window, exactly as in rns_announce_poll(). */
        RNS::Bytes payload(g_rns_out_payload, g_rns_out_len);
        uint32_t t0 = micros();
        RNS::Packet pkt(g_rns_out_dest, payload);
        RNS::PacketReceipt receipt = pkt.receipt_send();
        dt = (uint32_t)(micros() - t0);
        sent = (bool)receipt;
        if (!sent) g_rns_out_why = "no interface accepted the packet";
    } catch (const std::exception &e) {
        failed = true;
        snprintf(err, sizeof(err), "%s", e.what());
    } catch (...) {
        failed = true;
        snprintf(err, sizeof(err), "unknown exception");
    }

    /* ONLY A SUCCESSFUL SEND IS TIMED. The window closes after receipt_send(),
     * and on the failing branch that call has already emitted ERROR("No
     * interfaces could process the outbound packet") — Packet.cpp — whose
     * Log.cpp line ends in a blocking Serial.flush() on this task. Publishing
     * that as send_us_last would make a number documented as the cost of an
     * X25519 exchange into a measurement of the console at 115200 baud, and no
     * source-text pin can see it because the flush is inside the library. */
    if (sent && dt > 0) {
        g_rns_send_us_last = dt;
        if (dt > g_rns_send_us_max) g_rns_send_us_max = dt;
    }
    if (failed) {
        /* A throw is not retried: assignHex, the Destination constructor and
         * pack() fail for reasons that do not improve with time, and the
         * message would otherwise burn the whole ladder to reach the same
         * exception seven times. The text is copied inside the catch and used
         * here for the reason rns_announce_poll() explains — e.what()'s storage
         * dies with the handler.
         *
         * IT GOES INTO send_error, not only onto the console. A
         * std::length_error out of pack(), a std::invalid_argument and a
         * std::bad_alloc are three different faults with three different fixes,
         * and a fixed literal makes them one sentence for anybody who is not
         * holding a serial cable. */
        Serial.printf("[rns] send to %s threw: %s\n", g_rns_out_to, err);
        event_add("rns send to %s threw: %s", g_rns_out_to, err);
        rns_send_retire(false, err);
        return;
    }
    if (sent) {
        event_add("rns sent %u B to %s in %luus", (unsigned)g_rns_out_len,
                  g_rns_out_to, (unsigned long)dt);
        rns_send_retire(true, nullptr);
    }
    /* Not sent and not thrown: no interface took it, which is ordinarily the
     * link being down. Left on the ladder to be retried on the throttle. */
}

/* The tick Reticulum finally gets. Gated on rns_started because
 * Reticulum::loop() opens with assert(_object) — a no-op under NDEBUG, so an
 * unstarted stack would dereference null rather than trip the assert. */
static void skill_rns_poll() {
    if (!rns_started) return;
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < RNS_TICK_MS) return;
    last = now;
    rns_stack.loop();
    rns_announce_poll();
    /* After rns_stack.loop(), never before it: the drain that fills the inbox
     * runs inside that call, so a pickup ahead of it is one tick stale. */
    rns_inbox_poll();
    /* The lxmf.delivery ring is drained and parsed on the same rule and for a
     * sharper reason: the parse it runs is the tens-of-ms work the callback was
     * forbidden, so it must be here, after the drain, never inside it. */
    rns_lxmf_inbox_poll();
    /* Also after the stack, and for a sharper version of the same reason: the
     * path response the send is waiting for arrives through the drain inside
     * rns_stack.loop(), so a step taken ahead of it reads last tick's path
     * table and every rung of the ladder costs an extra RNS_TICK_MS. */
    rns_send_poll();
    /* Loop task, right after the stack has run: everything GET /rns/status is
     * not allowed to read live is republished here. */
    rns_status_publish();
}

static const Skill rns_skill = {
    .name = "rns",
    .version = "0.5.0",
    .describe = rns_describe,
    .endpoints = rns_endpoints,
    .register_routes = rns_register_routes,
    .tick = skill_rns_poll
};

static void skill_rns_init() {
    if (rns_started) return;             /* one bring-up per boot */

    /* Explicit, unlike the inbox's. That one is a plain struct in .bss and
     * static zero-initialisation is the whole story; this one holds two
     * std::atomic counters whose default constructor is trivial before C++20,
     * so "it is a static, therefore it is zero" is an argument about the
     * linker rather than about the type. Stating it costs one memset at boot. */
    rns_outbox_init(&g_rns_outbox);
    lxmf_origin_init(&g_rns_lxmf_origin);

    try {
        /* The adapter's init() calls SPIFFS.begin(true, "") — formatOnFail with
         * an EMPTY base path. That is a no-op only because setup() already
         * mounted SPIFFS and the framework returns early when mounted. Verify
         * that rather than assume it: if this skill ever runs before the mount,
         * the adapter would format the partition and take /mesh_identity.id,
         * /gw_token.txt, /wg.json and the notify store with it. */
        if (SPIFFS.totalBytes() == 0) {
            rns_error = "SPIFFS not mounted; refusing to let the adapter mount it";
        } else if (!rns_filesystem.init(false)) {
            rns_error = "SPIFFS adapter failed to attach";
        } else {
            RNS::Utilities::OS::register_filesystem(rns_filesystem);
            /* Matches the -DRNS_LOG_LEVEL=RNS_LOG_LEVEL_ERROR compile-time
             * floor: anything above ERROR is not in the binary to emit, and
             * LOG_TRACE would in any case stall the loop task on the console. */
            RNS::loglevel(RNS::LOG_ERROR);
            RNS::Reticulum::transport_enabled(false);
            RNS::Reticulum::probe_destination_enabled(false);
            /* BEFORE start(), and before any interface exists to deliver a
             * packet: Transport::inbound() reads _callbacks._filter_packet on
             * every packet with no null check beyond `if (_callbacks.
             * _filter_packet)`, so a late registration is simply a window in
             * which announces are verified at full price. Nothing here depends
             * on the stack being up — the setter writes one static pointer. */
            RNS::Transport::set_filter_packet_callback(rns_packet_filter);
            /* NOTHING ELSE GOES BETWEEN HERE AND start(). A previous revision
             * called Identity::known_destinations_maxsize(64) and
             * Transport::path_table_maxsize(64) on this line, to cap heap stores
             * that -DRNS_PERSIST_*=0 had made live and that the library's own
             * set_max_recs calls do not reach on a node with
             * transport_enabled(false). That firmware panic-looped on the device
             * and was rolled back. Do not read that as "these two lines faulted"
             * — they cannot, each is a scalar store into a static that was
             * constructed before app_main, and the OTA image is confirmed on 60 s
             * of uptime rather than on reaching the end of setup(). They are gone
             * because the flag that needed them is gone: known destinations is
             * back at
             * the library default, where the store is an invalid FileStore that
             * stores nothing and therefore grows by nothing. The path store
             * keeps its flag — it has been a live heap store across every build
             * that has booted here, bounded only by a one-week per-entry TTL,
             * and the filter above strictly reduces what reaches it. It is not
             * capped here either: this is where that was tried. The hashlist caps
             * itself: set_max_recs runs at the top of Transport::start(),
             * outside the transport_enabled() block. See platformio.ini. */
            rns_stack = RNS::Reticulum();
            rns_stack.start();
            rns_started = true;
        }
    } catch (const std::exception &e) {
        rns_started = false;
        rns_error = "exception during bring-up";
        Serial.printf("[rns] bring-up failed: %s\n", e.what());
    } catch (...) {
        rns_started = false;
        rns_error = "unknown exception during bring-up";
    }

    /* Separate scope: the identity is layered on top of a stack that already
     * started, so a throw here must not un-report rns_started. */
    if (rns_started) {
        try {
            rns_identity_bring_up();
        } catch (const std::exception &e) {
            rns_error = "exception during identity bring-up";
            Serial.printf("[rns] identity failed: %s\n", e.what());
        } catch (...) {
            rns_error = "unknown exception during identity bring-up";
        }
    }

    /* Destination: the thing that makes this node addressable at all.
     *
     * Its own scope, after the identity and after start(), for the reason the
     * identity phase has one: the constructor calls
     * Transport::register_destination() itself and that THROWS
     * std::runtime_error on a duplicate hash (Transport.cpp), so a second
     * bring-up must not be able to un-report rns_started. skill_rns_init()'s
     * `if (rns_started) return;` guard is what keeps it to one construction per
     * boot; nothing else does.
     *
     * Gated on rns_identity_ok, not just rns_started — see the rns_destination
     * declaration for what a NONE identity would silently do to the address.
     *
     * accepts_links(false) is NOT a default. Destination::Object initialises
     * _accept_link_requests = true, and an inbound LINKREQUEST to a destination
     * that accepts one runs an X25519 key exchange plus an Ed25519 signature
     * synchronously inside the 8 ms drain, at the request of any stranger who
     * can reach the interface. Nothing on this node terminates a link yet, so
     * every one of those would be paid for and discarded. The library's own
     * probe destination sets exactly this (Transport.cpp, probe responder).
     *
     * The proof strategy is left at the library default, PROVE_NONE. PROVE_ALL
     * would sign a proof for every inbound DATA packet, again inside the drain,
     * and it is not what makes the node reachable: reachability is answered by
     * path requests, which the library handles on its own and which need no
     * proof strategy at all. */
    if (rns_started && rns_identity_ok) {
        try {
            rns_destination = RNS::Destination(
                rns_identity, RNS::Type::Destination::IN,
                RNS::Type::Destination::SINGLE, RNS_DEST_APP_NAME,
                RNS_DEST_ASPECTS);
            rns_destination.accepts_links(false);
            rns_destination.set_packet_callback(rns_data_callback);
            /* Everything after the constructor is an inline scalar store into
             * the shared object and cannot throw, so this line is the first
             * moment the destination is both registered AND configured — which
             * is exactly what rns_dest_ok means. Nothing that can throw is
             * allowed between the constructor and here. */
            rns_dest_ok = true;
        } catch (const std::exception &e) {
            rns_error = "exception creating the destination";
            Serial.printf("[rns] destination failed: %s\n", e.what());
        } catch (...) {
            rns_error = "unknown exception creating the destination";
        }
    }

    /* Separate scope for the address STRING, for the same reason the identity
     * has one and with a distinct error of its own. From the constructor above
     * onwards the destination is registered in Transport::_destinations: it is
     * already answering path requests and already paying a signature for each
     * one, whatever this file thinks of it. Bytes::toHex() builds a std::string
     * and is the only statement here that can fail. Folding it into the block
     * above would report "exception creating the destination" for a destination
     * that was created perfectly well — /rns/status would show address:null
     * beside an error denying the destination exists, while announces_sent
     * climbed. What actually failed in that case is the rendering, and that is
     * what it now says. */
    if (rns_dest_ok) {
        try {
            snprintf(rns_dest_addr, sizeof(rns_dest_addr), "%s",
                     rns_destination.hash().toHex().c_str());
        } catch (const std::exception &e) {
            rns_error = "destination created but its address could not be rendered";
            Serial.printf("[rns] address render failed: %s\n", e.what());
        } catch (...) {
            rns_error = "destination created but its address could not be rendered";
        }
    }

    /* The LXMF delivery destination, a SEPARATE address alongside seed.pager.
     * Same gating (rns_started && rns_identity_ok) and the same construction
     * rules as the seed.pager block above — built on the loaded identity, IN/
     * SINGLE, links declined — because a different (app_name, aspects) pair only
     * changes the hash, not the hazards. register_destination() cannot collide:
     * two distinct hashes.
     *
     * THE PROOF STRATEGY IS LEFT AT THE LIBRARY DEFAULT, PROVE_NONE, ON PURPOSE,
     * and not for seed.pager's reason. microReticulum 0.5.0 runs the proof
     * strategy in Transport::inbound() ON THE LOOP TASK INSIDE THE DRAIN
     * (Transport.cpp: after destination.receive(), a PROVE_APP destination calls
     * its _proof_requested callback and, on true, packet.prove()), and
     * packet.prove() is Identity::prove() — an Ed25519 SIGN plus a packet send,
     * synchronous, tens of milliseconds. Setting PROVE_APP here would therefore
     * drop that sign into the 8 ms drain for every message we recognise as ours,
     * which is exactly the cost this file declines everywhere else. There is no
     * off-loop prove hook in this library, so a delivery RECEIPT — which is what
     * PROVE_APP buys — waits for a later commit that can prove after the drain.
     * PROVE_ALL is worse still (a sign per inbound DATA packet, authenticated or
     * not) and is never an option here. */
    if (rns_started && rns_identity_ok) {
        try {
            rns_lxmf_destination = RNS::Destination(
                rns_identity, RNS::Type::Destination::IN,
                RNS::Type::Destination::SINGLE, RNS_LXMF_APP_NAME,
                RNS_LXMF_ASPECTS);
            rns_lxmf_destination.accepts_links(false);
            rns_lxmf_destination.set_packet_callback(rns_lxmf_data_callback);
            rns_lxmf_dest_ok = true;
        } catch (const std::exception &e) {
            rns_error = "exception creating the lxmf destination";
            Serial.printf("[rns] lxmf destination failed: %s\n", e.what());
        } catch (...) {
            rns_error = "unknown exception creating the lxmf destination";
        }
    }

    /* Its address string, in its own scope for the same reason seed.pager's is:
     * from the constructor onwards the destination is registered and answering
     * path requests whatever toHex() does, so a rendering failure must not read
     * as the destination not existing. */
    if (rns_lxmf_dest_ok) {
        try {
            snprintf(rns_lxmf_dest_addr, sizeof(rns_lxmf_dest_addr), "%s",
                     rns_lxmf_destination.hash().toHex().c_str());
        } catch (const std::exception &e) {
            rns_error = "lxmf destination created but its address could not be rendered";
            Serial.printf("[rns] lxmf address render failed: %s\n", e.what());
        } catch (...) {
            rns_error = "lxmf destination created but its address could not be rendered";
        }
    }

    /* Interface: construct -> mode -> register -> start, the order the
     * upstream example uses. It runs AFTER Reticulum::start() rather than
     * before, which is safe because Transport::start() never touches the
     * interface table, and it keeps a half-started stack from acquiring an
     * interface it cannot service.
     *
     * start() opens no socket — see the class comment. The dial is decided by
     * the tick once WiFi is up, so an interface is registered even when
     * /rns.json is missing or disabled: that is what lets POST /rns/config
     * bring the link up later without a reboot. */
    if (rns_started) {
        try {
            rns_cfg_load();
            g_rns_iface = new RnsTcpInterface();
            g_rns_iface.mode(RNS::Type::Interface::MODE_FULL);
            RNS::Transport::register_interface(g_rns_iface);
            if (!g_rns_iface.start()) rns_error = "TCP interface refused to start";
        } catch (const std::exception &e) {
            rns_error = "exception registering the TCP interface";
            Serial.printf("[rns] interface failed: %s\n", e.what());
        } catch (...) {
            rns_error = "unknown exception registering the TCP interface";
        }
    }

    /* THE ROOM. Until this line the router pointer was null and an inbound
     * envelope became a card and nothing else — the behaviour that shipped
     * before either half of the chat existed. One call is the whole wiring:
     * from here a parsed envelope also lands in the `claude` agent's room named
     * by its session field, and the two counters in GET /rns/status start
     * moving. It is registered unconditionally, whatever the bring-up above
     * concluded, because the router can only ever be reached from a packet the
     * stack delivered — a node that failed to start delivers none, and a node
     * that comes up later through POST /rns/config must not find the hook
     * missing because of how boot went. */
    rns_set_room_router(claude_route_incoming);

    Serial.printf("[rns] started=%d identity=%d hash=%s iface=%s:%u en=%d%s%s\n",
                  (int)rns_started, (int)rns_identity_ok,
                  rns_hexhash[0] ? rns_hexhash : "-",
                  g_rns_host[0] ? g_rns_host : "-", (unsigned)g_rns_port,
                  (int)g_rns_cfg_enabled,
                  rns_error ? " err=" : "", rns_error ? rns_error : "");
    skill_register(&rns_skill);
    event_add("rns skill started=%d id=%d iface=%d", (int)rns_started,
              (int)rns_identity_ok, (int)g_rns_cfg_enabled);
}
