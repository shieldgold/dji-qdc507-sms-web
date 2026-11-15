/* CPU-only admission boundaries. No module, clock, supervisor or VPN actions. */
#define main qdc_guard_main
#define printf test_gate_printf
#include "guard.c"
#undef main
#undef printf
#include <assert.h>
#include <stdarg.h>

static long values[3];
static unsigned reads, waits, value_count;
static int interrupt_wait;
static int interrupt_ready;

int test_gate_printf(const char *format, ...) {
    va_list args; va_start(args, format);
    int rc = vprintf(format, args);
    va_end(args);
    if (interrupt_ready && !strncmp(format, "CLIENT_ADMISSION_READY", 22)) raise(SIGTERM);
    return rc;
}

long __wrap_memory_available(void) {
    assert(reads < value_count);
    return values[reads++];
}
int __wrap_nanosleep(const struct timespec *request, struct timespec *remaining) {
    (void)remaining;
    assert(request->tv_sec == 5 && request->tv_nsec == 0);
    waits++;
    if (interrupt_wait) {
        raise(SIGTERM);
        errno = EINTR;
        return -1;
    }
    return 0;
}
/* musl on 32-bit ARM redirects nanosleep to its time64 ABI symbol. */
int __wrap___nanosleep_time64(const struct timespec *request, struct timespec *remaining) {
    return __wrap_nanosleep(request, remaining);
}
static int admission(const char *threshold, long a, long b, long c, unsigned count) {
    values[0] = a; values[1] = b; values[2] = c;
    value_count = count; reads = waits = 0;
    interrupted = deadline_fired = 0;
    char *args[] = {"qdc-ike-guard", "admit-client", (char *)threshold, NULL};
    return qdc_guard_main(3, args);
}
int main(void) {
    assert(admission("12288", 12288, 12288, 12288, 3) == 0);
    assert(reads == 3 && waits == 3);
    values[0] = values[1] = values[2] = 12288;
    reads = waits = 0;
    char *framed[] = {"qdc-ike-guard", "admit-client", "12288", "RUN01", "gate", NULL};
    assert(qdc_guard_main(5, framed) == 0);
    assert(reads == 3 && waits == 3);
    interrupt_ready = 1;
    assert(admission("12288", 13000, 13000, 13000, 3) != 0);
    assert(reads == 3 && waits == 3);
    interrupt_ready = 0;
    assert(admission("12288", 12020, 0, 0, 1) != 0);
    assert(reads == 1 && waits == 0);
    assert(admission("12288", 13000, 12287, 0, 2) != 0);
    assert(reads == 2 && waits == 1);
    assert(admission("12288", -1, 0, 0, 1) != 0);
    assert(reads == 1 && waits == 0);
    assert(admission("11776", 11776, 11776, 11776, 3) == 0);
    assert(reads == 3 && waits == 3);
    for (unsigned i = 0; i < 5; i++) {
        const char *bad[] = {"6144", "12020", "12289", "012288", "12288x"};
        assert(admission(bad[i], 0, 0, 0, 0) == 64);
        assert(reads == 0 && waits == 0);
    }
    interrupt_wait = 1;
    assert(admission("12288", 13000, 0, 0, 1) != 0);
    assert(reads == 1 && waits == 1);
    int status;
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    puts("ADMISSION_TESTS_PASS thresholds_boundaries_fail_fast_interrupt_no_children=1");
    return 0;
}
