/* Synthetic admission controls; no keyboard, peer or stored settings are used. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

int owner_checked_control(void) {
    puts("OWNER_TEST_RESULT control=0");
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    owner_checked_control();
#if defined(SYNTHETIC_ASAN)
    volatile unsigned char *value = malloc(1);
    if (!value) {
        return 2;
    }
    value[1] = 7;
    free((void *)value);
#elif defined(SYNTHETIC_UBSAN)
    volatile int value = INT_MAX;
    value += 1;
#else
    puts("runtime error: a synthetic application label");
    puts("AddressSanitizer is enabled for this synthetic control");
#endif
    puts("EVENT 1");
    return 0;
}
