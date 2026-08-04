/* Quadrature rotary-encoder decoder. Part of the seed firmware.
 *
 * MIT License. Copyright (c) 2026 seed contributors.
 * See Rotary.h for the full license text.
 *
 * Algorithm: each call reads the two encoder pins into a two-bit code and looks
 * up the signed step for the (previous, current) transition in a small table.
 * Valid Gray-code edges score +1 (one rotational direction) or -1 (the other);
 * illegal edges (both bits changed at once, or no change) score 0 and are
 * ignored, which debounces contact chatter for free. The signed steps accumulate
 * and, once a full detent's worth has built up, one direction event is emitted
 * and that amount is subtracted back out so overshoot carries into the next
 * detent. A full-step detent is four edges; HALF_STEP halves the threshold so an
 * event fires at both the rest and half positions, matching a two-per-detent
 * encoder.
 *
 * process() stays a normal flash-resident function (no IRAM_ATTR): the caller's
 * ISR already brackets flash-cache windows, so this must not live in IRAM.
 */

#include "Arduino.h"
#include "Rotary.h"

/* Signed step for every (previous<<2 | current) two-bit transition. Index bits
 * are the pin reading (pin2<<1 | pin1). Legal single-bit Gray edges map to +/-1
 * according to rotation sense; illegal or null transitions map to 0. This 16-
 * entry form is the public-domain quadrature table. */
static const signed char TRANSITIONS[16] = {
   0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0,
};

/* Edges per detent: four for a full-step encoder, two in half-step mode. */
#ifdef HALF_STEP
#define DETENT_STEPS 2
#else
#define DETENT_STEPS 4
#endif

// Constructor. Each arg is the pin number for one encoder contact.
Rotary::Rotary(char _pin1, char _pin2) {
  pin1 = _pin1;
  pin2 = _pin2;
#ifdef ENABLE_PULLUPS
  pinMode(pin1, INPUT_PULLUP);
  pinMode(pin2, INPUT_PULLUP);
#else
  pinMode(pin1, INPUT);
  pinMode(pin2, INPUT);
#endif
  // Prime with the current pin reading so the first process() call measures a
  // real edge rather than a phantom transition from a fixed start state.
  prev = ((unsigned char)digitalRead(pin2) << 1) | (unsigned char)digitalRead(pin1);
  accum = 0;
}

unsigned char Rotary::process() {
  // Current two-bit pin reading.
  unsigned char cur = ((unsigned char)digitalRead(pin2) << 1) | (unsigned char)digitalRead(pin1);
  // Signed step for this transition; 0 for illegal/null edges (debounce).
  signed char step = TRANSITIONS[((prev & 0x3) << 2) | (cur & 0x3)];
  prev = cur;
  if (step == 0)
    return DIR_NONE;
  accum += step;
  if (accum >= DETENT_STEPS) {
    accum -= DETENT_STEPS;
    return DIR_CW;
  }
  if (accum <= -DETENT_STEPS) {
    accum += DETENT_STEPS;
    return DIR_CCW;
  }
  return DIR_NONE;
}
