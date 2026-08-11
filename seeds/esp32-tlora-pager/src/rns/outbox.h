#pragma once

/*
 * rns/outbox.h — the handoff between POST /rns/send and the loop task that
 * actually encrypts and transmits, plus the wire envelope the peer agreed to —
 * BOTH HALVES OF IT, emitted and read — and the retry decision that keeps an
 * unresolvable address from being a silent failure.
 *
 * THE PARSER IS IN HERE AND NOT IN THE RECEIVE PATH, even though it is the
 * receive path that calls it, because the format has to have exactly one owner.
 * See READING THE ENVELOPE further down for what that buys and for the rule
 * that keeps a payload which is not an envelope from becoming a lost message.
 *
 * WHY THIS EXISTS. Sending is the mirror image of receiving and it is harder in
 * exactly one way: the producer is a DIFFERENT TASK. The HTTP handler runs on
 * the AsyncTCP task and the send has to run on the loop task, because the send
 * reaches Identity::recall(), Transport::has_path() and Transport::outbound(),
 * all of which walk structures the loop task mutates — and because the send
 * encrypts, which is public-key work nobody wants on the socket task.
 *
 * THE MEMORY ORDERING HERE IS LOAD-BEARING, and that is the one line of this
 * file that rns/inbox.h could afford not to write. Inbox says its publish-last
 * discipline is "defensive rather than load-bearing today", and it is right:
 * both of its sides are the loop task. Here they are not. The producer is
 * AsyncTCP (its own task, and on this dual-core chip potentially its own core)
 * and the consumer is the loop task, so a payload store that the compiler or
 * the store buffer floats PAST the index that publishes it is a real defect
 * with a real symptom — a peer receiving a slot that was still being filled.
 * So the two indices are std::atomic and the pairing is spelled out rather than
 * implied:
 *
 *   producer: memcpy the bytes, then wr.store(release)
 *   consumer: wr.load(acquire), read the bytes, then rd.store(release)
 *   producer: rd.load(acquire) to see how much room it has
 *
 * That is the textbook single-producer/single-consumer ring, and it is written
 * with TWO MONOTONIC COUNTERS rather than a head and a shared count for the
 * reason that makes SPSC work at all: every variable has exactly one writer.
 * `wr` is the producer's alone, `rd` is the consumer's alone, and the depth is
 * their unsigned difference. A shared `count` incremented by one side and
 * decremented by the other — which is what inbox.h uses, correctly, because
 * there both sides are one task — would be a read-modify-write from two tasks
 * and would lose messages.
 *
 * THE COUNTERS WRAP AT 2^32, AND THE DEPTH SURVIVES IT BECAUSE IT IS UNSIGNED —
 * but the SLOT INDEX does not survive it for free, and an earlier draft of this
 * comment claimed it did. `wr % RNS_OUTBOX_SLOTS` is only continuous across the
 * wrap when RNS_OUTBOX_SLOTS divides 2^32, i.e. when it is a power of two. At
 * five slots, wr = 0xFFFFFFFF and wr = 0 both land on slot 0: one queued
 * message is overwritten before it is read and another is delivered twice,
 * while the depth reads a perfectly sensible 2. So the power of two is a
 * CORRECTNESS REQUIREMENT rather than a taste in round numbers, and the
 * static_assert below is what says so to anybody retuning the depth.
 *
 * WHY FOUR SLOTS. Not a smoothing buffer this time, and the arithmetic is
 * different from the inbox's. The inbox is sized to one drain pass because a
 * burst arrives whether we like it or not. Here nothing bursts on its own: each
 * slot is a deliberate HTTP request, and the consumer spends up to
 * (RNS_OUTBOX_ATTEMPTS_MAX - 1) * RNS_OUTBOX_RETRY_MS — 60 s, not 70, because
 * the first attempt is immediate and the last one is not followed by a wait —
 * on ONE message before giving up, since a path may have to be requested and
 * then waited for. So the
 * queue can never be a throughput device: four messages is already up to four
 * minutes of work. Four is chosen as "a handful of requests typed or scripted
 * while one is resolving" — deep enough that a caller sending a message in
 * pieces is not refused mid-way, shallow enough that ~1.7 KB of static RAM buys
 * it on a board that just had its buffers moved to PSRAM to free internal RAM.
 * A fifth message is refused and COUNTED, never overwritten: the sender is the
 * one still holding a refused message, and it gets an error out of the same
 * POST that offered it.
 *
 * THE ENVELOPE IS FROZEN BY AGREEMENT WITH THE PEER. Do not redesign it here:
 *
 *     1|<32 hex sender address>|<session name>|<text>
 *
 * Four fields, three separators, and the separator is '|'. The version is a
 * SINGLE DIGIT, so a reader can branch on one byte without parsing. The sender
 * address is this node's DESTINATION hash — the value GET /rns/status calls
 * `address`, not `hash` — and it is in the envelope for one reason: a packet to
 * a SINGLE destination carries no source, so without this the peer receives
 * text it cannot answer.
 *
 * PARSE AND EMIT BY THE FIRST THREE SEPARATORS ONLY. The text is the remainder
 * of the payload after the third '|', byte for byte, and it is NOT escaped —
 * a '|' inside the message is an ordinary character and must not break the
 * frame. This is the whole reason the rule is stated as "first three" rather
 * than "split on '|'": the naive split is correct for every message anybody
 * tests by hand and wrong for the first message that quotes a shell pipeline.
 * The builder therefore appends the text verbatim and this file contains no
 * escaping, no quoting and no separator scan over the text.
 *
 * THE BUDGET, and the reason it is checked HERE rather than found out later.
 * RNS_OUTBOX_PAYLOAD_MAX is 383 — Type::Packet::ENCRYPTED_MDU, the largest
 * plaintext one encrypted packet to a SINGLE destination carries, the same
 * constant rns/inbox.h receives into and the one skills/rns.cpp static_asserts
 * against the library. The envelope costs 36 bytes plus the session name:
 *
 *     1 (version) + 1 + 32 (address) + 1 + len(session) + 1 = 36 + len(session)
 *
 * so the text budget is 347 - len(session). NOTHING IN THE LIBRARY CHECKS THIS
 * BEFORE ENCRYPTING. Packet::pack() throws std::length_error on the packed
 * size, which is AFTER Destination::encrypt() has generated an ephemeral X25519
 * keypair and run the exchange — about a tenth of a second of loop task spent
 * to produce an exception. Refusing an oversize message at the door costs a
 * strlen, and it lets POST /rns/send answer the caller with a reason instead of
 * accepting a message that will fail later where only a counter can see it.
 *
 * THE SESSION NAME may be EMPTY, and that is a documented value rather than an
 * oversight: the peer routes an empty session to its newest one. Otherwise it
 * is [A-Za-z0-9._-], at most RNS_OUTBOX_SESSION_MAX bytes. The charset is
 * deliberately narrow — it is the character class that cannot contain the
 * separator, cannot contain a control byte, and needs no quoting wherever the
 * peer puts it.
 *
 * THE RETRY DECISION IS A PURE FUNCTION for the same reason rns/annsched.h's
 * schedule is: the caller cannot block. Transport::request_path() returns void,
 * this library has no await_path() and no path-response callback, so polling
 * Transport::has_path() across ticks is the ONLY mechanism available — and a
 * busy-wait would be worse than untidy, because the loop task is what drains
 * the socket, so blocking on a path response prevents the very frame carrying
 * it from ever being read. rns_outbox_next() turns "how long have we been
 * trying" into one of three answers and holds the two numbers that bound it.
 *
 * PURITY. No Arduino, no clock of its own, no globals: the firmware owns the
 * storage and passes every millis() stamp in, exactly like rns/inbox.h,
 * rns/annsched.h and rns/pktfilter.h. tools/test_rns_outbox.sh compiles this
 * header on the host and drives the real code.
 */

