// Seed MeshCore client — private DM RX/TX for notify + sparse keepalive.
#pragma once

#include <Arduino.h>
#include <stdbool.h>
#include <stdint.h>

// Returns true if radio + BaseChatMesh started.
bool mesh_client_begin();

// Call from main loop (same core as display — shared SPI).
void mesh_client_loop();

// True after successful begin().
bool mesh_client_ready();

// Send private text to home Heltec (by pubkey hex). Returns false if not ready.
// expected_ack filled for processAck RTT; last_send_ms for RTT calc.
bool mesh_client_send_to_gateway(const char *text, uint32_t *expected_ack,
                                 uint32_t *est_timeout_ms);

// Last send millis (for ACK RTT).
uint32_t mesh_client_last_send_ms();

// Injected by skill: callbacks implemented in meshcore.cpp
void mesh_client_set_callbacks(
    void (*on_dm)(const char *from_name, const char *text),
    void (*on_ack)(uint32_t rtt_ms),
    void (*on_log)(const char *msg));
