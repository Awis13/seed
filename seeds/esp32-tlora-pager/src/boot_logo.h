#pragma once

/*
 * boot_logo.h - PURE boot-splash "decrypt" animation logic for the b33pr
 * wordmark. No Arduino, no SPI, no floating point: given a frame index it
 * returns, for each cell, the glyph code point and RGB888 colour to paint.
 * The hardware side (hw_ui.cpp) owns the framebuffer blit and the per-frame
 * delay; this header owns the schedule, the phase machine and the colour ramp
 * so a host test can pin the whole timeline without a board.
 *
 * Effect design: TerminalTextEffects (c) ChrisBuilds, MIT
 *   (https://github.com/ChrisBuilds/terminaltexteffects). Rust reference port:
 *   omacom-io/ttfx (effects/decrypt.rs). This is a clean-room reimplementation
 *   of the "decrypt" effect against our own 5x7 framebuffer - the algorithm
 *   (block-ramp type-in, fast cipher flicker, slow settle, white->gold
 *   discovery flash) is ported; none of the upstream code is copied.
 *
 * Timeline (TICK_MS per frame): the browser preview b33pr-boot.html is the
 * approved artifact and runs at ~1.45x. To hit the owner's "a touch snappier"
 * / ~1.2-1.5 s budget on a single-row 5-cell wordmark (not the preview's
 * multi-row block-art), the swap COUNTS are compressed vs the raw ttfx ranges
 * (fast 28-42 -> 10-16, slow linger 35-60 -> 12-18) while the phase STRUCTURE,
 * glyph set and colours are kept exactly. Worst-case total is bounded by
 * construction: decrypt_start(10) + fast_max(16) + slow_max(18) + found(10) +
 * hold(6) = 60 frames = 1440 ms at TICK_MS=24. See the deviation note in the
 * commit / report.
 */

#include <stdint.h>

