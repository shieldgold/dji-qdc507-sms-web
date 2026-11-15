/* Linux link --wrap=getrandom: never changes kernel entropy or system time. */
#include "qdc.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
static unsigned calls, mode;
ssize_t __wrap_getrandom(void *out, size_t len, unsigned flags) {
    assert(flags == GRND_NONBLOCK);
    calls++;
    if (mode == 1 || (mode == 0 && calls == 1)) {
        errno = EAGAIN;
        return -1;
    }
    if (mode == 2) {
        errno = ENOSYS;
        return -1;
    }
    memset(out, 0xa5, len);
    return (ssize_t)len;
}
int main(void) {
    unsigned char out[32];
    qdc_error e = {0};
    assert(!qdc_random(out, sizeof(out), &e) && calls == 2);
    for (size_t i = 0; i < sizeof(out); i++)
        assert(out[i] == 0xa5);
    mode = 1;
    calls = 0;
    memset(out, 0xaa, sizeof(out));
    uint64_t start = qdc_mono_ms();
    assert(qdc_random(out, sizeof(out), &e) == -1 && calls == 2 && qdc_mono_ms() - start < 1000);
    for (size_t i = 0; i < sizeof(out); i++)
        assert(!out[i]);
    mode = 2;
    calls = 0;
    assert(qdc_random(out, sizeof(out), &e) == -1 && calls == 1);
    puts("crypto cold pool / missing syscall fail closed without weak fallback: PASS");
    return 0;
}