#include <atomic>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Type::Packet::ENCRYPTED_MDU. static_asserted against the library's own enum
 * in skills/rns.cpp, next to the identical assertion rns/inbox.h needs. */
#define RNS_OUTBOX_PAYLOAD_MAX 383
/* A destination hash as GET /rns/status prints it: 32 lowercase hex characters,
 * no separators — the form rnpath and rnprobe take. */
#define RNS_OUTBOX_ADDR_HEX 32
/* '1' '|' <32> '|' <session> '|' — everything the text is not. */
#define RNS_OUTBOX_ENVELOPE_OVERHEAD 36
/* Longest session name the peer accepts. */
#define RNS_OUTBOX_SESSION_MAX 23
/* Text budget with an EMPTY session: 383 - 36. A named session costs its own
 * length on top, which is why the check below is a function and not this
 * constant. */
#define RNS_OUTBOX_TEXT_MAX \
    (RNS_OUTBOX_PAYLOAD_MAX - RNS_OUTBOX_ENVELOPE_OVERHEAD)

/* See WHY FOUR SLOTS above. */
#define RNS_OUTBOX_SLOTS 4
/* ...and see THE COUNTERS WRAP for why the shape of that number is not free:
 * the slot index is the monotonic counter modulo this, and it is only
 * continuous across the 2^32 wrap when this divides 2^32. */
static_assert((RNS_OUTBOX_SLOTS & (RNS_OUTBOX_SLOTS - 1)) == 0,
              "RNS_OUTBOX_SLOTS must be a power of two: the slot index is the "
              "monotonic counter modulo it, and any other depth loses and "
              "duplicates a message when the counter wraps at 2^32");

