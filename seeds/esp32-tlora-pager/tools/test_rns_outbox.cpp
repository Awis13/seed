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
 * the text as everything after the third. Deliberately written here rather than
 * exported from the header — the device never parses — so that the test proves
 * the emitted bytes are readable under the agreed rule rather than under
 * whatever the builder happens to do.
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

    printf("RNS outbox tests: OK\n");
    return 0;
}