namespace bootlogo {

// ---- product wordmark ------------------------------------------------------
static const int  N_CELLS = 5;
static const char WORD[N_CELLS + 1] = "b33pr";

// ---- frame cadence ---------------------------------------------------------
// 24 ms/frame ~= the approved preview's BASE(1000/30) / 1.45x speed.
static const int TICK_MS = 24;

// ---- phase durations (frames) ---------------------------------------------
static const int TYPE_RAMP         = 8;   // block ramp length before cipher (ttfx: 4 blocks x2)
static const int FAST_MIN          = 10;  // fast cipher flicker, min frames
static const int FAST_SPAN         = 7;   // -> [10, 16]
static const int SLOW_SHORT_MIN    = 3;   // common short settle
static const int SLOW_SHORT_SPAN   = 4;   // -> [3, 6]
static const int SLOW_LINGER_MIN   = 12;  // 30% "linger" settle
static const int SLOW_LINGER_SPAN  = 7;   // -> [12, 18]
static const int FOUND_LEN         = 10;  // white->gold discovery flash steps
static const int HOLD              = 6;   // final gold hold before boot continues
// Duration floor: when the random draws resolve early, hold gold longer so the
// splash never feels abrupt. 52*24=1248ms floor; the construction ceiling is
// 10+16+18+10+6=60 frames = 1440ms, so total is always in [1.25s, 1.44s].
static const int MIN_TOTAL_FRAMES  = 52;

// ---- colours (RGB888 0xRRGGBB; hw converts to RGB565 at blit) --------------
static const uint32_t GREENS[3] = {0x008000u, 0x00CB00u, 0x00FF00u}; // ciphertext
static const uint32_t GOLD      = 0xEDA000u;                          // final settle
static const uint32_t WHITE     = 0xFFFFFFu;                          // flash peak

// ---- glyph sets ------------------------------------------------------------
// Block ramp: full -> dark -> medium -> light shade (all covered by box_glyphs).
static const uint32_t RAMP[4] = {0x2588u, 0x2593u, 0x2592u, 0x2591u};
// Cipher pool: printable ASCII 33..126 plus the box/block code points that the
// panel font actually renders (kept in sync with box_glyphs.h). Any code point
// here must resolve to a real glyph, never the '?' fallback.
static const uint32_t CIPHER_BOX[] = {
    0x2588u, 0x2593u, 0x2592u, 0x2591u, 0x2500u, 0x2502u, 0x250Cu, 0x2510u,
    0x2514u, 0x2518u, 0x251Cu, 0x2524u, 0x252Cu, 0x2534u, 0x253Cu, 0x2550u,
    0x2551u, 0x2580u, 0x2584u,
};
static const int CIPHER_ASCII_N = 126 - 33 + 1;                 // 94
static const int CIPHER_BOX_N   = (int)(sizeof(CIPHER_BOX) / sizeof(CIPHER_BOX[0]));
static const int CIPHER_N       = CIPHER_ASCII_N + CIPHER_BOX_N; // 113

enum class Phase : uint8_t { Typing = 0, Fast = 1, Slow = 2, Found = 3, Done = 4 };

struct Cell {
    uint8_t reveal_frame;   // typing (block ramp) starts
    uint8_t typed_frame;    // reveal + TYPE_RAMP -> cipher hold
    uint8_t fast_len;       // fast cipher frames after decrypt_start
    uint8_t slow_len;       // slow settle frames
    uint8_t resolve_frame;  // enters Found (white flash begins)
    uint8_t done_frame;     // fully gold
};

struct Schedule {
    Cell     cells[N_CELLS];
    uint32_t seed;          // drives the deterministic glyph flicker
    uint16_t decrypt_start; // frame all cells are typed -> global fast begins
    uint16_t total_frames;  // frame count incl. the final gold hold
};

struct CellOut {
    Phase    phase;
    uint32_t cp;    // code point to draw
    uint32_t rgb;   // RGB888 colour
    bool     shown; // false -> paint background only (cell not yet revealed)
};

// ---- deterministic hashing (no RNG state; frame is the only clock) ---------
static inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static inline uint32_t cipher_cp(uint32_t h) {
    uint32_t idx = h % (uint32_t)CIPHER_N;
    if (idx < (uint32_t)CIPHER_ASCII_N) return 33u + idx;
    return CIPHER_BOX[idx - (uint32_t)CIPHER_ASCII_N];
}

static inline uint32_t green_pick(uint32_t seed, int cell, int frame) {
    uint32_t h = hash32(seed ^ (0x1000193u * (uint32_t)(cell + 1)) ^
                        (0x0193u * (uint32_t)(frame + 1)));
    return GREENS[h % 3u];
}

// Vertical final gradient. On a single-row wordmark the top/bottom stops of the
// gold gradient collapse to the canonical settle colour, so every cell resolves
// to GOLD (#eda000). With a multi-row mark this is where the vertical lerp goes.
static inline uint32_t gold_for(int /*cell*/) { return GOLD; }

// Channel-wise integer lerp a->b, step k of n (k in [0,n]).
static inline uint32_t lerp888(uint32_t a, uint32_t b, int k, int n) {
    if (n <= 0) return b;
    if (k < 0) k = 0; if (k > n) k = n;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * k / n;
    int g = ag + (bg - ag) * k / n;
    int bl = ab + (bb - ab) * k / n;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

// Build the per-cell schedule from a seed. Deterministic: same seed -> same
// timeline (the host test injects a fixed seed; the firmware injects entropy).
static inline Schedule build_schedule(uint32_t seed) {
    Schedule s;
    s.seed = seed;
    uint16_t decrypt_start = 0;
    for (int i = 0; i < N_CELLS; i++) {
        uint8_t rev = (uint8_t)(i / 2);          // typing_speed 2 cells/frame
        uint8_t typed = (uint8_t)(rev + TYPE_RAMP);
        if (typed > decrypt_start) decrypt_start = typed;
        s.cells[i].reveal_frame = rev;
        s.cells[i].typed_frame  = typed;
    }
    s.decrypt_start = decrypt_start;
    uint16_t total = 0;
    for (int i = 0; i < N_CELLS; i++) {
        uint32_t h1 = hash32(seed ^ (0x9E3779B9u * (uint32_t)(i + 1)));
        uint32_t h2 = hash32(h1);
        uint8_t fast = (uint8_t)(FAST_MIN + (h1 % (uint32_t)FAST_SPAN));
        uint8_t slow;
        if ((h2 % 100u) < 30u)
            slow = (uint8_t)(SLOW_LINGER_MIN + ((h2 >> 8) % (uint32_t)SLOW_LINGER_SPAN));
        else
            slow = (uint8_t)(SLOW_SHORT_MIN + ((h2 >> 8) % (uint32_t)SLOW_SHORT_SPAN));
        uint16_t resolve = (uint16_t)(decrypt_start + fast + slow);
        uint16_t done    = (uint16_t)(resolve + FOUND_LEN);
        s.cells[i].fast_len      = fast;
        s.cells[i].slow_len      = slow;
        s.cells[i].resolve_frame = (uint8_t)resolve;
        s.cells[i].done_frame    = (uint8_t)done;
        if (done > total) total = done;
    }
    uint16_t tf = (uint16_t)(total + HOLD);
    if (tf < (uint16_t)MIN_TOTAL_FRAMES) tf = (uint16_t)MIN_TOTAL_FRAMES;
    s.total_frames = tf;
    return s;
}

// State of one cell at one frame. Pure function of (schedule, cell, frame).
static inline CellOut cell_out(const Schedule &s, int cell, int frame) {
    CellOut o;
    o.phase = Phase::Typing;
    o.cp = (uint32_t)' ';
    o.rgb = GREENS[0];
    o.shown = false;
    if (cell < 0 || cell >= N_CELLS) return o;   // OOB guard
    const Cell &c = s.cells[cell];
    const uint32_t letter = (uint32_t)(unsigned char)WORD[cell];

    if (frame < c.reveal_frame) {                // not revealed yet
        return o;
    }
    if (frame < c.typed_frame) {                 // block ramp type-in
        int k = frame - c.reveal_frame;
        int idx = k / 2; if (idx > 3) idx = 3;
        o.phase = Phase::Typing;
        o.cp = RAMP[idx];
        o.rgb = green_pick(s.seed, cell, frame);
        o.shown = true;
        return o;
    }
    if (frame < c.resolve_frame) {               // cipher flicker (fast then slow)
        bool fast = frame < (int)(s.decrypt_start + c.fast_len);
        int period = fast ? 2 : 4;               // fast: swap every 2f; slow: every 4f
        uint32_t h = hash32(s.seed ^ (0x2545F491u * (uint32_t)(cell + 1)) ^
                            (uint32_t)(frame / period) * 0x9E3779B9u);
        o.phase = fast ? Phase::Fast : Phase::Slow;
        o.cp = cipher_cp(h);
        o.rgb = green_pick(s.seed, cell, frame);
        o.shown = true;
        return o;
    }
    if (frame < c.done_frame) {                   // discovery: white -> gold
        int k = frame - c.resolve_frame;
        o.phase = Phase::Found;
        o.cp = letter;
        o.rgb = lerp888(WHITE, gold_for(cell), k, FOUND_LEN);
        o.shown = true;
        return o;
    }
    o.phase = Phase::Done;                         // settled gold
    o.cp = letter;
    o.rgb = gold_for(cell);
    o.shown = true;
    return o;
}

}  // namespace bootlogo
