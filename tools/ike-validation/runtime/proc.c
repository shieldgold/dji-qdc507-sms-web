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
int proc_smaps(pid_t pid, struct qdc_smaps *sm) {
    /* Reused sequentially: bounded memory, read every VMA to EOF. */
    static char text[131072]; char path[80];
    snprintf(path, sizeof(path), "/proc/%ld/smaps", (long)pid);
    if (read_complete(path, text, sizeof(text)) < 1) return -1;
    return qdc_parse_smaps(text, sm);
}
