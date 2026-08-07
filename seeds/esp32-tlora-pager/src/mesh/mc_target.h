// Minimal MeshCore radio target for T-Lora Pager SX1262 (seed embed).
#pragma once

#include <helpers/ArduinoHelpers.h>
#include <MeshCore.h>

class SeedTLoraBoard;
bool mesh_radio_init();
void mesh_radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr);
void mesh_radio_set_tx_power(int8_t dbm);
uint32_t mesh_radio_rng_seed();
mesh::Radio &mesh_get_radio();

extern VolatileRTCClock fallback_clock;
extern ArduinoMillis mesh_ms_clock;
extern StdRNG mesh_rng;
