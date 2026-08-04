/* Host-side simulation test for the rotary-encoder decoder (Rotary.cpp).
 *
 * MIT License. Copyright (c) 2026 seed contributors. Part of the seed firmware.
 *
 * WHY this exists: the encoder is a physical knob, so the only developer-side
 * proof that the decoder no longer "doesn't react, then skips" is to drive the
 * real decoder logic with synthetic pin sequences and assert the emitted events.
 * This compiles the ACTUAL src/Rotary.cpp against a tiny Arduino stub -- no
 * ESP32, no PlatformIO -- so what is tested is exactly what ships.
 *
 * Model of the hardware: the caller's ISR fires on every pin CHANGE and reads
 * the NET pin state, so each interrupt = one process() call for one pin code.
 * We therefore feed one pin code per process() call; a "missed"/coalesced edge
 * is modelled simply by omitting an intermediate code from the sequence.
 *
 * Build + run (full-step, the mode that ships for env:ats-mini):
 *   c++ -std=c++11 -Wall -Wextra -I test/mock -I src \
 *       test/test_rotary.cpp src/Rotary.cpp -o /tmp/test_rotary && /tmp/test_rotary
 * Half-step build (LILYGO_SI473X boards): add -DHALF_STEP to both files.
 */

#include <cstdio>
#include <cstdlib>
#include <vector>
#include "Arduino.h"
#include "Rotary.h"

// ---- Arduino stub: a fake pin array the test writes and the decoder reads. ----
static int g_pin[64];
void pinMode(int, int) {}
int digitalRead(int pin) { return g_pin[pin]; }

// The decoder instance under test. Rotary(pin1, pin2) reads pin2<<1 | pin1, so
// with pin1=1, pin2=2 a fed "code" equals exactly what process() reads back.
static const int PIN1 = 1, PIN2 = 2;

// Two-bit pin codes (pin2<<1 | pin1): 00=0, 01=1, 10=2, 11=3.
enum { C00 = 0, C01 = 1, C10 = 2, C11 = 3 };

// Feed one pin code to the real decoder and return its event.
static unsigned char feed(Rotary &enc, int code) {
  g_pin[PIN1] = code & 1;
  g_pin[PIN2] = (code >> 1) & 1;
  return enc.process();
}

// One clean detent as a sequence of pin codes (the rest 00 is the anchor, so a
// detent is the four edges that leave and return to it).
//   CW  detent: 00 -> 10 -> 11 -> 01 -> 00   (edges: 10,11,01,00)
//   CCW detent: 00 -> 01 -> 11 -> 10 -> 00   (edges: 01,11,10,00)
static const int CW_DETENT[4]  = { C10, C11, C01, C00 };
static const int CCW_DETENT[4] = { C01, C11, C10, C00 };

struct Counts { int cw; int ccw; };

static Counts run(Rotary &enc, const std::vector<int> &codes) {
  Counts c = {0, 0};
  for (size_t i = 0; i < codes.size(); i++) {
    unsigned char s = feed(enc, codes[i]);
    if (s == DIR_CW) c.cw++;
    else if (s == DIR_CCW) c.ccw++;
  }
  return c;
}

static void push_detent(std::vector<int> &v, const int det[4]) {
  for (int i = 0; i < 4; i++) v.push_back(det[i]);
}

// ---- The old free-running-accumulator decoder, reimplemented verbatim, purely
// so the reversal test can demonstrate it FAILS where the new one passes. This
// is the logic that shipped before the fix (signed steps into an accumulator
// that is never re-anchored to the detent rest state). ----
struct OldDecoder {
  int prev;
  int accum;
#ifdef HALF_STEP
  static const int DETENT = 2;
#else
  static const int DETENT = 4;
#endif
  OldDecoder() : prev(C00), accum(0) {}
  unsigned char process(int cur) {
    static const signed char T[16] = {
       0, -1, +1,  0,
      +1,  0,  0, -1,
      -1,  0,  0, +1,
       0, +1, -1,  0,
    };
    signed char step = T[((prev & 3) << 2) | (cur & 3)];
    prev = cur;
    if (step == 0) return DIR_NONE;
    accum += step;
    if (accum >= DETENT)  { accum -= DETENT; return DIR_CW; }
    if (accum <= -DETENT) { accum += DETENT; return DIR_CCW; }
    return DIR_NONE;
  }
};

static Counts run_old(OldDecoder &enc, const std::vector<int> &codes) {
  Counts c = {0, 0};
  for (size_t i = 0; i < codes.size(); i++) {
    unsigned char s = enc.process(codes[i]);
    if (s == DIR_CW) c.cw++;
    else if (s == DIR_CCW) c.ccw++;
  }
  return c;
}

// ---- Assertion plumbing ----
static int g_fail = 0;
#define CHECK(cond, msg) do { \
  if (cond) { printf("  PASS: %s\n", msg); } \
  else { printf("  FAIL: %s\n", msg); g_fail++; } \
} while (0)

