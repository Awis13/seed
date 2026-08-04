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
 * reverse for the opposite direction). process() reads the pins, tracks the net
 * quadrature position, and reports one DIR_CW / DIR_CCW event per completed
 * detent. In HALF_STEP mode it reports at both the rest and half positions, i.e.
 * twice per detent. Invalid or bouncing transitions cancel out and never
 * miscount (inherent debounce). This is an independent implementation built on
 * the public-domain 4x4 quadrature transition table.
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
    unsigned char prev;   // last two-bit pin reading (pin2<<1 | pin1)
    signed char accum;    // net quadrature steps toward the next detent
    unsigned char pin1;
    unsigned char pin2;
};
#endif
