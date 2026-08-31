#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/settings_policy.h"
int main(void) {
    assert(settings_autolock_next(0) == 30);
    assert(settings_autolock_next(30) == 60);
    assert(settings_autolock_next(60) == 300);
    assert(settings_autolock_next(300) == 0);
    assert(!settings_autolock_due(30, 29999, false));
    assert(settings_autolock_due(30, 30000, false));
    assert(!settings_autolock_due(30, 30000, true));
    assert(!settings_autolock_due(0, UINT32_MAX, false));
    puts("settings autolock tests: OK");
}
