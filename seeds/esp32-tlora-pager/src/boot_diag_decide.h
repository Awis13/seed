#pragma once
//
// Pure, host-testable decision logic for the NVS-persistent panic counter
// (panics_since_flash). No Arduino, no NVS here: the flash read/write is HW glue
// in boot_diag_init(); this header only decides what the persisted value should
// become, so the rule can be unit-tested on the host.
//
// Why this counter exists: the reset BUTTON on this board reports the same reason
// as a real power cycle (ESP_RST_POWERON), and RTC_NOINIT memory — where the
// ephemeral panic_count lives — is wiped on any poweron. So a manual reset erases
// the RTC panic history, and a panic that happened before the button press reads
// back as "clean". panics_since_flash is stored in NVS (flash), which is NOT
// wiped on poweron, so it keeps the panic evidence across a manual reset. It is
// cleared ONLY when the build signature changes (a genuine reflash) — never on a
// bare poweron/button — so "since flash" stays accurate and a reflash still gets
// a clean slate.
//
// This deliberately does NOT try to tell the button apart from real power: both
// are ESP_RST_POWERON and esp_reset_reason() cannot distinguish them. The fix is
// to make the history survive the event, not to label the event.

#include <stdint.h>
#include <string.h>

struct boot_diag_nvs_decision {
    bool     sig_changed;         // stored signature != current => a fresh flash
    uint32_t panics_since_flash;  // resulting value to serve and (maybe) persist
    bool     write_needed;        // true iff the stored value must be rewritten
};

// stored_sig    : build signature currently in NVS (nullptr if the key is absent).
// current_sig   : this build's signature (never nullptr).
// stored_panics : panics_since_flash currently in NVS (0 if absent).
// is_panic      : the SAME panic decision boot_diag_init() made for the RTC
//                 counter (reset_is_panic + the coredump-gated ESP_RST_WDT case).
//                 It is passed in, not re-derived, so both counters agree.
static inline boot_diag_nvs_decision boot_diag_nvs_decide(
        const char *stored_sig, const char *current_sig,
        uint32_t stored_panics, bool is_panic) {
    boot_diag_nvs_decision d;
    d.sig_changed = (stored_sig == nullptr) ||
                    strcmp(stored_sig, current_sig) != 0;
    // Fresh flash => clean slate; otherwise carry the persisted value forward.
    d.panics_since_flash = d.sig_changed ? 0u : stored_panics;
    // A fresh flash rewrites both the value (to 0) and the new signature.
    d.write_needed = d.sig_changed;
    if (is_panic) {
        d.panics_since_flash++;
        d.write_needed = true;
    }
    return d;
}
