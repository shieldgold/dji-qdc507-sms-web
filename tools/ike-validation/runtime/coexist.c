/* Identity-bound handoff to the existing clock/SMS recovery process. */
#define _GNU_SOURCE
#include "coexist.h"
#include "http_probe.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char sms_exe[] = "/data/smsweb/smsweb";
static int token(const char *s) {
    size_t n = strlen(s);
    return n && n <= 48 && strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == n;
}
static int boot_id(const char *s) {
    if (strlen(s) != 36) return 0;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return 0; }
        else if (!strchr("0123456789abcdef", s[i])) return 0;
    }
    return 1;
}
static int integer(const char *s, unsigned long long *n) {
    if (!*s || strspn(s, "0123456789") != strlen(s)) return -1;
    errno = 0; char *end; *n = strtoull(s, &end, 10);
    return errno || *end || !*n ? -1 : 0;
}
static int process(char *s, struct proc_id *id) {
    char *space = strchr(s, ' '); unsigned long long pid, start;
    if (!space || strchr(space+1, ' ')) return -1;
    *space = 0;
    if (integer(s, &pid) || pid > INT_MAX || integer(space+1, &start)) return -1;
    id->pid = (pid_t)pid; id->start = start; return 0;
}
static int private_ip(const char *s) {
    unsigned a, b, c, d; char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0;
    char canonical[16]; snprintf(canonical, sizeof(canonical), "%u.%u.%u.%u", a,b,c,d);
    return !strcmp(canonical, s) && (a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168));
}
int qdc_coexist_parse(const char *text, struct qdc_coexist *c) {
    if (strlen(text) >= 2048) return -1;
    memset(c, 0, sizeof(*c));
    char copy[2048]; strcpy(copy, text);
    static const char *keys[] = {"QDC_COEXIST_V1", "RUN ", "BOOT ", "TOKEN ", "SUP ", "SMS ", "RESUME ", "HTTP ", "SECONDS "};
    char *p = copy;
    for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        char *end = strchr(p, '\n'); if (!end) return -1; *end = 0;
        size_t len = strlen(keys[i]);
        if (strncmp(p, keys[i], len) || (!i && p[len])) return -1;
        char *value = p+len;
        if (i == 1 || i == 3) {
            if (!token(value)) return -1;
            strcpy(i == 1 ? c->run : c->token, value);
        } else if (i == 2) {
            if (!boot_id(value)) return -1;
            strcpy(c->boot, value);
        } else if (i >= 4 && i <= 6) {
            if (process(value, i == 4 ? &c->supervisor : i == 5 ? &c->original_sms : &c->guardian)) return -1;
        } else if (i == 7) {
            if (!private_ip(value) || strlen(value) >= sizeof(c->http)) return -1;
            strcpy(c->http, value);
        } else if (i == 8) {
            if (strcmp(value, "30")) return -1;
            c->seconds = 30;
        }
        p = end+1;
    }
    char expected[64]; snprintf(expected, sizeof(expected), "%s_COEX", c->run);
    return *p || strcmp(c->token, expected) || c->supervisor.pid == c->original_sms.pid ||
           c->supervisor.pid == c->guardian.pid || c->original_sms.pid == c->guardian.pid ? -1 : 0;
}
static int protected_file(const char *path) {
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode) && !st.st_uid && !(st.st_mode & 0077) && st.st_size < 4096;
}
int qdc_coexist_load(const char *root, const char *request, struct qdc_coexist *c) {
    char path[256], text[2048];
    snprintf(path, sizeof(path), "%s/coexist.control", root);
    if (!protected_file(path) || read_complete(path, text, sizeof(text)) < 1 || qdc_coexist_parse(text, c)) return -1;
    const char *prefix = "/run/qdc-tun-";
    return strncmp(root, prefix, strlen(prefix)) || strcmp(root+strlen(prefix), c->run) || strcmp(request, c->token) ? -1 : 0;
}
static int supervisor(const struct qdc_coexist *c, int stopped) {
    struct proc_id id;
    return supervisor_matches(c->supervisor.pid, c->supervisor.start) && !proc_identity(c->supervisor.pid, &id) &&
           ((id.state == 'T' || id.state == 't') == stopped);
}
static int guardian(const char *root, const struct qdc_coexist *c) {
    char path[256], text[512], expected[512], exe[256]; struct proc_id id;
    snprintf(exe, sizeof(exe), "%s/qdc-ike-guard", root);
    if (!proc_matches(c->guardian.pid, c->guardian.start, exe) || proc_identity(c->guardian.pid, &id) || id.state == 'T' || id.state == 't') return 0;
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)c->guardian.pid);
    ssize_t got = read_complete(path, text, sizeof(text));
    int need = snprintf(expected, sizeof(expected), "%s%cresume-clock%c%ld%c%llu%c300%c", exe, 0, 0,
                        (long)c->supervisor.pid, 0, c->supervisor.start, 0, 0);
    if (got != need || memcmp(text, expected, (size_t)need)) return 0;
    struct timespec now; long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0 || clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    double age = now.tv_sec+now.tv_nsec/1e9-c->guardian.start/(double)hz;
    return age >= 0 && age <= 180;
}
/* Refuse duplicate or unrelated live SMS services, including after restoration. */
static int sms_scan(struct proc_id *found) {
    DIR *dir = opendir("/proc"); if (!dir) return -1;
    int count = 0; struct dirent *ent;
    while ((ent = readdir(dir))) {
        unsigned long long n;
        if (integer(ent->d_name, &n) || n > INT_MAX) continue;
        struct proc_id id;
        if (!proc_identity((pid_t)n, &id) && id.state != 'Z' && !strcmp(id.exe, sms_exe)) {
            *found = id; count++;
        }
    }
    closedir(dir); return count;
}
int qdc_coexist_preflight(const char *root, struct qdc_coexist *c) {
    char text[4096], expected[40], path[256]; struct proc_id id;
    snprintf(expected, sizeof(expected), "%s\n", c->boot);
    if (read_complete("/proc/sys/kernel/random/boot_id", text, sizeof(text)) < 1 || strcmp(text, expected) ||
        !supervisor(c, 1) || !guardian(root, c) || !qdc_http_local_address(c->http) ||
        !proc_identity(c->original_sms.pid, &id) || sms_scan(&id) != 0) return -1;
    snprintf(path, sizeof(path), "%s/sms-guard.log", root);
    if (!protected_file(path) || read_complete(path, text, sizeof(text)) < 1) return -1;
    char ready[128];
    snprintf(ready, sizeof(ready), "RESUME_GUARD_READY pid=%ld supervisor=%ld start=%llu seconds=300 restore_clock=1\n",
             (long)c->guardian.pid, (long)c->supervisor.pid, c->supervisor.start);
    return strcmp(text, ready) ? -1 : 0;
}
int qdc_coexist_begin(const char *root, struct qdc_coexist *c) {
    if (qdc_coexist_preflight(root, c)) return -1;
    /* Recheck PID/start/exe immediately before signalling. Linux 3.18 has no pidfd. */
    if (!guardian(root, c) || kill(c->guardian.pid, SIGTERM)) return -1;
    printf("COEXIST_RESTORE_REQUEST guardian_pid=%ld guardian_start=%llu\n", (long)c->guardian.pid, c->guardian.start);
    return 0;
}
int qdc_coexist_restored(const char *root, struct qdc_coexist *c) {
    char path[256], text[4096]; struct proc_id id;
    if (!supervisor_matches(c->supervisor.pid, c->supervisor.start)) return -1;
    snprintf(path, sizeof(path), "%s/sms-guard.log", root);
    if (!protected_file(path) || read_complete(path, text, sizeof(text)) < 1 || strstr(text, "RECOVERY_REQUIRED") || strstr(text, "RESUME_REFUSED")) return -1;
    char *clock = strstr(text, "\nCLOCK_RESTORED epoch="), *cont = strstr(text, "\nRESUME_SENT verify_sms_health_separately=1\n");
    int count = sms_scan(&id);
    if (count < 0 || count > 1) return -1;
    if (!clock || !cont) {
        /* A running original guardian is permitted only while it finishes restoration. */
        char exe[256]; snprintf(exe, sizeof(exe), "%s/qdc-ike-guard", root);
        return proc_matches(c->guardian.pid, c->guardian.start, exe) ? 0 : -1;
    }
    if (clock >= cont || !strstr(clock, " before_supervisor_cont=1\n") || !supervisor(c, 0)) return -1;
    if (!count) return 0;
    if (id.pid == c->original_sms.pid || id.start <= c->original_sms.start || id.parent != c->supervisor.pid || id.state == 'T' || id.state == 't') return -1;
    c->sms = id; c->active = 1;
    printf("COEXIST_SMS_RESTORED pid=%ld start=%llu parent=%ld clock_before_cont=1\n", (long)id.pid, id.start, (long)id.parent);
    return 1;
}
int qdc_coexist_alive(const struct qdc_coexist *c) {
    struct proc_id id;
    return c->active && supervisor(c, 0) && sms_scan(&id) == 1 && id.pid == c->sms.pid &&
           id.start == c->sms.start && id.parent == c->supervisor.pid && id.state != 'T' && id.state != 't';
}