/* The retry ladder. Reference values from shipped firmware on this board: one
 * attempt every ~10 s, about seven of them, so a destination that never
 * resolves fails in about a minute rather than never. The first attempt is
 * immediate — see rns_outbox_next(). */
#define RNS_OUTBOX_RETRY_MS 10000UL
#define RNS_OUTBOX_ATTEMPTS_MAX 7

/* Why a message could not be turned into an envelope. Every one of these is a
 * refusal the CALLER can act on, which is why they are distinct: POST
 * /rns/send answers with the reason and the message is never queued. */
typedef enum {
    RNS_ENV_OK = 0,
    RNS_ENV_BAD_FROM,     /* this node's own address is not 32 hex characters */
    RNS_ENV_BAD_SESSION,  /* too long, or a byte outside [A-Za-z0-9._-] */
    RNS_ENV_EMPTY_TEXT,   /* see the note in rns_envelope_build() */
    RNS_ENV_TOO_LONG,     /* text longer than 347 - len(session) */
    RNS_ENV_NO_ROOM       /* caller's buffer is smaller than the ceiling */
} rns_env_result;

/* A static literal for each, so the firmware can put it straight into
 * GET /rns/status without formatting anything. */
static inline const char *rns_env_reason(rns_env_result r) {
    switch (r) {
        case RNS_ENV_OK:          return "ok";
        case RNS_ENV_BAD_FROM:    return "this node has no address yet";
        case RNS_ENV_BAD_SESSION: return "bad session name";
        case RNS_ENV_EMPTY_TEXT:  return "empty text";
        case RNS_ENV_TOO_LONG:    return "text longer than the packet allows";
        case RNS_ENV_NO_ROOM:     return "no room for the envelope";
    }
    return "unknown";
}

/* The single digit the first field carries. It is a CONSTANT rather than a
 * literal in two places because the builder and the reader below are the two
 * halves that must never disagree: a version bumped in the emitter alone would
 * put a payload on the wire that this same firmware then reads as ordinary
 * text, and nothing would say so. */
#define RNS_ENVELOPE_VERSION '1'

/* n hexadecimal characters, with NO opinion about what follows them. Split out
 * of rns_addr_valid() because the reader below has a length and not a
 * terminator: an inbound address field is 32 bytes in the middle of a payload
 * that is not NUL-terminated anywhere. */
