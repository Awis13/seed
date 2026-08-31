#pragma once
#include <stdint.h>
static inline bool settings_autolock_valid(uint16_t s) {
    return s == 0 || s == 30 || s == 60 || s == 300;
}
static inline uint16_t settings_autolock_next(uint16_t s) {
    if (s == 0) return 30;
    if (s == 30) return 60;
    if (s == 60) return 300;
    return 0;
}
static inline bool settings_autolock_due(uint16_t s, uint32_t elapsed,
                                         bool locked) {
    return !locked && s && elapsed >= (uint32_t)s * 1000u;
}
