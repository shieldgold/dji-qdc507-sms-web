#ifndef QDC_SMS_SWAP_CORE_H
#define QDC_SMS_SWAP_CORE_H
#include <stdint.h>
#include <sys/types.h>

#define QDC_SMS_SWAP_SECONDS 300
#define QDC_SMS_SWAP_MAX_BYTES (16U * 1024U * 1024U)
struct swap_control {
    char run[49], boot[37];
    pid_t supervisor, guardian;
    uint64_t sup_start, guard_start, old_dev, old_ino, old_size, new_dev, new_ino, new_size;
};
int swap_run_valid(const char *);
int swap_uint(const char *, uint64_t *);
int swap_control_parse(const char *, const char *, struct swap_control *);
/* 1: this complete record names target; 0: other mount; -1: malformed record. */
int swap_mount_record(const char *, const char *, uint64_t *);
int swap_clock_proof(const char *, const struct swap_control *, int);
int swap_lease_due(double, double);
#endif
