#ifndef QDC_ENTROPY_WAIT_H
#define QDC_ENTROPY_WAIT_H
#include <sys/types.h>
#define QDC_ENTROPY_TIMEOUT_MS 90000L
#define QDC_ENTROPY_ADMISSION_KIB 8192L
#define QDC_ENTROPY_FLOOR_KIB 6144L
#define QDC_ENTROPY_DROP_KIB 3072L
struct qdc_entropy_ops {
    long (*now_ms)(void *);
    long (*available_kib)(void *);
    ssize_t (*getrandom_nonblock)(void *);
    int (*discard_urandom)(void *);
    int (*sleep_ms)(void *, unsigned);
    void *context;
};
struct qdc_entropy_result {
    const char *reason;
    long elapsed_ms, min_available_kib;
    ssize_t getrandom_return;
    int error;
    unsigned attempts;
};
/* 1 = exact SMS lab gate; 2 = exact coexist+combo lab gates; 0 = refused. */
int qdc_entropy_gate(const char *commandline);
int qdc_entropy_wait(const struct qdc_entropy_ops *, struct qdc_entropy_result *);
#endif
