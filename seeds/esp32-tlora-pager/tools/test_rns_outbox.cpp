/*
 * Host tests for the outbound queue, the wire envelope and the retry decision.
 *
 * rns/outbox.h is the seam between an HTTP handler that may not touch the
 * Reticulum stack and a loop task that spends public-key time on every packet,
 * and every rule it holds is a rule about a byte budget or a clock: an envelope
 * that must be at most 383 bytes because that is what one encrypted packet
 * carries, a text field that must survive containing the separator, a session
 * name that may be empty, and a retry ladder that has to stay correct across
 * the millis() rollover. None of that can be exercised on the device without a
 * peer, a socket and a flash; all of it compiles on the host.
 *
 * THE BOUNDARY IS THE POINT of sections 1-4. Nothing in microReticulum checks
 * the plaintext size before encrypting — Packet::pack() throws std::length_error
 * on the PACKED size, after Destination::encrypt() has already generated an
 * ephemeral X25519 keypair and run the exchange. So the only thing standing
 * between a caller and a tenth of a second of loop task spent to produce an
 * exception is the arithmetic in rns_envelope_build(), and an off-by-one in it
 * is invisible everywhere else.
 *
 * SECTION 5 IS THE SEPARATOR RULE, and it is the case a hand-written test
 * skips. The envelope is parsed by its FIRST THREE '|' characters and the text
 * is the whole remainder, unescaped — so a message that quotes a shell pipeline
 * must come back byte for byte. A builder that escaped, quoted or rejected the
 * separator would pass every other test in this file.
 *
 * SECTION 9 IS THE WRAPAROUND. Both the ring's monotonic counters and the retry
 * throttle are unsigned differences, and both are pinned near 2^32 because the
 * `now >= last + interval` form that fails there is the one a reader is most
 * likely to "simplify" them into.
 *
 * SECTIONS 12-17 ARE THE OTHER HALF OF THE FORMAT: the parser the receive path
 * calls, which lives in the same header as the builder so that the two cannot
 * drift. SECTION 12 IS THE PROPERTY THAT ENFORCES THAT — anything
 * rns_envelope_build() emits must come back out of rns_envelope_parse() as
 * exactly the three inputs, over a corpus chosen for the cases a hand-written
 * round trip skips. The rest are the hostile ones: a payload arrives from the
 * network before anything has authenticated it, so every field is validated,
 * and NOTHING IS EVER DROPPED — a payload that is not an envelope is a normal
 * outcome with its own reason code, because bare-text senders exist and a
 * message is never worth losing over its shape.
 *
 * SECTION 18 IS THE ONE PLACE THE TWO HALVES ARE DELIBERATELY DIFFERENT. A
 * session of exactly '*' marks a control frame — the peer naming its live rooms
 * — and the receive side accepts it while the builder still refuses it, so this
 * device can read one and can never emit one. Both directions are asserted on
 * the same value, because a single loosened validator satisfies every parse
 * assertion in the section and silently deletes the guarantee.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/rns/outbox.h"

static const char *ADDR = "6f796a5113a5a7f34512d2ede3723f40";
static const char *SELF = "0123456789abcdef0123456789abcdef";

/* Build a text of n bytes with a position-dependent pattern, so a copy that is
 * short, long, offset or stale fails on content and not only on length. */
static void filltext(char *buf, size_t n) {
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)('a' + (int)((i * 7u + (i >> 3)) % 26u));
    buf[n] = '\0';
}

/*
 * Split an envelope the way the peer does: on the FIRST THREE separators, with
 * the text as everything after the third.
 *
 * THE DEVICE DOES PARSE NOW — rns_envelope_parse() is in the same header as the
 * builder — and this stays anyway, as an INDEPENDENT ORACLE. Section 12 checks
 * the two halves of the header against each other, which is the property that
 * matters and also the one a shared mistake passes: a builder and a parser that
 * agreed on the wrong offset would round-trip perfectly. This function is
 * written from the agreed rule and not from either of them, so the shape
 * assertions in sections 1-5 fail when both halves drift together.
 */
static bool split3(const uint8_t *env, size_t len, char *ver, char *from,
                   char *session, char *text, size_t *text_len) {
    size_t sep[3];
    size_t found = 0;
    for (size_t i = 0; i < len && found < 3; i++)
        if (env[i] == '|') sep[found++] = i;
    if (found != 3) return false;

    memcpy(ver, env, sep[0]);
    ver[sep[0]] = '\0';
    memcpy(from, env + sep[0] + 1, sep[1] - sep[0] - 1);
    from[sep[1] - sep[0] - 1] = '\0';
    memcpy(session, env + sep[1] + 1, sep[2] - sep[1] - 1);
    session[sep[2] - sep[1] - 1] = '\0';
    *text_len = len - sep[2] - 1;
    memcpy(text, env + sep[2] + 1, *text_len);
    text[*text_len] = '\0';
    return true;
}