static inline bool rns_hex_n(const char *p, size_t n) {
    if (!p) return false;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

/* Exactly RNS_OUTBOX_ADDR_HEX hexadecimal characters and nothing else.
 *
 * Both ends of a send go through this: the `to` a caller posted and this node's
 * own address as it goes into the envelope. Bytes::assignHex() validates
 * NOTHING — its digit arithmetic maps every input byte to some value and the
 * only length handling is a truncation to an even count — so an address with a
 * typo would decode to a DIFFERENT hash and the packet would be encrypted and
 * sent into the void. Case is accepted either way; the device's own address is
 * always lowercase.
 *
 * The scan stops at the first character that is not a hex digit, so a string
 * shorter than 32 characters is refused on its terminator rather than read
 * past it. */
static inline bool rns_addr_valid(const char *hex) {
    if (!rns_hex_n(hex, (size_t)RNS_OUTBOX_ADDR_HEX)) return false;
    return hex[RNS_OUTBOX_ADDR_HEX] == '\0';
}

/* [A-Za-z0-9._-] over n bytes, length checked against RNS_OUTBOX_SESSION_MAX,
 * and n == 0 IS LEGAL — the peer routes an empty session to its newest one.
 * The length-carrying form, for the same reason rns_hex_n() exists. */
static inline bool rns_session_valid_n(const char *session, size_t n) {
    if (!session) return false;
    if (n > (size_t)RNS_OUTBOX_SESSION_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = session[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

/* The NUL-terminated form, which is what POST /rns/send has. */
static inline bool rns_session_valid(const char *session) {
    if (!session) return false;
    return rns_session_valid_n(session, strlen(session));
}

/* Bytes of text a session name of this LENGTH leaves room for. */
static inline size_t rns_envelope_text_budget_n(size_t session_len) {
    if (session_len >= (size_t)RNS_OUTBOX_TEXT_MAX) return 0;
    return (size_t)RNS_OUTBOX_TEXT_MAX - session_len;
}

/* Bytes of text this session name leaves room for: 347 with an empty session,
 * one fewer for each character of a named one. */
static inline size_t rns_envelope_text_budget(const char *session) {
    return rns_envelope_text_budget_n(session ? strlen(session) : 0);
}

/*
 * Build `1|<from>|<session>|<text>` into out, and say why not when it cannot.
 *
 * The payload is NOT NUL-terminated: it is bytes on a wire, *out_len of them,
 * and the terminator would be a byte of somebody's message. out_cap must be at
 * least RNS_OUTBOX_PAYLOAD_MAX; anything smaller is refused rather than
 * truncated, because a truncated envelope is a valid-looking envelope with a
 * cut message inside it.
 *
 * AN EMPTY TEXT IS REFUSED, and not for tidiness. It is refused for the
 * opposite of the reason an empty INBOUND payload is impossible: an empty
 * envelope is not empty at all — it is 36 bytes of frame — so it would encrypt,
 * travel, decrypt and reach the peer as a message with nothing in it, which is
 * a notification the recipient cannot act on and cannot distinguish from a bug
 * at either end. Refusing costs the caller one error and nobody a packet.
 *
 * The text is appended VERBATIM. See PARSE AND EMIT BY THE FIRST THREE
 * SEPARATORS above: no escaping happens here and none may be added, because the
 * peer's parser splits three times and takes the rest.
 */
static inline rns_env_result rns_envelope_build(const char *from_hex,
                                                const char *session,
                                                const char *text,
                                                uint8_t *out, size_t out_cap,
                                                uint16_t *out_len) {
    size_t slen, tlen, o = 0;

    if (out_len) *out_len = 0;
    if (!out || out_cap < (size_t)RNS_OUTBOX_PAYLOAD_MAX) return RNS_ENV_NO_ROOM;
    if (!rns_addr_valid(from_hex)) return RNS_ENV_BAD_FROM;
    if (!rns_session_valid(session)) return RNS_ENV_BAD_SESSION;
    if (!text || text[0] == '\0') return RNS_ENV_EMPTY_TEXT;

    slen = strlen(session);
    tlen = strlen(text);
    if (tlen > rns_envelope_text_budget(session)) return RNS_ENV_TOO_LONG;

    out[o++] = RNS_ENVELOPE_VERSION;
    out[o++] = '|';
    memcpy(out + o, from_hex, (size_t)RNS_OUTBOX_ADDR_HEX);
    o += (size_t)RNS_OUTBOX_ADDR_HEX;
    out[o++] = '|';
    memcpy(out + o, session, slen);
    o += slen;
    out[o++] = '|';
    memcpy(out + o, text, tlen);
    o += tlen;

    if (out_len) *out_len = (uint16_t)o;
    return RNS_ENV_OK;
}

/*
 * ---- READING THE ENVELOPE ------------------------------------------------
 *
 * THIS LIVES NEXT TO THE BUILDER AND NOWHERE ELSE. The format has exactly one
 * owner, so that emit and parse cannot drift: a second parser in the receive
 * path would be a copy of these rules that nothing forces to stay a copy, and
 * the first time a rule moved — the version digit, the session charset, the
 * "first three separators" reading — one half would follow it and the other
 * would not. tools/test_rns_outbox.cpp closes the loop by feeding this
 * function what rns_envelope_build() emitted and requiring the inputs back.
 *
 * PARSING IS SOFT, AND THAT IS THE WHOLE DESIGN. A payload arrives from the
 * network before anything has authenticated it, and NOT EVERY PAYLOAD IS AN
 * ENVELOPE: tools/rns-send sends bare text, and so does every peer that
 * predates the format. So this function has exactly two outcomes for the
 * caller — "here are the three fields" and "this is not an envelope" — and the
 * second one is not an error condition. The caller shows a non-envelope
 * exactly as it arrived and counts it. THE ONE OUTCOME THAT MUST NEVER HAPPEN
 * IS A DROPPED OR BLANKED MESSAGE: nothing here refuses a payload, it only
 * declines to take it apart.
 *
 * WHICH IS WHY THE RULES ARE STRICT EVEN THOUGH THE OUTCOME IS SOFT. Every
 * field is validated against the same constants the builder emits under — the
 * version digit, 32 hexadecimal characters, [A-Za-z0-9._-] within 23 bytes,
 * and a text that fits the budget — because a field that is nearly right is
 * more dangerous than one that is obviously wrong: a 40-byte "session" would
 * become a title, a non-hex "address" would become a reply target. Anything
 * that does not match the shape is not an envelope, and the whole payload is
 * shown raw instead. That includes an UNKNOWN VERSION DIGIT: '2' is a format
 * this firmware cannot read, so the honest thing is to put the bytes on the
 * screen rather than guess that the fields behind it are still in this order.
 *
 * AN EMPTY TEXT IS ALSO NOT AN ENVELOPE, and that is the same rule seen from
 * the other side. rns_envelope_build() refuses to emit one, so a frame with
 * nothing after the third separator was not built here; showing it raw puts
 * 36 bytes of visible frame on the screen, which is evidence, where taking it
 * apart would produce a card with an empty body — the blank message this file
 * exists to prevent.
 *
 * NOTHING IS COPIED AND NOTHING IS ALLOCATED. The view points INTO the
 * caller's buffer and its fields are NOT NUL-terminated, exactly like the
 * payload they point at; every one carries its own length. The caller owns the
 * buffer and must not let the view outlive it.
 */
typedef enum {
    RNS_ENVIN_OK = 0,
    RNS_ENVIN_NO_FRAME,      /* fewer than three separators: bare text */
    RNS_ENVIN_BAD_VERSION,   /* the first field is not the one digit we read */
    RNS_ENVIN_BAD_FROM,      /* the address field is not 32 hex characters */
    RNS_ENVIN_BAD_SESSION,   /* outside [A-Za-z0-9._-], or over 23 bytes */
    RNS_ENVIN_EMPTY_TEXT,    /* a frame with no message inside it */
    RNS_ENVIN_TOO_LONG       /* more bytes than one packet can carry */
} rns_envin_result;

/* A static literal for each, so the firmware can publish the last one without
 * formatting anything. They are DIAGNOSTICS ONLY: every value but
 * RNS_ENVIN_OK means the same thing to the caller — show the payload as it
 * arrived — and they are distinguished so that GET /rns/status can say which
 * shape keeps turning up. */
static inline const char *rns_envin_reason(rns_envin_result r) {
    switch (r) {
        case RNS_ENVIN_OK:          return "ok";
        case RNS_ENVIN_NO_FRAME:    return "not an envelope: plain text";
        case RNS_ENVIN_BAD_VERSION: return "unknown envelope version";
        case RNS_ENVIN_BAD_FROM:    return "sender address is not 32 hex";
        case RNS_ENVIN_BAD_SESSION: return "bad session name";
        case RNS_ENVIN_EMPTY_TEXT:  return "envelope with no text";
        case RNS_ENVIN_TOO_LONG:    return "longer than one packet carries";
    }
    return "unknown";
}

/* Where the three fields are, as pointers and lengths into the caller's
 * payload. Valid only when rns_envelope_parse() returned RNS_ENVIN_OK; zeroed
 * on every other outcome, so a caller that ignores the return value reads
 * nulls rather than stale pointers from the message before. */
typedef struct {
    const char *from;      /* 32 hex characters, no terminator */
    size_t from_len;
    const char *session;   /* may be EMPTY, which is a documented value */
    size_t session_len;
    const char *text;      /* the remainder, verbatim, separators and all */
    size_t text_len;
} rns_envelope_view;

/*
 * Read `1|<from>|<session>|<text>` out of a payload, or say it is not one.
 *
 * SPLIT ON THE FIRST THREE SEPARATORS ONLY. The text is everything after the
 * third '|', byte for byte, and a '|' inside it is an ordinary character — the
 * scan below stops the moment it has three, which is what makes that true.
 * A parser that split on every '|' would be correct for every message anybody
 * tests by hand and wrong for the first one that quotes a shell pipeline, and
 * it would be wrong by CORRUPTING the message rather than by refusing it.
 *
 * The payload is arbitrary bytes and may contain NUL, so nothing here uses a
 * string function on it: the length bounds every read, and the sanitiser in
 * rns/inbox.h is still what makes the text safe to put on a screen. This
 * function decides where the text IS; it does not decide that the text is
 * clean, and it must never be read as having done so.
 */
static inline rns_envin_result rns_envelope_parse(const uint8_t *payload,
                                                  size_t len,
                                                  rns_envelope_view *out) {
    const char *p = (const char *)payload;
    size_t sep[3];
    size_t found = 0;
    size_t slen, tlen;

    if (out) memset(out, 0, sizeof(*out));
    if (!p || len == 0) return RNS_ENVIN_NO_FRAME;
    /* Bounded before anything is indexed. A payload past the ceiling cannot
     * have come through one encrypted packet, so its shape proves nothing. */
    if (len > (size_t)RNS_OUTBOX_PAYLOAD_MAX) return RNS_ENVIN_TOO_LONG;

    for (size_t i = 0; i < len && found < 3; i++)
        if (p[i] == '|') sep[found++] = i;
    if (found < 3) return RNS_ENVIN_NO_FRAME;

    /* The version is ONE byte, so the first separator is at index 1 and a
     * longer first field is not a version we can read. */
    if (sep[0] != 1 || p[0] != RNS_ENVELOPE_VERSION) return RNS_ENVIN_BAD_VERSION;

    if (sep[1] - sep[0] - 1 != (size_t)RNS_OUTBOX_ADDR_HEX ||
        !rns_hex_n(p + sep[0] + 1, (size_t)RNS_OUTBOX_ADDR_HEX))
        return RNS_ENVIN_BAD_FROM;

    slen = sep[2] - sep[1] - 1;
    if (!rns_session_valid_n(p + sep[1] + 1, slen)) return RNS_ENVIN_BAD_SESSION;

    tlen = len - sep[2] - 1;
    if (tlen == 0) return RNS_ENVIN_EMPTY_TEXT;
    /* Redundant while len is bounded above — the arithmetic cannot produce a
     * text over budget — and kept because it is the same ceiling the builder
     * refuses at, and because "the caller passed a longer len than it should
     * have" is not a case worth discovering downstream in a fixed buffer. */
    if (tlen > rns_envelope_text_budget_n(slen)) return RNS_ENVIN_TOO_LONG;

    if (out) {
        out->from = p + sep[0] + 1;
        out->from_len = (size_t)RNS_OUTBOX_ADDR_HEX;
        out->session = p + sep[1] + 1;
        out->session_len = slen;
        out->text = p + sep[2] + 1;
        out->text_len = tlen;
    }
    return RNS_ENVIN_OK;
}

/*
 * ---- WHERE A PARSED ENVELOPE GOES NEXT -----------------------------------
 *
 * A ROOM ROUTER IS A FUNCTION POINTER AND NOT A DIRECT CALL, and the reason is
 * ownership rather than taste. Placing a message into a conversation belongs to
 * the skill that owns the conversations; that skill's helper DOES NOT EXIST IN
 * THIS TREE YET, and a direct call to a name nobody has written is a build
 * break dressed up as a design. So the receive path calls through a pointer
 * that is null until somebody sets it, and a null pointer is not a degraded
 * mode: it is exactly the behaviour that shipped before this hook existed —
 * the payload becomes a card and nothing else happens. The two sides can
 * therefore land in either order, and neither has to know the other's
 * internals.
 *
 * IT RUNS ON THE LOOP TASK, after rns_stack.loop(), from the same pickup that
 * raises the card. That is the whole contract and it is a tight one:
 *
 *   - NO SYNCHRONOUS SD, no SPIFFS, no filesystem of any kind. The loop task
 *     is what drains the Reticulum socket; a card that waits on a card reader
 *     stops packets arriving. The agreed split is RAM first, persist off-loop
 *     — the same shape skills/history.cpp already uses for its archive.
 *   - NO BLOCKING of any other sort: no delay(), no network, no waiting on
 *     another task.
 *   - It is called ONCE per parsed envelope, and never for a payload that was
 *     not an envelope.
 *
 * `session` and `text` are NUL-terminated C strings the caller owns for the
 * duration of the call and reuses afterwards: copy what you keep. `text` has
 * already been through rns/inbox.h's sanitiser, so it is printable and valid
 * UTF-8; it is the message, not the envelope.
 *
 * RETURNING false MUST BE VISIBLE. It means the message did not reach a room,
 * which is a message the user cannot see anywhere else, so the caller counts
 * it and publishes the reason. `reason` is an int the router writes for that
 * purpose — its meaning belongs to the router, which is why this file does not
 * enumerate it — and it must be written on every false return.
 *
 * The setter is defined in skills/rns.cpp, which owns the pointer; it is
 * declared here because this header is the contract both sides include.
 */
typedef bool (*rns_room_router)(const char *session, const char *text,
                                int *reason);

void rns_set_room_router(rns_room_router fn);

/*
 * The queue. The firmware owns one at file scope — fixed size, sized once,
 * never allocated — and passes it to every function below.
 *
 * wr and rd are MONOTONIC message counts, not indices: the slot is the counter
 * modulo RNS_OUTBOX_SLOTS and the depth is wr - rd. Each has exactly one
 * writer, which is what makes this safe across two tasks; see THE MEMORY
 * ORDERING HERE IS LOAD-BEARING at the top of this file for the pairing.
 *
 * `refused` counts offers the ring had no room for. Both its writer and its
 * reader are the AsyncTCP task — it is incremented in POST /rns/send and served
 * by GET /rns/status, which is the same task — so it is the one counter here
 * that crosses nothing and needs no snapshot. It is a plain uint32_t for that
 * reason and not by omission.
 */
typedef struct {
    uint8_t buf[RNS_OUTBOX_SLOTS][RNS_OUTBOX_PAYLOAD_MAX];
    uint16_t len[RNS_OUTBOX_SLOTS];
    char to[RNS_OUTBOX_SLOTS][RNS_OUTBOX_ADDR_HEX + 1];
    std::atomic<uint32_t> wr;  /* messages stored;  producer only */
    std::atomic<uint32_t> rd;  /* messages taken;   consumer only */
    uint32_t refused;          /* offers the ring was too full to hold */
} rns_outbox;

static inline void rns_outbox_init(rns_outbox *bx) {
    if (!bx) return;
    memset(bx->buf, 0, sizeof(bx->buf));
    memset(bx->len, 0, sizeof(bx->len));
    memset(bx->to, 0, sizeof(bx->to));
    bx->refused = 0;
    bx->wr.store(0, std::memory_order_relaxed);
    bx->rd.store(0, std::memory_order_relaxed);
}

/* Messages waiting, from either side. Unsigned subtraction, so it stays correct
 * when the counters wrap. */
static inline uint32_t rns_outbox_depth(const rns_outbox *bx) {
    if (!bx) return 0;
    return bx->wr.load(std::memory_order_acquire) -
           bx->rd.load(std::memory_order_acquire);
}

/*
 * Producer side, called from the HTTP handler on the AsyncTCP task. Everything
 * it does is two memcpys and a handful of scalar stores: no allocation, no
 * clock, no logging, nothing that can throw and nothing that touches the
 * Reticulum stack — the whole point of the queue is that the AsyncTCP task
 * never reaches Transport.
 *
 * `to` is the 32-hex destination the caller posted; it is copied because the
 * caller's JSON document is freed when the handler returns. `payload` is the
 * built envelope, likewise copied.
 *
 * IT RE-VALIDATES `to` RATHER THAN TRUSTING IT. The one call site validates it
 * first, so this is redundant today — and it is here anyway, because the copy
 * below reads exactly RNS_OUTBOX_ADDR_HEX bytes and a shorter string would read
 * past its terminator. Every other function in this header checks what it is
 * handed; the one that memcpys a fixed length out of a caller's pointer is a
 * strange place to start making exceptions.
 *
 * Returns false when the ring is full, and counts that refusal. A malformed
 * argument is refused WITHOUT counting: `refused` means "the queue was full",
 * which is a rate signal, and folding a caller bug into it would make that
 * number mean two things at once.
 */
static inline bool rns_outbox_put(rns_outbox *bx, const char *to,
                                  const uint8_t *payload, size_t len) {
    uint32_t wr, rd;
    uint8_t slot;

    if (!bx || !payload || len == 0) return false;
    if (len > (size_t)RNS_OUTBOX_PAYLOAD_MAX) return false;
    if (!rns_addr_valid(to)) return false;

    /* RELAXED for our own counter — this task is its only writer, so there is
     * nothing to synchronise with. ACQUIRE for the consumer's: it pairs with
     * the release store at the end of rns_outbox_take(), and it is what makes
     * "the slot is free" mean the consumer has finished reading it. Relaxing
     * this one is a mutation the host test cannot see; tools/test_task_unblock
     * .py pins this exact statement for that reason. */
    wr = bx->wr.load(std::memory_order_relaxed);
    rd = bx->rd.load(std::memory_order_acquire);
    if ((uint32_t)(wr - rd) >= (uint32_t)RNS_OUTBOX_SLOTS) {
        bx->refused++;
        return false;
    }

    slot = (uint8_t)(wr % (uint32_t)RNS_OUTBOX_SLOTS);
    memcpy(bx->buf[slot], payload, len);
    bx->len[slot] = (uint16_t)len;
    memcpy(bx->to[slot], to, (size_t)RNS_OUTBOX_ADDR_HEX);
    bx->to[slot][RNS_OUTBOX_ADDR_HEX] = '\0';
    /* Published LAST, with a RELEASE store: this must remain the final
     * statement of this function. Unlike rns_inbox_put()'s counterpart it is
     * not defensive — the consumer is another task, and a payload store that
     * floats past this one is a slot transmitted while it was being filled.
     * A source-text pin in tools/test_task_unblock.py enforces the position,
     * because a single-threaded host test cannot. */
    bx->wr.store(wr + 1, std::memory_order_release);
    return true;
}

/*
 * Consumer side, called from the loop task. Copies the oldest pending message
 * out and frees its slot; returns false when nothing is waiting.
 *
 * A `cap` smaller than the stored length is a caller bug rather than a runtime
 * condition — the firmware passes a buffer of RNS_OUTBOX_PAYLOAD_MAX — so the
 * copy is clamped and the reported length is what was actually written.
 */
static inline bool rns_outbox_take(rns_outbox *bx, char *to_out, size_t to_cap,
                                   uint8_t *dst, size_t cap,
                                   uint16_t *len_out) {
    uint32_t wr, rd;
    uint8_t slot;
    size_t n;

    if (len_out) *len_out = 0;
    if (to_out && to_cap > 0) to_out[0] = '\0';
    if (!bx) return false;

    /* THE ACQUIRE ON THIS LINE IS THE WHOLE POINT OF THE FILE. It pairs with
     * the release store at the end of rns_outbox_put(), and every read of
     * buf[slot], len[slot] and to[slot] below depends on it: relaxed, the
     * compiler and the core are both free to hoist those reads above this load,
     * and what comes out is a slot the producer was still filling. That is the
     * torn message this header opens by promising to prevent, and no
     * single-threaded host test can fail on it — so the statement is pinned by
     * source text in tools/test_task_unblock.py. RELAXED on rd is correct for
     * the same reason it is in put(): this task is its only writer. */
    rd = bx->rd.load(std::memory_order_relaxed);
    wr = bx->wr.load(std::memory_order_acquire);
    if (wr == rd) return false;

    slot = (uint8_t)(rd % (uint32_t)RNS_OUTBOX_SLOTS);
    n = bx->len[slot];
    if (n > cap) n = cap;
    if (n > 0 && dst) memcpy(dst, bx->buf[slot], n);
    if (len_out) *len_out = (uint16_t)n;
    if (to_out && to_cap > 0) {
        size_t tn = strlen(bx->to[slot]);
        if (tn > to_cap - 1) tn = to_cap - 1;
        memcpy(to_out, bx->to[slot], tn);
        to_out[tn] = '\0';
    }

    bx->len[slot] = 0;
    /* Released LAST, for the symmetric reason put() publishes last: until this
     * store the producer must not believe the slot is free to overwrite. */
    bx->rd.store(rd + 1, std::memory_order_release);
    return true;
}

/*
 * What the loop task should do about the message it is holding.
 *
 * THE FIRST ATTEMPT IS IMMEDIATE, which is why `attempts` and not a "started
 * at" stamp is the input: a message queued while the path is already known must
 * go out on the tick it was picked up, not RNS_OUTBOX_RETRY_MS later. After
 * that the throttle applies, measured from the last attempt.
 *
 * THE LAST ATTEMPT IS A PROBE WITH NO WAITING PERIOD AFTER IT, and that is a
 * decision rather than an ordering accident: the cap is tested BEFORE the
 * throttle, so the tick after the seventh attempt retires the message instead
 * of holding it for one more RNS_OUTBOX_RETRY_MS. The rungs are therefore at
 * 0, 10, 20, 30, 40, 50 and 60 seconds and the bound is
 * (RNS_OUTBOX_ATTEMPTS_MAX - 1) * RNS_OUTBOX_RETRY_MS = 60 s, not
 * ATTEMPTS_MAX * RETRY_MS.
 *
 * The alternative — throttle first, so every attempt including the last gets a
 * full dwell — was considered and rejected. A dwell after the LAST probe is a
 * dwell nothing looks at: no rung follows it to notice a path that arrived
 * during it, so all it buys is ten more seconds of a queue slot held by a
 * message already decided. A path landing just after the final probe is missed
 * either way; what changes it is RNS_OUTBOX_ATTEMPTS_MAX, and the caller learns
 * to send again from send_error rather than from a longer silence.
 *
 * GIVE_UP is not an error condition — it is the OUTCOME the whole file exists
 * to produce. A send to a peer we cannot resolve does not fail on this library:
 * Transport::outbound() takes the broadcast branch when there is no path and
 * emits a HEADER_1 packet the hub drops without a word, so the send "succeeds"
 * and the message is simply gone. Bounding the attempts is what converts that
 * silence into a failure with a reason and a counter.
 *
 * WRAPAROUND. The comparison is an unsigned subtraction of two uint32_t
 * millisecond stamps, correct across the ~49.7-day millis() rollover. Do not
 * rewrite it as `now >= last + interval`: that form goes permanently true or
 * permanently false for one rollover's worth of time, and it is the bug this
 * note exists to prevent. tools/test_rns_outbox.sh pins the behaviour near
 * 2^32.
 */
typedef enum {
    RNS_OUTBOX_GO,      /* attempt to resolve and send, now */
    RNS_OUTBOX_WAIT,    /* the throttle has not expired; come back next tick */
    RNS_OUTBOX_GIVE_UP  /* the attempts are spent; fail the message with a reason */
} rns_outbox_action;

/*
 * now_ms        millis() now
 * last_try_ms   millis() at the previous attempt; ignored when attempts == 0
 * attempts      attempts already made for THIS message
 */
static inline rns_outbox_action rns_outbox_next(uint32_t now_ms,
                                                uint32_t last_try_ms,
                                                uint8_t attempts) {
    if (attempts == 0) return RNS_OUTBOX_GO;
    if (attempts >= (uint8_t)RNS_OUTBOX_ATTEMPTS_MAX) return RNS_OUTBOX_GIVE_UP;
    if ((uint32_t)(now_ms - last_try_ms) >= RNS_OUTBOX_RETRY_MS)
        return RNS_OUTBOX_GO;
    return RNS_OUTBOX_WAIT;
}
