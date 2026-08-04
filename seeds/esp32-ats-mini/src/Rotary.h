/* Quadrature rotary-encoder decoder. Part of the seed firmware.
 *
 * MIT License. Copyright (c) 2026 seed contributors.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED. See the MIT License text for the full disclaimer.
 *
 * A mechanical rotary encoder outputs a two-bit Gray code on two pins. One
 * physical detent walks the pins through a full 00->10->11->01->00 cycle (or the
 * reverse for the opposite direction). process() runs an ORDERED state machine
 * that is anchored to the detent rest state (00): it advances one sub-state per
 * valid Gray edge and reports exactly ONE DIR_CW / DIR_CCW event when a complete
 * cycle returns to rest. Because the machine re-anchors at every physical detent,
 * a missed or coalesced edge (common when an ISR reads the net pin state) simply
 * fails to complete that detent and resets to rest -- it can NEVER leave a
 * residual that eats a later detent or shifts the emit phase. Illegal edges
 * (both bits change at once, or no change) reset to the anchor too, which
 * debounces contact chatter for free. In HALF_STEP mode it reports at both the
 * rest (00) and half (11) positions, i.e. twice per detent. This is an
 * independent implementation: the state names, encoding and transition tables
 * below are derived directly from the quadrature Gray-code sequence.
 */
#include "Arduino.h"

#ifndef rotary_h
#define rotary_h

// Enable this to emit codes twice per step
//#define HALF_STEP

#if defined(LILYGO_SI473X)
#define HALF_STEP
#endif

#define ENABLE_PULLUPS  // Enable weak pullups

// Values returned by 'process'
#define DIR_NONE 0x0    // No complete step yet
#define DIR_CW   0x10   // Clockwise step
#define DIR_CCW  0x20   // Anti-clockwise step

class Rotary
{
  public:
    Rotary(char, char);
    // Process pin(s)
    unsigned char process();
  private:
    // Current machine state. The low nibble is the sub-state id; the emit bits
    // (DIR_CW / DIR_CCW) are ORed into the table entry that completes a detent
    // and are masked off before the next lookup.
    unsigned char state;
    unsigned char pin1;
    unsigned char pin2;
};
#endif