int main() {
#ifdef HALF_STEP
  const int PER = 2;  // half-step: one event at rest (00) and one at half (11)
  const char *mode = "HALF_STEP";
#else
  const int PER = 1;  // full-step: one event per detent, at rest only
  const char *mode = "full-step";
#endif
  printf("Rotary decoder host simulation (%s mode)\n", mode);

  // --- 1. Clean detents: N clean CW cycles -> exactly N*PER CW, 0 CCW; reverse. ---
  {
    const int N = 5;
    std::vector<int> cw, ccw;
    for (int i = 0; i < N; i++) { push_detent(cw, CW_DETENT); push_detent(ccw, CCW_DETENT); }
    Rotary a(PIN1, PIN2); Counts rc = run(a, cw);
    CHECK(rc.cw == N * PER && rc.ccw == 0, "N clean CW cycles -> N*PER CW, 0 CCW");
    Rotary b(PIN1, PIN2); Counts rd = run(b, ccw);
    CHECK(rd.ccw == N * PER && rd.cw == 0, "N clean CCW cycles -> N*PER CCW, 0 CW");
  }

  // --- 2. No drift on a missed edge: an incomplete CW detent (one intermediate
  //        edge dropped) is cleanly skipped and leaves NO residual, so several
  //        following clean CW detents still count exactly. ---
  {
    const int N = 4;
    std::vector<int> seq;
    // Missed-edge CW detent: 00 -> 10 -> (11 dropped) -> 01 -> 00. Cannot complete.
    seq.push_back(C10); seq.push_back(C01); seq.push_back(C00);
    for (int i = 0; i < N; i++) push_detent(seq, CW_DETENT);
    Rotary a(PIN1, PIN2); Counts rc = run(a, seq);
    CHECK(rc.cw == N * PER && rc.ccw == 0,
          "missed-edge detent skipped, later CW count unshifted (no residual)");
  }

  // --- 3. Clean direction reversal (the user's bug): a few CW detents, then
  //        immediately a few CCW detents. The FIRST CCW must emit (no eaten
  //        detent) and totals must be exact. ---
  {
    const int NCW = 3, NCCW = 3;
    std::vector<int> seq;
    for (int i = 0; i < NCW; i++)  push_detent(seq, CW_DETENT);
    for (int i = 0; i < NCCW; i++) push_detent(seq, CCW_DETENT);
    Rotary a(PIN1, PIN2);
    // Watch the first CCW event: feed CW block, then first CCW detent alone.
    Rotary b(PIN1, PIN2);
    for (int i = 0; i < NCW; i++) run(b, std::vector<int>(CW_DETENT, CW_DETENT + 4));
    Counts first = run(b, std::vector<int>(CCW_DETENT, CCW_DETENT + 4));
    CHECK(first.ccw == PER && first.cw == 0, "first CCW detent after CW emits (not eaten)");
    Counts rc = run(a, seq);
    CHECK(rc.cw == NCW * PER && rc.ccw == NCCW * PER, "reversal totals exact");
  }

  // --- 4. Bounce immunity: chatter on the CW-leading contact before completing a
  //        CW detent yields exactly one CW event (PER in half-step) and no CCW. ---
  {
    std::vector<int> seq;
    // 00<->10 chatter, then complete the CW detent 10 -> 11 -> 01 -> 00.
    seq.push_back(C10); seq.push_back(C00);
    seq.push_back(C10); seq.push_back(C00);
    seq.push_back(C10); seq.push_back(C11); seq.push_back(C01); seq.push_back(C00);
    Rotary a(PIN1, PIN2); Counts rc = run(a, seq);
    CHECK(rc.cw == PER && rc.ccw == 0, "bounce before a CW detent -> one CW, no spurious CCW");
  }

  // --- 5. Teeth / before-after: the drift scenario the fix targets. A CW detent
  //        that loses one intermediate edge, then K clean CCW detents. The NEW
  //        decoder re-anchors, so all K CCW detents count. The OLD accumulator
  //        keeps +2 residual, so its first CCW detent is EATEN -- this is exactly
  //        the "doesn't react, then skips" the user reported. We assert NEW is
  //        correct AND OLD is wrong, proving the reversal test has teeth. ---
  {
    const int K = 3;
    std::vector<int> seq;
    seq.push_back(C10); seq.push_back(C01); seq.push_back(C00);  // missed-edge CW detent
    for (int i = 0; i < K; i++) push_detent(seq, CCW_DETENT);
    Rotary a(PIN1, PIN2); Counts rnew = run(a, seq);
    OldDecoder o; Counts rold = run_old(o, seq);
    printf("  [new decoder] cw=%d ccw=%d   [old accumulator] cw=%d ccw=%d\n",
           rnew.cw, rnew.ccw, rold.cw, rold.ccw);
    CHECK(rnew.ccw == K * PER && rnew.cw == 0, "NEW: no drift, all K CCW detents counted");
#ifndef HALF_STEP
    // Teeth: in full-step the +2 residual leaves the first CCW detent short of
    // the threshold, so the old accumulator emits fewer CCW than fed -- eaten.
    // (Half-step drift manifests differently -- spurious events rather than an
    // eaten detent -- so this before/after comparison is full-step specific.)
    CHECK(rold.ccw < K * PER, "OLD: residual eats a CCW detent (demonstrates the bug/teeth)");
#endif
  }

  if (g_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
  printf("%d CHECK(S) FAILED\n", g_fail);
  return 1;
}
