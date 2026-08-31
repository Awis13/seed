// Rotary encoder + center click on T-Lora Pager.
// Pins: ROTARY_A=40, ROTARY_B=41, ROTARY_C=7 (pins_arduino.h).

#pragma once

#include <Arduino.h>

void hw_input_begin();

// Call every loop(). Updates step/click edges.
void hw_input_poll();

// Detents since last call (signed; + = CW). Consuming.
int hw_input_steps();

// True once per physical press-release shorter than hold threshold.
bool hw_input_click();

// True while center key is held.
bool hw_input_held();
bool hw_input_long_press();
