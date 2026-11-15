#ifndef QDC_ARM_TLS_LAB_H
#define QDC_ARM_TLS_LAB_H
/* Disposable ARM VM helpers. No endpoint or randomness override is provided. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#define LAB_ADMISSION_KIB 8192L
#define LAB_FLOOR_KIB 6144L
#define LAB_DROP_KIB 3072L

static inline int lab_gate(const char *line) {
    const char *keys[] = {"qdc.sms.lab=", "qdc.tls.lab=", "qdc.coexist.lab=", "qdc.sms.combo="};
    unsigned seen[4] = {0};
    if (!line) return 0;
    while (*line) {
        while (isspace((unsigned char)*line)) line++;
        const char *end = line;
        while (*end && !isspace((unsigned char)*end)) end++;
        size_t n = (size_t)(end - line);
        for (unsigned i = 0; i < 4; i++) {
            size_t k = strlen(keys[i]);
            if (n >= k && !memcmp(line, keys[i], k)) {
                if (i > 1 || seen[i]++ || n != k + 1 || line[k] != '1') return 0;
            }
        }
        line = end;
    }
    return seen[0] == 1 && seen[1] == 1;
}

static inline int lab_runtime(void) {
    char line[4096];
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return -1;
    size_t n = fread(line, 1, sizeof(line) - 1, f);
    int extra = fgetc(f), bad = ferror(f);
    fclose(f);
    line[n] = 0;
    struct utsname u;
    return !bad && extra == EOF && !memchr(line, 0, n) && lab_gate(line) &&
                   !uname(&u) && !strcmp(u.release, "3.18.44") && !strncmp(u.machine, "arm", 3)
               ? 0 : -1;
}

static inline long lab_now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_sec > 2147483) return -1;
    return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static inline long lab_available_kib(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    long available = -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemAvailable: %ld kB", &available) == 1) break;
    fclose(f);
    return available;
}

static inline const char *lab_memory_reason(long baseline, long available) {
    if (baseline < LAB_ADMISSION_KIB) return "admission";
    if (available < 0) return "memory-read";
    if (available < LAB_FLOOR_KIB) return "memory-floor";
    if (baseline - available > LAB_DROP_KIB) return "memory-drop";
    return NULL;
}

#endif
