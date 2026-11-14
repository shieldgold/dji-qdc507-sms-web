#define _GNU_SOURCE
#include "proc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ssize_t read_complete(const char *path, char *buf, size_t cap) {
    if (cap < 2) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, buf + used, cap - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return -1; }
        if (!n) break;
        used += (size_t)n;
        if (used == cap - 1) { close(fd); errno = EOVERFLOW; return -1; }
    }
    close(fd); buf[used] = 0; return (ssize_t)used;
}
int proc_identity(pid_t pid, struct proc_id *id) {
    char path[80], text[4096];
    if (pid < 1) return -1;
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    if (read_complete(path, text, sizeof(text)) < 1) return -1;
    /* comm may itself contain spaces and parentheses. Field 3 follows LAST ')'. */
    char *end = strrchr(text, ')'), *save = NULL;
    if (!end || end[1] != ' ') return -1;
    int field = 3; unsigned long long start = 0; char state = 0; pid_t parent = -1;
    for (char *p = strtok_r(end + 2, " \n", &save); p; p = strtok_r(NULL, " \n", &save), field++) {
        if (field == 3) state = *p;
        if (field == 4) {
            char *tail; errno = 0; long n = strtol(p, &tail, 10);
            if (errno || *tail || n < 0 || n > INT_MAX) return -1;
            parent = (pid_t)n;
        }
        if (field == 22) {
            char *tail; errno = 0; start = strtoull(p, &tail, 10);
            if (errno || *tail) return -1;
            break;
        }
    }
    if (!start || !state || parent < 0) return -1;
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
    ssize_t n = readlink(path, id->exe, sizeof(id->exe)-1);
    if (n < 1 || n == (ssize_t)sizeof(id->exe)-1) return -1;
    id->exe[n] = 0; id->start = start; id->state = state; id->pid = pid; id->parent = parent;
    return 0;
}
int proc_matches(pid_t pid, unsigned long long start, const char *exe) {
    struct proc_id id;
    return !proc_identity(pid, &id) && id.start == start && !strcmp(id.exe, exe) && id.state != 'Z';
}
int supervisor_matches(pid_t pid, unsigned long long start) {
    if (!proc_matches(pid, start, "/bin/busybox")) return 0;
    char path[80], text[1024];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    ssize_t n = read_complete(path, text, sizeof(text));
    static const char expected[] = "/bin/busybox\0/bin/sh\0/data/smsweb/start.sh\0supervise\0";
    return n == (ssize_t)sizeof(expected)-1 && !memcmp(text, expected, sizeof(expected)-1);
}
long memory_available(void) {
    char text[16384];
    if (read_complete("/proc/meminfo", text, sizeof(text)) < 1) return -1;
    for (char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p+1 : NULL) {
        long n;
        if (sscanf(p, "MemAvailable: %ld kB", &n) == 1) return n;
    }
    return -1;
}
int memory_safe(long available, long baseline) {
    return available >= 6144 && baseline >= 0 && baseline - available <= 3072;
}
enum smaps_state { SMAPS_KEY, SMAPS_SPACE, SMAPS_SIGN, SMAPS_NUMBER, SMAPS_IGNORE };
struct smaps_reader {
    struct qdc_smaps total;
    long *field;
    unsigned long number;
    size_t key_used;
    char key[sizeof("Private_Dirty:")];
    enum smaps_state state;
    int negative, seen_rss, line_start, done;
};
static int smaps_number(struct smaps_reader *r) {
    long n = r->negative ? (r->number == (unsigned long)LONG_MAX + 1UL ?
        LONG_MIN : -(long)r->number) : (long)r->number;
    if ((n > 0 && *r->field > LONG_MAX - n) ||
        (n < 0 && *r->field < LONG_MIN - n)) {
        errno = EOVERFLOW; return -1;
    }
    *r->field += n;
    if (r->field == &r->total.rss) r->seen_rss = 1;
    r->state = SMAPS_IGNORE; return 0;
}
static int smaps_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}
static int smaps_byte(struct smaps_reader *r, unsigned char c) {
    if (r->done) return 0;
    if (!c) {
        if (r->state == SMAPS_NUMBER && smaps_number(r)) return -1;
        r->done = 1; return 0;
    }
    if (c == '\n') {
        if (r->state == SMAPS_NUMBER && smaps_number(r)) return -1;
        /* Like sscanf's %ld, whitespace after ':' may span newlines. */
        if (r->state != SMAPS_SPACE) r->state = SMAPS_KEY;
        r->key_used = 0; r->line_start = 1; return 0;
    }
    int line_start = r->line_start; r->line_start = 0;
    if (r->state == SMAPS_SPACE) {
        if (smaps_space(c)) return 0;
        r->number = 0; r->negative = c == '-';
        if (c == '-' || c == '+') { r->state = SMAPS_SIGN; return 0; }
        if (c >= '0' && c <= '9') r->state = SMAPS_NUMBER;
        else {
            /* A new field at the start of the next line invalidates the
             * preceding field's missing value, then gets parsed normally. */
            r->state = line_start ? SMAPS_KEY : SMAPS_IGNORE;
        }
    }
    if (r->state == SMAPS_SIGN) {
        r->state = c >= '0' && c <= '9' ? SMAPS_NUMBER : SMAPS_IGNORE;
    }
    if (r->state == SMAPS_NUMBER) {
        if (c >= '0' && c <= '9') {
            unsigned long limit = (unsigned long)LONG_MAX + (unsigned long)r->negative;
            unsigned long digit = (unsigned long)(c - '0');
            if (r->number > (limit - digit) / 10UL) { errno = EOVERFLOW; return -1; }
            r->number = r->number * 10UL + digit; return 0;
        }
        /* Preserve the existing parser's conversion rule: a numeric prefix
         * counts even when the optional trailing text is not " kB". */
        return smaps_number(r);
    }
    if (r->state != SMAPS_KEY) return 0;
    if (r->key_used == sizeof(r->key)-1) { r->state = SMAPS_IGNORE; return 0; }
    r->key[r->key_used++] = (char)c; r->key[r->key_used] = 0;
    if (c != ':') return 0;
    if (!strcmp(r->key, "Rss:")) r->field = &r->total.rss;
    else if (!strcmp(r->key, "Pss:")) r->field = &r->total.pss;
    else if (!strcmp(r->key, "Anonymous:")) r->field = &r->total.anonymous;
    else if (!strcmp(r->key, "Private_Dirty:")) r->field = &r->total.dirty;
    else if (!strcmp(r->key, "Private_Clean:")) r->field = &r->total.clean;
    else { r->state = SMAPS_IGNORE; return 0; }
    r->state = SMAPS_SPACE; return 0;
}
int proc_smaps(pid_t pid, struct qdc_smaps *sm) {
    char text[512], path[80];
    struct smaps_reader reader = {0}; reader.line_start = 1;
    snprintf(path, sizeof(path), "/proc/%ld/smaps", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, text, sizeof(text));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto failed;
        if (!n) break;
        /* Keep read_complete's original 131072-byte-buffer boundary: room
         * for its terminator was reserved, and filling that room failed. */
        if ((size_t)n >= 131071 - used) { errno = EOVERFLOW; goto failed; }
        used += (size_t)n;
        for (ssize_t i = 0; i < n; i++) if (smaps_byte(&reader, (unsigned char)text[i])) goto failed;
    }
    close(fd);
    if (reader.state == SMAPS_NUMBER && smaps_number(&reader)) return -1;
    if (!reader.seen_rss) return -1;
    *sm = reader.total; return 0;
failed: {
        int error = errno; close(fd); errno = error; return -1;
    }
}
