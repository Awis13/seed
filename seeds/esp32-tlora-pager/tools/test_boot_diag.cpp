/*
 * Host tests for the NVS-persistent panic-counter decision (boot_diag_decide.h).
 *
 * Pure logic — no Arduino, no NVS. The reset button on this board reports
 * ESP_RST_POWERON (same as real power) and wipes RTC_NOINIT, so the RTC
 * panic_count is erased by every manual reset. panics_since_flash lives in NVS
 * (survives poweron) and must:
 *   - reset to 0 on a fresh flash (build signature changed), incl. absent sig;
 *   - increment on a panic while the signature is unchanged (no reset);
 *   - stay UNCHANGED with no write on a bare poweron/button (same signature,
 *     no panic) — the manual-reset-survives contract;
 *   - a fresh-flash boot that also panicked ends at 1 (clean slate, then +1).
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/boot_diag_decide.h"

int main(void) {
    // 1. fresh flash (signature changed): reset to 0, write needed.
    {
        boot_diag_nvs_decision d =
            boot_diag_nvs_decide("0.9.65 old", "0.9.66 new", 7, false);
        assert(d.sig_changed);
        assert(d.panics_since_flash == 0);
        assert(d.write_needed);
    }
    // 2. absent stored signature (nullptr) is a fresh flash too.
    {
        boot_diag_nvs_decision d =
            boot_diag_nvs_decide(nullptr, "0.9.66 new", 4, false);
        assert(d.sig_changed);
        assert(d.panics_since_flash == 0);
        assert(d.write_needed);
    }
    // 3. fresh flash AND a panic on the same boot: clean slate then +1 = 1.
    {
        boot_diag_nvs_decision d =
            boot_diag_nvs_decide(nullptr, "0.9.66 new", 9, true);
        assert(d.sig_changed);
        assert(d.panics_since_flash == 1);
        assert(d.write_needed);
    }
    // 4. same signature + panic: increment the stored value, no reset.
    {
        boot_diag_nvs_decision d =
            boot_diag_nvs_decide("0.9.66 new", "0.9.66 new", 3, true);
        assert(!d.sig_changed);
        assert(d.panics_since_flash == 4);
        assert(d.write_needed);
    }
    // 5. same signature, no panic (a plain poweron/button): value UNCHANGED,
    //    NO write, NO reset — this is the whole point of the NVS counter.
    {
        boot_diag_nvs_decision d =
            boot_diag_nvs_decide("0.9.66 new", "0.9.66 new", 5, false);
        assert(!d.sig_changed);
        assert(d.panics_since_flash == 5);
        assert(!d.write_needed);
    }

    printf("boot diag nvs decision tests: OK\n");
    return 0;
}