int main() {
    uint8_t env[RNS_OUTBOX_PAYLOAD_MAX];
    uint16_t len = 0;
    char ver[64], from[64], sess[64], text[512];
    size_t tlen = 0;

    /* ---- 1. the shape, with an empty session --------------------------------
     * "1|<32 hex>||<text>". The empty session is a DOCUMENTED value, not a
     * degenerate one: the peer routes it to its newest session, so the two
     * adjacent separators have to survive the builder. */
    {
        assert(rns_envelope_build(SELF, "", "hello", env, sizeof(env), &len) ==
               RNS_ENV_OK);
        assert(len == RNS_OUTBOX_ENVELOPE_OVERHEAD + 5);
        assert(memcmp(env, "1|", 2) == 0);
        assert(memcmp(env + 2, SELF, 32) == 0);
        assert(env[34] == '|' && env[35] == '|');
        assert(memcmp(env + 36, "hello", 5) == 0);
        assert(split3(env, len, ver, from, sess, text, &tlen));
        assert(strcmp(ver, "1") == 0);
        assert(strcmp(from, SELF) == 0);
        assert(strcmp(sess, "") == 0);
        assert(strcmp(text, "hello") == 0);
    }

    /* The overhead is 36 bytes and that number is arithmetic, not a constant
     * somebody typed: version + separator + address + separator + separator. */
    assert(RNS_OUTBOX_ENVELOPE_OVERHEAD == 1 + 1 + RNS_OUTBOX_ADDR_HEX + 1 + 1);
    assert(RNS_OUTBOX_TEXT_MAX == 347);

    /* ---- 2. THE EXACT BOUNDARY, empty session ------------------------------
     * 347 bytes of text fills the packet to 383 and must be accepted; 348 must
     * be refused. This is the assertion the library cannot make for us: it
     * would encrypt first and throw afterwards. */
    {
        char big[RNS_OUTBOX_TEXT_MAX + 8];
        filltext(big, RNS_OUTBOX_TEXT_MAX);
        assert(rns_envelope_build(SELF, "", big, env, sizeof(env), &len) ==
               RNS_ENV_OK);
        assert(len == RNS_OUTBOX_PAYLOAD_MAX);
        assert(memcmp(env + RNS_OUTBOX_ENVELOPE_OVERHEAD, big,
                      RNS_OUTBOX_TEXT_MAX) == 0);

        filltext(big, RNS_OUTBOX_TEXT_MAX + 1);
        len = 0xFFFF;
        assert(rns_envelope_build(SELF, "", big, env, sizeof(env), &len) ==
               RNS_ENV_TOO_LONG);
        assert(len == 0);   /* a refusal must not report a length */
    }

    /* ---- 3. the session costs its own length -------------------------------
     * A 23-byte name is the longest the peer accepts, and it takes 23 bytes off
     * the text budget: 347 - 23 = 324 fits and 325 does not. A budget computed
     * from the constant alone rather than from strlen(session) passes section 2
     * and fails here, which is why both exist. */
    {
        char sname[RNS_OUTBOX_SESSION_MAX + 2];
        char big[RNS_OUTBOX_TEXT_MAX + 8];
        memset(sname, 'a', RNS_OUTBOX_SESSION_MAX);
        sname[RNS_OUTBOX_SESSION_MAX] = '\0';
        assert(strlen(sname) == 23);
        assert(rns_envelope_text_budget(sname) == 324);

        filltext(big, 324);
        assert(rns_envelope_build(SELF, sname, big, env, sizeof(env), &len) ==
               RNS_ENV_OK);
        assert(len == RNS_OUTBOX_PAYLOAD_MAX);
        assert(split3(env, len, ver, from, sess, text, &tlen));
        assert(strcmp(sess, sname) == 0);
        assert(tlen == 324 && strcmp(text, big) == 0);

        filltext(big, 325);
        assert(rns_envelope_build(SELF, sname, big, env, sizeof(env), &len) ==
               RNS_ENV_TOO_LONG);

        /* 24 characters is one too many for the name itself. */
        sname[RNS_OUTBOX_SESSION_MAX] = 'a';
        sname[RNS_OUTBOX_SESSION_MAX + 1] = '\0';
        assert(!rns_session_valid(sname));
        assert(rns_envelope_build(SELF, sname, "hi", env, sizeof(env), &len) ==
               RNS_ENV_BAD_SESSION);
    }

    /* ---- 4. the refusals, each with its own reason -------------------------- */
    {
        assert(rns_envelope_build("nothex", "", "hi", env, sizeof(env), &len) ==
               RNS_ENV_BAD_FROM);
        /* 31 and 33 characters: a length check that only tested "at least 32"
         * would accept the long one and send to a different address. */
        assert(!rns_addr_valid("6f796a5113a5a7f34512d2ede3723f4"));
        assert(!rns_addr_valid("6f796a5113a5a7f34512d2ede3723f400"));
        assert(rns_addr_valid(ADDR));
        assert(rns_addr_valid("6F796A5113A5A7F34512D2EDE3723F40"));  /* upper */
        assert(!rns_addr_valid("6f796a5113a5a7f34512d2ede3723g40")); /* 'g' */
        assert(!rns_addr_valid(NULL));

        assert(rns_envelope_build(SELF, "", "", env, sizeof(env), &len) ==
               RNS_ENV_EMPTY_TEXT);
        assert(rns_envelope_build(SELF, "", NULL, env, sizeof(env), &len) ==
               RNS_ENV_EMPTY_TEXT);
        /* A separator in the SESSION would forge a field boundary; the charset
         * is what forbids it. */
        assert(!rns_session_valid("a|b"));
        assert(!rns_session_valid("with space"));
        assert(!rns_session_valid("\n"));
        assert(rns_session_valid(""));
        assert(rns_session_valid("main.2_a-B9"));
        assert(rns_envelope_build(SELF, "a|b", "hi", env, sizeof(env), &len) ==
               RNS_ENV_BAD_SESSION);

        /* A buffer smaller than the ceiling is refused, never truncated: a cut
         * envelope still parses, and would deliver a cut message as a whole
         * one. */
        assert(rns_envelope_build(SELF, "", "hi", env,
                                  RNS_OUTBOX_PAYLOAD_MAX - 1, &len) ==
               RNS_ENV_NO_ROOM);
        assert(rns_envelope_build(SELF, "", "hi", NULL, sizeof(env), &len) ==
               RNS_ENV_NO_ROOM);

        /* Every reason has a distinct, non-empty literal for /rns/status. */
        assert(strcmp(rns_env_reason(RNS_ENV_TOO_LONG),
                      rns_env_reason(RNS_ENV_EMPTY_TEXT)) != 0);
        assert(rns_env_reason(RNS_ENV_BAD_FROM)[0] != '\0');
    }

    /* ---- 5. A '|' INSIDE THE TEXT MUST NOT BREAK THE FRAME ------------------
     * The rule is "first three separators", so the text is appended verbatim
     * and the peer's split recovers it byte for byte however many separators it
     * contains — including a leading and a trailing one. */
    {
        const char *piped = "|run a | b | c| and then||done|";
        assert(rns_envelope_build(SELF, "s1", piped, env, sizeof(env), &len) ==
               RNS_ENV_OK);
        assert(split3(env, len, ver, from, sess, text, &tlen));
        assert(strcmp(sess, "s1") == 0);
        assert(tlen == strlen(piped));
        assert(memcmp(text, piped, tlen) == 0);
        /* And the separator count proves nothing was escaped away. */
        size_t bars = 0;
        for (uint16_t i = 0; i < len; i++)
            if (env[i] == '|') bars++;
        assert(bars == 3 + 7);
    }

    /* ---- 6. the ring: FIFO, by content -------------------------------------- */
    {
        rns_outbox bx;
        uint8_t out[RNS_OUTBOX_PAYLOAD_MAX];
        char to[RNS_OUTBOX_ADDR_HEX + 1];
        rns_outbox_init(&bx);

        assert(rns_outbox_depth(&bx) == 0);
        assert(!rns_outbox_take(&bx, to, sizeof(to), out, sizeof(out), &len));
        assert(len == 0 && to[0] == '\0');

        for (int i = 0; i < 3; i++) {
            char body[16];
            snprintf(body, sizeof(body), "msg%d", i);
            assert(rns_envelope_build(SELF, "", body, env, sizeof(env), &len) ==
                   RNS_ENV_OK);
            assert(rns_outbox_put(&bx, ADDR, env, len));
        }
        assert(rns_outbox_depth(&bx) == 3);

        for (int i = 0; i < 3; i++) {
            char body[16];
            snprintf(body, sizeof(body), "msg%d", i);
            assert(rns_outbox_take(&bx, to, sizeof(to), out, sizeof(out), &len));
            assert(strcmp(to, ADDR) == 0);
            assert(split3(out, len, ver, from, sess, text, &tlen));
            assert(strcmp(text, body) == 0);
        }
        assert(rns_outbox_depth(&bx) == 0);
        assert(bx.refused == 0);
    }

    /* ---- 7. OVERFLOW: KEEP THE OLDEST, COUNT THE REFUSAL --------------------
     * The queued messages must still come out intact and in order, and the
     * refusal must still be visible — a silently dropped message is the one
     * outcome that is not acceptable, and it is exactly what a missing
     * `refused++` looks like from the outside. Here the sender learns about it
     * synchronously as well, because put() runs inside the POST. */
    {
        rns_outbox bx;
        uint8_t out[RNS_OUTBOX_PAYLOAD_MAX];
        char to[RNS_OUTBOX_ADDR_HEX + 1];
        rns_outbox_init(&bx);

        for (int i = 0; i < RNS_OUTBOX_SLOTS; i++) {
            char body[16];
            snprintf(body, sizeof(body), "q%d", i);
            assert(rns_envelope_build(SELF, "", body, env, sizeof(env), &len) ==
                   RNS_ENV_OK);
            assert(rns_outbox_put(&bx, ADDR, env, len));
        }
        assert(rns_outbox_depth(&bx) == RNS_OUTBOX_SLOTS);

        uint8_t over[RNS_OUTBOX_PAYLOAD_MAX];
        uint16_t over_len = 0;
        assert(rns_envelope_build(SELF, "", "overflow", over, sizeof(over),
                                  &over_len) == RNS_ENV_OK);
        assert(!rns_outbox_put(&bx, ADDR, over, over_len));
        assert(bx.refused == 1);
        assert(rns_outbox_depth(&bx) == RNS_OUTBOX_SLOTS);

        /* The oldest is still the oldest, and the refused one never entered. */
        assert(rns_outbox_take(&bx, to, sizeof(to), out, sizeof(out), &len));
        assert(split3(out, len, ver, from, sess, text, &tlen));
        assert(strcmp(text, "q0") == 0);
        /* One slot free, one more accepted, and the ring keeps its order. */
        assert(rns_outbox_put(&bx, ADDR, over, over_len));
        assert(bx.refused == 1);
        for (int i = 1; i < RNS_OUTBOX_SLOTS; i++) {
            char body[16];
            snprintf(body, sizeof(body), "q%d", i);
            assert(rns_outbox_take(&bx, to, sizeof(to), out, sizeof(out), &len));
            assert(split3(out, len, ver, from, sess, text, &tlen));
            assert(strcmp(text, body) == 0);
        }
        assert(rns_outbox_take(&bx, to, sizeof(to), out, sizeof(out), &len));
        assert(split3(out, len, ver, from, sess, text, &tlen));
        assert(strcmp(text, "overflow") == 0);
        assert(rns_outbox_depth(&bx) == 0);
    }

    /* ---- 8. a full-size payload survives the ring intact -------------------- */
    {
        rns_outbox bx;
        uint8_t out[RNS_OUTBOX_PAYLOAD_MAX];
        char to[RNS_OUTBOX_ADDR_HEX + 1];
        char big[RNS_OUTBOX_TEXT_MAX + 8];
        rns_outbox_init(&bx);

        filltext(big, RNS_OUTBOX_TEXT_MAX);
        assert(rns_envelope_build(SELF, "", big, env, sizeof(env), &len) ==
               RNS_ENV_OK);
        assert(len == RNS_OUTBOX_PAYLOAD_MAX);
        assert(rns_outbox_put(&bx, ADDR, env, len));
        assert(rns_outbox_take(&bx, to, sizeof(to), out, sizeof(out), &len));
        assert(len == RNS_OUTBOX_PAYLOAD_MAX);
        assert(memcmp(out, env, RNS_OUTBOX_PAYLOAD_MAX) == 0);
        /* Nothing longer than the ceiling can enter the ring at all. */
        assert(!rns_outbox_put(&bx, ADDR, env, RNS_OUTBOX_PAYLOAD_MAX + 1));
        assert(!rns_outbox_put(&bx, ADDR, env, 0));

        /* AND NOTHING WITH A MALFORMED ADDRESS. put() copies exactly
         * RNS_OUTBOX_ADDR_HEX bytes out of `to`, so a shorter string would be
         * read past its terminator — the one place in this header where
         * trusting the caller is a read overrun and not merely a bad value.
         * The refusal is NOT counted: `refused` means the queue was full. */
        assert(rns_envelope_build(SELF, "", "hi", env, sizeof(env), &len) ==
               RNS_ENV_OK);
        assert(!rns_outbox_put(&bx, "6f796a51", env, len));       /* short */
        assert(!rns_outbox_put(&bx, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", env, len));
        assert(!rns_outbox_put(&bx, NULL, env, len));
        assert(bx.refused == 0);
        assert(rns_outbox_depth(&bx) == 0);
        assert(rns_outbox_put(&bx, ADDR, env, len));              /* still works */
    }

    /* ---- 8-bis. THE SLOT COUNT MUST BE A POWER OF TWO -----------------------
     * The header static_asserts it, so this section cannot fail at runtime —
     * it exists to state WHY, next to the wrap test below that depends on it.
     * The slot is the monotonic counter modulo RNS_OUTBOX_SLOTS, and that is
     * only continuous across the 2^32 wrap when the depth divides 2^32. At five
     * slots, wr = 0xFFFFFFFF and wr = 0 are the same slot: one queued message
     * is overwritten unread and another is delivered twice, while the depth
     * reads a perfectly innocent 2. */
    {
        static_assert((RNS_OUTBOX_SLOTS & (RNS_OUTBOX_SLOTS - 1)) == 0,
                      "the slot index is the counter modulo the depth");
        assert(0xFFFFFFFFu % (uint32_t)RNS_OUTBOX_SLOTS !=
               0u % (uint32_t)RNS_OUTBOX_SLOTS);
    }

    /* ---- 9. THE COUNTERS WRAP AND THE DEPTH STAYS RIGHT ---------------------
     * wr and rd are monotonic message counts, so after 2^32 messages they wrap.
     * The depth is their unsigned difference and survives it; an implementation
     * that compared them with `<` would report an empty ring forever after. */
    {
        rns_outbox bx;
        uint8_t out[RNS_OUTBOX_PAYLOAD_MAX];
        char to[RNS_OUTBOX_ADDR_HEX + 1];
        rns_outbox_init(&bx);
        bx.wr.store(0xFFFFFFFEu);
        bx.rd.store(0xFFFFFFFEu);

        assert(rns_outbox_depth(&bx) == 0);
        assert(rns_envelope_build(SELF, "", "before", env, sizeof(env), &len) ==
               RNS_ENV_OK);
        assert(rns_outbox_put(&bx, ADDR, env, len));
        assert(rns_envelope_build(SELF, "", "after", env, sizeof(env), &len) ==
               RNS_ENV_OK);
        assert(rns_outbox_put(&bx, ADDR, env, len));   /* wr wraps to 0 here */
        assert(bx.wr.load() == 0);
        assert(rns_outbox_depth(&bx) == 2);

        assert(rns_outbox_take(&bx, to, sizeof(to), out, sizeof(out), &len));
        assert(split3(out, len, ver, from, sess, text, &tlen));
        assert(strcmp(text, "before") == 0);
        assert(rns_outbox_take(&bx, to, sizeof(to), out, sizeof(out), &len));
        assert(split3(out, len, ver, from, sess, text, &tlen));
        assert(strcmp(text, "after") == 0);
        assert(rns_outbox_depth(&bx) == 0);
    }

    /* ---- 10. the retry decision --------------------------------------------
     * First attempt immediate, throttle honoured to the millisecond, cap
     * enforced. The ladder is ~10 s x 7 attempts, i.e. about a minute before a
     * destination that never resolves is reported as failed. */
    {
        assert(RNS_OUTBOX_RETRY_MS == 10000UL);
        assert(RNS_OUTBOX_ATTEMPTS_MAX == 7);

        /* attempts == 0: go now, whatever the stamps say. A message queued
         * while the path is already known must leave on the tick it was picked
         * up. */
        assert(rns_outbox_next(0, 0, 0) == RNS_OUTBOX_GO);
        assert(rns_outbox_next(1000, 999999, 0) == RNS_OUTBOX_GO);

        /* One attempt made: the throttle, to the millisecond. */
        assert(rns_outbox_next(5000, 5000, 1) == RNS_OUTBOX_WAIT);
        assert(rns_outbox_next(5000 + 9999, 5000, 1) == RNS_OUTBOX_WAIT);
        assert(rns_outbox_next(5000 + 10000, 5000, 1) == RNS_OUTBOX_GO);
        assert(rns_outbox_next(5000 + 10001, 5000, 1) == RNS_OUTBOX_GO);

        /* THE CAP IS CHECKED BEFORE THE THROTTLE, deliberately: the tick after
         * the last attempt retires the message rather than holding a queue slot
         * for one more dwell that no rung would look at. So GIVE_UP wins even
         * when the throttle has not expired — including at now == last, which
         * is the tick immediately after the seventh probe. */
        assert(rns_outbox_next(0, 0, RNS_OUTBOX_ATTEMPTS_MAX) ==
               RNS_OUTBOX_GIVE_UP);
        assert(rns_outbox_next(5020, 5000, RNS_OUTBOX_ATTEMPTS_MAX) ==
               RNS_OUTBOX_GIVE_UP);   /* one 20 ms tick later, not 10 s later */
        assert(rns_outbox_next(999999, 0, RNS_OUTBOX_ATTEMPTS_MAX + 1) ==
               RNS_OUTBOX_GIVE_UP);
        assert(rns_outbox_next(0, 0, RNS_OUTBOX_ATTEMPTS_MAX - 1) !=
               RNS_OUTBOX_GIVE_UP);

        /* Six waits and a seventh attempt is the whole ladder, and the bound is
         * (ATTEMPTS_MAX - 1) * RETRY_MS = 60 s — NOT ATTEMPTS_MAX * RETRY_MS,
         * because the first attempt is immediate and the last one is not
         * followed by a dwell. */
        assert((RNS_OUTBOX_ATTEMPTS_MAX - 1) * RNS_OUTBOX_RETRY_MS == 60000UL);
        {
            uint32_t t = 1000, last = 1000;
            uint8_t attempts = 1;      /* the immediate first one is spent */
            int gos = 0;
            for (; attempts < RNS_OUTBOX_ATTEMPTS_MAX;) {
                t += RNS_OUTBOX_RETRY_MS;
                assert(rns_outbox_next(t, last, attempts) == RNS_OUTBOX_GO);
                last = t;
                attempts++;
                gos++;
            }
            assert(gos == 6);
            assert(t - 1000 == 60000UL);
            assert(rns_outbox_next(t, last, attempts) == RNS_OUTBOX_GIVE_UP);
        }
    }

    /* ---- 11. THE THROTTLE ACROSS THE millis() ROLLOVER ----------------------
     * `now >= last + interval` is the form that breaks here: at last =
     * 0xFFFFFF00 the sum wraps and the comparison goes permanently true, so a
     * message would be retried every tick for the rest of its ladder. The
     * unsigned difference is right. */
    {
        const uint32_t last = 0xFFFFFF00u;   /* 256 ms before the rollover */
        assert(rns_outbox_next(last, last, 1) == RNS_OUTBOX_WAIT);
        assert(rns_outbox_next(0x00000000u, last, 1) == RNS_OUTBOX_WAIT);
        /* last + 10000 lands 9744 ms past the wrap: 256 ms of the interval was
         * spent before it and 9744 after. */
        assert(rns_outbox_next(9744u - 1u, last, 1) == RNS_OUTBOX_WAIT);
        assert(rns_outbox_next(9744u, last, 1) == RNS_OUTBOX_GO);
        assert(rns_outbox_next(9745u, last, 1) == RNS_OUTBOX_GO);
    }

    /* ---- 12. THE ROUND TRIP: WHAT THE BUILDER EMITS, THE PARSER READS -------
     * This is the property that stops the two halves of the format drifting,
     * and it is the reason both live in one header. Anything
     * rns_envelope_build() produces must come back out of
     * rns_envelope_parse() as EXACTLY the three inputs — not merely as three
     * fields, and not merely with the right lengths.
     *
     * The corpus is chosen for the cases a hand-written round trip skips: an
     * empty session, the longest legal one, the separator inside the text, a
     * leading and a trailing separator, one byte of text, and the text that
     * fills the packet to its ceiling. */
    {
        char maxtext[RNS_OUTBOX_TEXT_MAX + 8];
        char s23[RNS_OUTBOX_SESSION_MAX + 2];
        /* The text that fills the packet WITH a 23-byte session: 347 - 23 =
         * 324, so the payload is 383 again by the other route. This is the one
         * case where rns_envelope_text_budget_n() is load-bearing at BOTH ends
         * at once — the builder refusing a byte more and the parser accepting
         * exactly this much — and a budget that used the constant alone instead
         * of the session's length passes every other case in this corpus. */
        char budgettext[RNS_OUTBOX_TEXT_MAX + 8];
        filltext(maxtext, RNS_OUTBOX_TEXT_MAX);
        memset(s23, 'z', RNS_OUTBOX_SESSION_MAX);
        s23[RNS_OUTBOX_SESSION_MAX] = '\0';
        filltext(budgettext, RNS_OUTBOX_TEXT_MAX - RNS_OUTBOX_SESSION_MAX);
        assert(strlen(budgettext) == 324);

        struct { const char *session; const char *text; } cases[] = {
            {"",      "hello"},
            {"",      "x"},
            {"beacon", "Received. Answering only on the device."},
            {"a.b_c-9", "line one\nline two"},
            {s23,     "the longest session name the peer accepts"},
            {s23,     budgettext},
            {"s1",    "|run a | b | c| and then||done|"},
            {"pipe",  "|"},
            {"",      maxtext},
        };

        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            rns_envelope_view v;
            assert(rns_envelope_build(SELF, cases[c].session, cases[c].text,
                                      env, sizeof(env), &len) == RNS_ENV_OK);
            assert(rns_envelope_parse(env, len, &v) == RNS_ENVIN_OK);
            assert(v.from_len == RNS_OUTBOX_ADDR_HEX);
            assert(memcmp(v.from, SELF, v.from_len) == 0);
            assert(v.session_len == strlen(cases[c].session));
            assert(memcmp(v.session, cases[c].session, v.session_len) == 0);
            assert(v.text_len == strlen(cases[c].text));
            assert(memcmp(v.text, cases[c].text, v.text_len) == 0);
            /* The view points INTO the payload rather than at a copy: that is
             * what lets the firmware sanitise the text straight out of the
             * receive buffer with nothing allocated in between. */
            assert(v.from == (const char *)env + 2);
            assert(v.text + v.text_len == (const char *)env + len);
        }

        /* BOTH ROUTES TO 383 BYTES, checked as lengths rather than trusted:
         * an empty session with 347 bytes of text, and a 23-byte session with
         * 324. The second is the packet-filling case the corpus was missing. */
        assert(rns_envelope_build(SELF, s23, budgettext, env, sizeof(env),
                                  &len) == RNS_ENV_OK);
        assert(len == RNS_OUTBOX_PAYLOAD_MAX);
        {
            rns_envelope_view v;
            assert(rns_envelope_parse(env, len, &v) == RNS_ENVIN_OK);
            assert(v.session_len == RNS_OUTBOX_SESSION_MAX);
            assert(v.text_len == RNS_OUTBOX_TEXT_MAX - RNS_OUTBOX_SESSION_MAX);
            assert(memcmp(v.text, budgettext, v.text_len) == 0);
        }
        /* ...and one byte more of text with that session is over the packet. */
        {
            char over[RNS_OUTBOX_TEXT_MAX + 8];
            filltext(over, RNS_OUTBOX_TEXT_MAX - RNS_OUTBOX_SESSION_MAX + 1);
            assert(rns_envelope_build(SELF, s23, over, env, sizeof(env), &len) ==
                   RNS_ENV_TOO_LONG);
        }

        /* The ceiling case above is the whole packet, and it must stay so: a
         * round trip that quietly stopped at 382 bytes would pass every other
         * assertion here. */
        assert(rns_envelope_build(SELF, "", maxtext, env, sizeof(env), &len) ==
               RNS_ENV_OK);
        assert(len == RNS_OUTBOX_PAYLOAD_MAX);
        {
            rns_envelope_view v;
            assert(rns_envelope_parse(env, len, &v) == RNS_ENVIN_OK);
            assert(v.text_len == RNS_OUTBOX_TEXT_MAX);
        }
    }

    /* ---- 13. THE PARSER TAKES THE FIRST THREE SEPARATORS AND NOTHING ELSE ---
     * Same rule as the builder's, from the other side, and the same reason it
     * is stated as "first three": a naive split on every '|' is correct for
     * every message anybody tests by hand and CORRUPTS the first one that
     * quotes a shell pipeline. The payload here is written by hand rather than
     * built, so the parser is tested against the agreed bytes and not against
     * whatever the builder happens to do. */
    {
        rns_envelope_view v;
        const char *raw = "1|6f796a5113a5a7f34512d2ede3723f40|beacon|a|b|c";
        size_t n = strlen(raw);
        assert(rns_envelope_parse((const uint8_t *)raw, n, &v) == RNS_ENVIN_OK);
        assert(memcmp(v.from, ADDR, 32) == 0);
        assert(v.session_len == 6 && memcmp(v.session, "beacon", 6) == 0);
        assert(v.text_len == 5 && memcmp(v.text, "a|b|c", 5) == 0);

        /* An empty session is a DOCUMENTED value: two adjacent separators, and
         * the peer routes it to its newest conversation. */
        const char *nosess = "1|6f796a5113a5a7f34512d2ede3723f40||text";
        assert(rns_envelope_parse((const uint8_t *)nosess, strlen(nosess), &v) ==
               RNS_ENVIN_OK);
        assert(v.session_len == 0);
        assert(v.text_len == 4 && memcmp(v.text, "text", 4) == 0);

        /* NOT NUL-TERMINATED, EVER. The payload is bytes off a wire and the
         * length is the only boundary there is, so the parser is handed a
         * fragment of a longer buffer with more separators and more text
         * behind it: anything that reached for a terminator would swallow
         * them. */
        char buf[64];
        memset(buf, '|', sizeof(buf));
        memcpy(buf, nosess, strlen(nosess));
        assert(rns_envelope_parse((const uint8_t *)buf, strlen(nosess), &v) ==
               RNS_ENVIN_OK);
        assert(v.text_len == 4 && memcmp(v.text, "text", 4) == 0);
    }

    /* ---- 14. WHAT IS NOT AN ENVELOPE, AND WHY THAT IS NOT A FAILURE ---------
     * Every case below is shown to the user exactly as it arrived. The rule the
     * firmware depends on is that the outcome is DISTINGUISHABLE from OK and
     * that the view is left empty, so a caller that ignores the reason still
     * cannot read a field that was never parsed.
     *
     * THE ONE OUTCOME THAT MUST NEVER HAPPEN IS A DROPPED OR BLANKED MESSAGE,
     * so each of these is a payload the caller must still be able to put on the
     * screen whole — which it can, because nothing here consumes it. */
    {
        rns_envelope_view v;
        struct { const char *payload; rns_envin_result want; } bad[] = {
            /* Plain text, no separators at all: tools/rns-send, and every peer
             * that predates the format. */
            {"just a message with no framing at all", RNS_ENVIN_NO_FRAME},
            {"", RNS_ENVIN_NO_FRAME},
            {"|", RNS_ENVIN_NO_FRAME},
            {"1|6f796a5113a5a7f34512d2ede3723f40|beacon", RNS_ENVIN_NO_FRAME},
            /* An UNKNOWN VERSION IS NOT AN ENVELOPE: the fields behind a digit
             * we do not know are not promised to be in this order, so the
             * honest answer is the raw bytes. */
            {"2|6f796a5113a5a7f34512d2ede3723f40|beacon|hi",
             RNS_ENVIN_BAD_VERSION},
            {"x|6f796a5113a5a7f34512d2ede3723f40|beacon|hi",
             RNS_ENVIN_BAD_VERSION},
            /* A version field of more than one byte: the separator is not where
             * a version-1 envelope puts it. */
            {"11|6f796a5113a5a7f34512d2ede3723f40|beacon|hi",
             RNS_ENVIN_BAD_VERSION},
            {"|6f796a5113a5a7f34512d2ede3723f40|beacon|hi",
             RNS_ENVIN_BAD_VERSION},
            /* NON-HEX ADDRESS. It would decode to a different hash and a reply
             * would be encrypted to nobody, so it is refused as a field rather
             * than passed on as a reply target. */
            {"1|6f796a5113a5a7f34512d2ede3723fzz|beacon|hi", RNS_ENVIN_BAD_FROM},
            {"1|6f796a51|beacon|hi", RNS_ENVIN_BAD_FROM},              /* short */
            {"1|6f796a5113a5a7f34512d2ede3723f400|beacon|hi",
             RNS_ENVIN_BAD_FROM},                                      /* long */
            {"1||beacon|hi", RNS_ENVIN_BAD_FROM},                      /* empty */
            /* SESSION CHARSET. Anything outside [A-Za-z0-9._-] would go
             * straight into a card title. */
            {"1|6f796a5113a5a7f34512d2ede3723f40|bea con|hi",
             RNS_ENVIN_BAD_SESSION},
            {"1|6f796a5113a5a7f34512d2ede3723f40|bea\x01n|hi",
             RNS_ENVIN_BAD_SESSION},
            {"1|6f796a5113a5a7f34512d2ede3723f40|beacon/room|hi",
             RNS_ENVIN_BAD_SESSION},
            /* AN EMPTY TEXT IS NOT AN ENVELOPE. The builder refuses to emit
             * one, so this frame was not built here — and taking it apart would
             * produce a card with an empty body, which is the blank message the
             * soft-parse rule exists to prevent. Shown raw, all 36 bytes of
             * visible framing, it is at least evidence. */
            {"1|6f796a5113a5a7f34512d2ede3723f40|beacon|", RNS_ENVIN_EMPTY_TEXT},
            {"1|6f796a5113a5a7f34512d2ede3723f40||", RNS_ENVIN_EMPTY_TEXT},
        };

        for (size_t b = 0; b < sizeof(bad) / sizeof(bad[0]); b++) {
            memset(&v, 0xAA, sizeof(v));
            assert(rns_envelope_parse((const uint8_t *)bad[b].payload,
                                      strlen(bad[b].payload), &v) ==
                   bad[b].want);
            /* The view is ZEROED on every outcome but OK, so a caller that
             * ignores the return value reads nulls rather than the pointers
             * from the message before this one. */
            assert(v.from == NULL && v.from_len == 0);
            assert(v.session == NULL && v.session_len == 0);
            assert(v.text == NULL && v.text_len == 0);
            /* Every reason is a usable literal: it is published as-is. */
            assert(rns_envin_reason(bad[b].want)[0] != '\0');
        }
        assert(rns_envelope_parse(NULL, 10, &v) == RNS_ENVIN_NO_FRAME);
        /* A null view is legal — the firmware only wants the verdict when it is
         * counting — and must not be dereferenced. */
        assert(rns_envelope_parse((const uint8_t *)"nope", 4, NULL) ==
               RNS_ENVIN_NO_FRAME);
    }

    /* ---- 15. THE SESSION LENGTH BOUNDARY, FROM THE READING SIDE -------------
     * 23 bytes is the longest name the peer accepts and 24 is not a name at
     * all. The firmware copies this field into a fixed buffer sized from
     * RNS_OUTBOX_SESSION_MAX and terminates it, so a parser that let 24 through
     * would be a one-byte overrun rather than an ugly title — which is why the
     * boundary is checked here and not only in the builder. */
    {
        rns_envelope_view v;
        char p[128];
        char sname[RNS_OUTBOX_SESSION_MAX + 2];
        int n;

        memset(sname, 'a', RNS_OUTBOX_SESSION_MAX);
        sname[RNS_OUTBOX_SESSION_MAX] = '\0';
        n = snprintf(p, sizeof(p), "1|%s|%s|hi", SELF, sname);
        assert(rns_envelope_parse((const uint8_t *)p, (size_t)n, &v) ==
               RNS_ENVIN_OK);
        assert(v.session_len == RNS_OUTBOX_SESSION_MAX);

        memset(sname, 'a', RNS_OUTBOX_SESSION_MAX + 1);
        sname[RNS_OUTBOX_SESSION_MAX + 1] = '\0';
        assert(strlen(sname) == 24);
        n = snprintf(p, sizeof(p), "1|%s|%s|hi", SELF, sname);
        assert(rns_envelope_parse((const uint8_t *)p, (size_t)n, &v) ==
               RNS_ENVIN_BAD_SESSION);
    }

    /* ---- 16. THE PACKET CEILING, FROM THE READING SIDE ----------------------
     * 383 bytes is the largest plaintext one encrypted packet to a SINGLE
     * destination carries, so a payload of exactly that must parse and one byte
     * more cannot have arrived through the path this parser serves. The text
     * budget is checked against the SESSION's length for the same reason the
     * builder checks it there: a constant budget is right for an empty session
     * and one byte over the packet for every named one. */
    {
        rns_envelope_view v;
        uint8_t big[RNS_OUTBOX_PAYLOAD_MAX + 2];
        char maxtext[RNS_OUTBOX_TEXT_MAX + 8];

        filltext(maxtext, RNS_OUTBOX_TEXT_MAX);
        assert(rns_envelope_build(SELF, "", maxtext, big, sizeof(big), &len) ==
               RNS_ENV_OK);
        assert(len == RNS_OUTBOX_PAYLOAD_MAX);
        assert(rns_envelope_parse(big, len, &v) == RNS_ENVIN_OK);
        assert(v.text_len == RNS_OUTBOX_TEXT_MAX);
        assert(memcmp(v.text, maxtext, v.text_len) == 0);

        /* One byte past the ceiling: not a shape this parser will vouch for. */
        big[len] = 'x';
        assert(rns_envelope_parse(big, (size_t)len + 1, &v) ==
               RNS_ENVIN_TOO_LONG);
    }

    /* ---- 17. THE VERSION DIGIT HAS ONE OWNER -------------------------------
     * The builder writes RNS_ENVELOPE_VERSION and the parser compares against
     * it, so a bump moves both at once. If it were a literal in two places, a
     * bump in the emitter alone would put payloads on the wire that this same
     * firmware reads as ordinary text — and nothing would say so, because that
     * is a legal outcome. */
    {
        assert(RNS_ENVELOPE_VERSION == '1');
        assert(rns_envelope_build(SELF, "", "v", env, sizeof(env), &len) ==
               RNS_ENV_OK);
        assert(env[0] == RNS_ENVELOPE_VERSION);
    }

    /* ---- 18. THE CONTROL FRAME, AND THE ONE-WAY DOOR IT COMES THROUGH ------
     * The peer sends `1|<32 hex>|*|main,sonata` to say which rooms are live.
     * The session field is exactly one byte, '*', and the byte was chosen
     * because it is OUTSIDE [A-Za-z0-9._-] and therefore cannot collide with a
     * room name however the peer names its rooms.
     *
     * THE ASYMMETRY IS THE ASSERTION. The receive side takes it; the builder
     * refuses it, and that refusal is what makes "this device never emits a
     * control frame" a property of the code rather than a promise in a comment.
     * A single loosened validator would pass every parse assertion below and
     * quietly delete that property, which is why both directions are checked on
     * the same value. */
    {
        rns_envelope_view v;
        char p[128];
        int n;

        /* The three predicates, on their own, before any framing. */
        assert(rns_session_is_control_n("*", 1));
        assert(!rns_session_valid_n("*", 1));       /* SEND side refuses it */
        assert(rns_session_valid_in_n("*", 1));     /* RECEIVE side takes it */
        assert(RNS_ENVELOPE_CONTROL_SESSION == '*');
        assert(RNS_ENVELOPE_CONTROL_SESSION == 0x2A);

        /* EXACTLY ONE BYTE, and nothing near it. "**", "*a" and "a*" are not
         * control frames and are not names either — '*' is outside the charset
         * — so they stay exactly what they were before this existed. */
        assert(!rns_session_is_control_n("**", 2));
        assert(!rns_session_is_control_n("*a", 2));
        assert(!rns_session_is_control_n("a*", 2));
        assert(!rns_session_is_control_n("", 0));
        assert(!rns_session_is_control_n(NULL, 1));
        assert(!rns_session_valid_in_n("**", 2));
        assert(!rns_session_valid_in_n("*a", 2));
        assert(!rns_session_valid_in_n("a*", 2));
        /* ...and the values that were legal stay legal on the receive side:
         * an empty session, and the longest name the peer accepts. */
        assert(rns_session_valid_in_n("", 0));
        assert(rns_session_valid_in_n("main.2_a-B9", 11));
        {
            char s23[RNS_OUTBOX_SESSION_MAX + 2];
            memset(s23, 'a', RNS_OUTBOX_SESSION_MAX + 1);
            assert(rns_session_valid_in_n(s23, RNS_OUTBOX_SESSION_MAX));
            assert(!rns_session_valid_in_n(s23, RNS_OUTBOX_SESSION_MAX + 1));
        }

        /* THE FRAME PARSES, and the session comes back as ONE byte pointing
         * into the payload — not copied, not rewritten, not terminated. */
        n = snprintf(p, sizeof(p), "1|%s|*|main,sonata", SELF);
        assert(rns_envelope_parse((const uint8_t *)p, (size_t)n, &v) ==
               RNS_ENVIN_OK);
        assert(v.session_len == 1);
        assert(v.session[0] == '*');
        assert(v.session == (const char *)p + 35);
        assert(v.text_len == 11 && memcmp(v.text, "main,sonata", 11) == 0);
        assert(memcmp(v.from, SELF, RNS_OUTBOX_ADDR_HEX) == 0);

        /* An empty room list is still an ordinary control frame: one character
         * of text is enough, and a frame with NO text is refused exactly as
         * every other empty envelope is. */
        n = snprintf(p, sizeof(p), "1|%s|*|-", SELF);
        assert(rns_envelope_parse((const uint8_t *)p, (size_t)n, &v) ==
               RNS_ENVIN_OK);
        assert(v.session_len == 1 && v.text_len == 1);
        n = snprintf(p, sizeof(p), "1|%s|*|", SELF);
        assert(rns_envelope_parse((const uint8_t *)p, (size_t)n, &v) ==
               RNS_ENVIN_EMPTY_TEXT);

        /* THE NEIGHBOURS OF '*' ARE NOT ENVELOPES. Each of these differs from
         * the frame above by one byte, and each must stay a payload shown raw. */
        {
            const char *near[] = {"**", "*a", "a*", "*.", " *", "**a"};
            for (size_t i = 0; i < sizeof(near) / sizeof(near[0]); i++) {
                n = snprintf(p, sizeof(p), "1|%s|%s|hi", SELF, near[i]);
                assert(rns_envelope_parse((const uint8_t *)p, (size_t)n, &v) ==
                       RNS_ENVIN_BAD_SESSION);
                assert(v.session == NULL && v.session_len == 0);
            }
        }

        /* THE BUILDER REFUSES IT, which is the half that must not drift. This
         * device is the thing a control frame instructs; being unable to build
         * one is what keeps it from instructing anybody else. */
        assert(rns_envelope_build(SELF, "*", "main,sonata", env, sizeof(env),
                                  &len) == RNS_ENV_BAD_SESSION);
        assert(len == 0);
        assert(!rns_session_valid("*"));
        assert(rns_envelope_build(SELF, "**", "hi", env, sizeof(env), &len) ==
               RNS_ENV_BAD_SESSION);
        assert(rns_envelope_build(SELF, "*a", "hi", env, sizeof(env), &len) ==
               RNS_ENV_BAD_SESSION);
        assert(rns_envelope_build(SELF, "a*", "hi", env, sizeof(env), &len) ==
               RNS_ENV_BAD_SESSION);
        /* ...and everything the builder took before, it still takes. */
        assert(rns_envelope_build(SELF, "", "hi", env, sizeof(env), &len) ==
               RNS_ENV_OK);
        assert(rns_envelope_build(SELF, "main", "hi", env, sizeof(env), &len) ==
               RNS_ENV_OK);

        /* THE BUDGET IS THE ORDINARY ONE FOR A ONE-BYTE SESSION: 347 - 1. The
         * control session is a session as far as the arithmetic is concerned,
         * so a full-length room list is bounded like any other text and both
         * sides agree on where the ceiling is. */
        assert(rns_envelope_text_budget_n(1) == RNS_OUTBOX_TEXT_MAX - 1);
        {
            uint8_t big[RNS_OUTBOX_PAYLOAD_MAX + 2];
            char rooms[RNS_OUTBOX_TEXT_MAX + 8];
            size_t o = 0;
            filltext(rooms, RNS_OUTBOX_TEXT_MAX - 1);
            memcpy(big, "1|", 2); o = 2;
            memcpy(big + o, SELF, RNS_OUTBOX_ADDR_HEX); o += RNS_OUTBOX_ADDR_HEX;
            big[o++] = '|'; big[o++] = '*'; big[o++] = '|';
            memcpy(big + o, rooms, RNS_OUTBOX_TEXT_MAX - 1);
            o += RNS_OUTBOX_TEXT_MAX - 1;
            assert(o == RNS_OUTBOX_PAYLOAD_MAX);
            assert(rns_envelope_parse(big, o, &v) == RNS_ENVIN_OK);
            assert(v.session_len == 1 && v.text_len == RNS_OUTBOX_TEXT_MAX - 1);
            /* One byte more is past what a single packet carries. */
            big[o] = 'x';
            assert(rns_envelope_parse(big, o + 1, &v) == RNS_ENVIN_TOO_LONG);
        }
    }

    printf("RNS outbox tests: OK\n");
    return 0;
}
