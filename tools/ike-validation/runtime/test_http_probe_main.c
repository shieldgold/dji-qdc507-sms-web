#include "http_probe.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static double began, previous, max_gap;
static int calls, cancel, late_eof, cancel_first;
static double monotonic(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return t.tv_sec+t.tv_nsec/1e9;
}
static int pump(void *unused) {
    (void)unused;
    double now = monotonic();
    if (previous && now-previous > max_gap) max_gap = now-previous;
    previous = now; calls++;
    if (cancel_first && calls == 1) return 1;
    if (late_eof && calls == 9) usleep(2300000);
    return cancel && now-began >= .2;
}
int main(int argc, char **argv) {
    if (argc == 2) return qdc_http_once(argv[1]);
    if (argc != 3 || (strcmp(argv[2], "pump") && strcmp(argv[2], "cancel") && strcmp(argv[2], "late-eof") && strcmp(argv[2], "cancel-first"))) return 64;
    cancel_first = !strcmp(argv[2], "cancel-first");
    late_eof = !strcmp(argv[2], "late-eof");
    cancel = !strcmp(argv[2], "cancel"); began = monotonic();
    struct qdc_http_result result;
    int rc = qdc_http_run(argv[1], pump, NULL, &result);
    printf("HTTP_PROBE status=%d transport=%d bytes=%zu\n", result.status, result.transport, result.body);
    fprintf(stderr, "PUMP calls=%d max_gap_ms=%.3f\n", calls, max_gap*1000);
    return rc;
}
