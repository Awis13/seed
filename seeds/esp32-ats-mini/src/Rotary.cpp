/* Quadrature rotary-encoder decoder. Part of the seed firmware.
 *
 * MIT License. Copyright (c) 2026 seed contributors.
 * See Rotary.h for the full license text.
 *
 * Algorithm: an ordered quadrature state machine anchored to the detent rest
 * state. Each call reads the two encoder pins into a two-bit code
 * (pin2<<1 | pin1) and looks up the next state in a table indexed by
 * [current sub-state][pin code]. Starting from the rest anchor, a run of valid
 * Gray edges walks the machine through the sub-states of one detent; the entry
 * that returns the machine to the anchor also carries an emit bit (DIR_CW /
 * DIR_CCW), so exactly one event fires per completed detent. Any illegal edge
 * (both bits flip at once, or a jump that does not belong to the current
 * sub-state) routes straight back to the anchor with no emit -- this both
 * debounces contact chatter and guarantees that a missed/coalesced edge cannot
 * leave a residual. Because the machine is re-anchored at every physical detent,
 * error can never accumulate: after any number of bounces, dropped edges or
 * direction reversals, the very next completed detent in either direction
 * reports correctly. A full-step detent is one 00->10->11->01->00 cycle;
 * HALF_STEP adds a second anchor at 11 so an event fires at both the rest and
 * half positions, matching a two-per-detent encoder.
 *
 * The transition tables are derived from the Gray-code sequence below, with our
 * own sub-state names and encoding -- not transcribed from any external table.
 *
 * process() stays a normal flash-resident function (no IRAM_ATTR): the caller's
 * ISR already brackets flash-cache windows, so this must not live in IRAM.
 */

#include "Arduino.h"
#include "Rotary.h"

/* Pin code = pin2<<1 | pin1, so the four columns of every row below are the
 * readings 00, 01, 10, 11 in that order. Our direction convention (unchanged
 * from the decoder this replaces): from rest 00, the first edge to 10 heads
 * clockwise, the first edge to 01 heads counter-clockwise.
 *   CW  detent: 00 -> 10 -> 11 -> 01 -> 00
 *   CCW detent: 00 -> 01 -> 11 -> 10 -> 00
 * The emit bits (DIR_CW / DIR_CCW) live in bits 0x10/0x20 and are ORed into the
 * table entry that completes a detent; the low nibble is always the next
 * sub-state id, extracted with & 0x0f on the following call. */

#ifdef HALF_STEP

/* Half-step: two anchors, at rest (00) and at half (11). Each anchor emits when
 * reached from the correct intermediate, so a full Gray cycle yields two events.
 * Sub-states: _S0 = anchor 00, _S1 = anchor 11; the four _FROMx states are the
 * single intermediate held while travelling between the two anchors. */
#define H_S0        0x0  // anchor at rest 00
#define H_CCW_FROM0 0x1  // left 00 counter-clockwise, sitting at 01, heading to 11
#define H_CW_FROM0  0x2  // left 00 clockwise, sitting at 10, heading to 11
#define H_S1        0x3  // anchor at half 11
#define H_CW_FROM1  0x4  // left 11 clockwise, sitting at 01, heading to 00
#define H_CCW_FROM1 0x5  // left 11 counter-clockwise, sitting at 10, heading to 00

static const unsigned char TTABLE[6][4] = {
  //             00                 01                  10                 11
  /* H_S0        */ { H_S0,             H_CCW_FROM0,        H_CW_FROM0,        H_S1              },
  /* H_CCW_FROM0 */ { H_S0,             H_CCW_FROM0,        H_S0,              H_S1 | DIR_CCW    },
  /* H_CW_FROM0  */ { H_S0,             H_S0,               H_CW_FROM0,        H_S1 | DIR_CW     },
  /* H_S1        */ { H_S0,             H_CW_FROM1,         H_CCW_FROM1,       H_S1              },
  /* H_CW_FROM1  */ { H_S0 | DIR_CW,    H_CW_FROM1,         H_S1,              H_S1              },
  /* H_CCW_FROM1 */ { H_S0 | DIR_CCW,   H_S1,               H_CCW_FROM1,       H_S1              },
};
#define R_START H_S0

#else

/* Full-step: one anchor, at rest 00. A complete four-edge cycle back to the
 * anchor emits once. Sub-states walk one detent:
 *   CW:  _BEGIN (at 10) -> _NEXT (at 11) -> _FINAL (at 01) -> anchor 00, emit CW
 *   CCW: _BEGIN (at 01) -> _NEXT (at 11) -> _FINAL (at 10) -> anchor 00, emit CCW
 * _NEXT / _FINAL absorb bounce by stepping back toward _BEGIN without emitting;
 * any edge that does not fit the current sub-state routes back to the anchor. */
#define F_START     0x0
#define F_CW_BEGIN  0x1  // at 10, one edge into a CW detent
#define F_CW_NEXT   0x2  // at 11, midway through a CW detent
#define F_CW_FINAL  0x3  // at 01, one edge from completing a CW detent
#define F_CCW_BEGIN 0x4  // at 01, one edge into a CCW detent
#define F_CCW_NEXT  0x5  // at 11, midway through a CCW detent
#define F_CCW_FINAL 0x6  // at 10, one edge from completing a CCW detent

static const unsigned char TTABLE[7][4] = {
  //             00                 01                  10                 11
  /* F_START     */ { F_START,          F_CCW_BEGIN,        F_CW_BEGIN,        F_START           },
  /* F_CW_BEGIN  */ { F_START,          F_START,            F_CW_BEGIN,        F_CW_NEXT         },
  /* F_CW_NEXT   */ { F_START,          F_CW_FINAL,         F_CW_BEGIN,        F_CW_NEXT         },
  /* F_CW_FINAL  */ { F_START | DIR_CW, F_CW_FINAL,         F_START,           F_CW_NEXT         },
  /* F_CCW_BEGIN */ { F_START,          F_CCW_BEGIN,        F_START,           F_CCW_NEXT        },
  /* F_CCW_NEXT  */ { F_START,          F_CCW_BEGIN,        F_CCW_FINAL,       F_CCW_NEXT        },
  /* F_CCW_FINAL */ { F_START | DIR_CCW,F_START,            F_CCW_FINAL,       F_CCW_NEXT        },
};
#define R_START F_START

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
  // Prime the machine at its rest anchor. The state machine self-corrects to the
  // real detent within one cycle even if the knob does not start exactly at 00,
  // so no phantom event is possible from a fixed start state.
  state = R_START;
}

unsigned char Rotary::process() {
  // Current two-bit pin reading (pin2<<1 | pin1).
  unsigned char code = ((unsigned char)digitalRead(pin2) << 1) | (unsigned char)digitalRead(pin1);
  // Advance the machine; mask the emit bits off the stored state so only the
  // sub-state id feeds the next lookup.
  state = TTABLE[state & 0x0f][code & 0x3];
  return state & 0x30;
}
