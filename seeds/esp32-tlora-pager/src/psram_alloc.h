#pragma once
// PSRAM-preferred allocation for the large task-context buffers (the micron
// page store, the history RAM index and the micron render grids). These are the
// biggest static internal-RAM consumers in the firmware; parking them in the
// board's 8 MB external QSPI PSRAM relieves internal DRAM, which is what the OOM
// root cause needs (a foreign mDNS packet -> esp_log -> mutex alloc aborts once
// internal RAM is exhausted).
//
// EXT_RAM_BSS_ATTR would be the one-attribute route, but this build's precompiled
// sdkconfig (esp32s3/qio_qspi) does NOT define CONFIG_SPIRAM_ALLOW_BSS_SEG_-
// EXTERNAL_MEMORY, so that macro expands to nothing (a silent no-op that leaves
// the buffer in DRAM). We therefore allocate explicitly at init.
//
// GRACEFUL FALLBACK: prefer external SPI RAM, fall back to internal DRAM if PSRAM
// is absent or the external allocation fails, so the buffer always works on a
// board with no/failed PSRAM. SAFETY: PSRAM is cached but NOT ISR-safe; every
// caller here runs in task context only (page store on loop/AsyncTCP, history
// index under g_hist_mux, render grids on the UI/loop task) — never from an ISR.
//
// Firmware glue only: this header pulls in esp_heap_caps.h and is never compiled
// by the host tests (they build the pure micron/history headers directly).
#include <stddef.h>
#include <string.h>
#include "esp_heap_caps.h"

// Allocate `size` zeroed bytes, external PSRAM first, internal DRAM on fallback.
// Returns nullptr only if BOTH pools are exhausted (callers guard for it).
static inline void *psram_calloc_pref(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);     // external RAM first
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL); // fall back to DRAM
    if (p) memset(p, 0, size);
    return p;
}

// newlib fopen → lock_init_generic aborts when internal DRAM cannot allocate the
// FILE lock (serial 2026-08-14: open chat → agents_sync_view → FS::open → abort).
// Soft-skip the open rather than reboot. 12 KB leaves room for the lock + VFS
// object + a small JsonDocument without starving lwIP/WiFi.
#ifndef FS_OPEN_MIN_INTERNAL
#define FS_OPEN_MIN_INTERNAL 12288u
#endif
static inline bool fs_internal_heap_ok(size_t need = FS_OPEN_MIN_INTERNAL) {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= need;
}
