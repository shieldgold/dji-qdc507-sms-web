/* Linker-wrap fixture only; never part of the ARM helper. */
#include <stdlib.h>
long __wrap_memory_available(void) {
    static unsigned long calls;
    const char *text=getenv("SMS_SWAP_TEST_LOW_AFTER");
    unsigned long high=text?strtoul(text,NULL,10):0;
    return calls++<high?32768:6143;
}
