/*
 * Host tests for the b33pr boot-splash decrypt animation (boot_logo.h). The
 * pure schedule/phase/colour logic is Arduino-free by design so the whole
 * timeline can be pinned without a panel. This asserts:
 *   - the phase machine advances monotonically Typing -> Fast -> Slow -> Found
 *     -> Done for every cell, and every phase is actually reached;
 *   - the mark resolves to EXACTLY "b33pr" by the end (final glyph per cell);
 *   - the white->gold discovery ramp is monotonic per channel and lands on the
 *     canonical gold #eda000, with the flash peak starting at white;
 *   - the cipher/ramp glyphs are drawn in green, the final glyph in gold;
 *   - the total duration stays inside the ~1.2-1.5 s budget;
 *   - cell_out never indexes out of the cell array (OOB guard).
 * Deterministic: a fixed seed pins the schedule; the firmware injects entropy.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../src/boot_logo.h"

using namespace bootlogo;

static int rank(Phase p) { return (int)p; }

// Every code point the animation can emit must be a real, drawable glyph:
// space, ASCII 33..126, or one of the box/block code points. (Mirrors what
// box_glyphs.h + FONT5X7 cover; a '?' fallback would mean an unrenderable cp.)
static bool drawable(uint32_t cp) {
    if (cp == ' ') return true;
    if (cp >= 33 && cp <= 126) return true;
    for (int i = 0; i < CIPHER_BOX_N; i++)
        if (CIPHER_BOX[i] == cp) return true;
    return false;
}

int main() {
    // A spread of seeds so the randomised fast/slow draws are exercised, not
    // just one lucky timeline.
    const uint32_t seeds[] = {0u, 1u, 42u, 0xDEADBEEFu, 0x12345678u,
                              0xFFFFFFFFu, 7u, 100000u, 0xA5A5A5A5u, 999u};

    for (size_t si = 0; si < sizeof(seeds) / sizeof(seeds[0]); si++) {
        const Schedule s = build_schedule(seeds[si]);

        // --- duration budget: 1.2 s .. 1.5 s inclusive -----------------------
        int total_ms = (int)s.total_frames * TICK_MS;
        assert(total_ms >= 1200 && "splash must not be too short");
        assert(total_ms <= 1500 && "splash must stay snappy (<=1.5s)");

        for (int cell = 0; cell < N_CELLS; cell++) {
            const Cell &c = s.cells[cell];
            // schedule ordering sanity
            assert(c.reveal_frame < c.typed_frame);
            assert(c.typed_frame <= s.decrypt_start);
            assert(c.resolve_frame > s.decrypt_start);
            assert(c.done_frame == c.resolve_frame + FOUND_LEN);

            const uint32_t letter = (uint32_t)(unsigned char)WORD[cell];

            int prev_rank = -1;
            bool saw_fast = false, saw_slow = false, saw_found = false, saw_done = false;
            uint32_t last_found_rgb = WHITE;
            int last_r = 256, last_g = 256, last_b = 256;

            for (int f = 0; f < (int)s.total_frames; f++) {
                CellOut o = cell_out(s, cell, f);

                // every emitted code point is drawable (never the '?' fallback)
                assert(drawable(o.cp));

                // phase never regresses
                assert(rank(o.phase) >= prev_rank && "phase must not go backwards");
                prev_rank = rank(o.phase);

                if (o.phase == Phase::Typing) {
                    // before reveal: hidden; during ramp: a block shade in green
                    if (f < c.reveal_frame) {
                        assert(!o.shown);
                    } else {
                        assert(o.shown);
                        bool is_ramp = (o.cp == RAMP[0] || o.cp == RAMP[1] ||
                                        o.cp == RAMP[2] || o.cp == RAMP[3]);
                        assert(is_ramp && "type-in draws the block ramp");
                        assert(o.rgb == GREENS[0] || o.rgb == GREENS[1] || o.rgb == GREENS[2]);
                    }
                } else if (o.phase == Phase::Fast || o.phase == Phase::Slow) {
                    saw_fast |= (o.phase == Phase::Fast);
                    saw_slow |= (o.phase == Phase::Slow);
                    assert(o.shown);
                    // cipher is always green
                    assert(o.rgb == GREENS[0] || o.rgb == GREENS[1] || o.rgb == GREENS[2]);
                } else if (o.phase == Phase::Found) {
                    saw_found = true;
                    assert(o.shown);
                    assert(o.cp == letter && "found paints the resolved letter");
                    // white->gold ramp: monotonic non-increasing per channel
                    int r = (o.rgb >> 16) & 0xFF, g = (o.rgb >> 8) & 0xFF, b = o.rgb & 0xFF;
                    if (f == c.resolve_frame) {
                        assert(o.rgb == WHITE && "flash peak starts at white");
                    }
                    assert(r <= last_r && g <= last_g && b <= last_b &&
                           "discovery ramp is monotonic toward gold");
                    last_r = r; last_g = g; last_b = b;
                    last_found_rgb = o.rgb;
                } else { // Done
                    saw_done = true;
                    assert(o.shown);
                    assert(o.cp == letter && "settled cell shows its final letter");
                    assert(o.rgb == GOLD && "settles to canonical gold #eda000");
                }
            }

            // every phase was actually reached for this cell
            assert(saw_fast && "fast phase reached");
            assert(saw_slow && "slow phase reached");
            assert(saw_found && "found phase reached");
            assert(saw_done && "done phase reached");
            // the ramp ended just shy of gold, then Done pins exact gold
            (void)last_found_rgb;
        }

        // --- final frame resolves to EXACTLY "b33pr" -------------------------
        int last = (int)s.total_frames - 1;
        for (int cell = 0; cell < N_CELLS; cell++) {
            CellOut o = cell_out(s, cell, last);
            assert(o.phase == Phase::Done);
            assert(o.cp == (uint32_t)(unsigned char)WORD[cell] &&
                   "the mark reads b33pr at the end");
            assert(o.rgb == GOLD);
        }

        // --- OOB guard: out-of-range cells do not read past the array --------
        CellOut lo = cell_out(s, -1, last);
        CellOut hi = cell_out(s, N_CELLS, last);
        assert(!lo.shown && !hi.shown);
    }

    // sanity on the cipher pool sizing
    assert(CIPHER_N == CIPHER_ASCII_N + CIPHER_BOX_N);
    for (int i = 0; i < CIPHER_N; i++) assert(drawable(cipher_cp((uint32_t)i)));

    return 0;
}
