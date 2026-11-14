/* Test-only fixture. Never included in a hardware bundle. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double monotonic(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) _exit(70);
    return value.tv_sec+value.tv_nsec/1e9;
}
static int read_file(const char *path, char *text, size_t cap) {
    int fd = open(path, O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t count = read(fd, text, cap-1);
    close(fd);
    if (count < 0 || count == (ssize_t)cap-1) return -1;
    text[count] = 0;
    return 0;
}
static int identity(pid_t pid, unsigned long long *start, char *exe, size_t cap) {
    char path[80], text[4096];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    if (read_file(path, text, sizeof(text))) return -1;
    char *p = strrchr(text, ')'), *save = NULL;
    if (!p || p[1] != ' ') return -1;
    int field = 3;
    for (p = strtok_r(p+2, " \n", &save); p; p = strtok_r(NULL, " \n", &save), field++) {
        if (field == 22) { *start = strtoull(p, NULL, 10); break; }
    }
    if (!p || !*start) return -1;
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
    ssize_t size = readlink(path, exe, cap-1);
    if (size < 1 || size == (ssize_t)cap-1) return -1;
    exe[size] = 0;
    return 0;
}
static int matches(pid_t pid, unsigned long long expected, const char *wanted) {
    unsigned long long start; char exe[512];
    return !identity(pid, &start, exe, sizeof(exe)) && start == expected && !strcmp(exe, wanted);
}
static int wait_text(const char *path, const char *wanted, int seconds) {
    static char text[262144];
    double deadline = monotonic()+seconds;
    while (monotonic() < deadline) {
        if (!read_file(path, text, sizeof(text)) && strstr(text, wanted)) return 0;
        usleep(50000);
    }
    return -1;
}
static int sms(void) {
    signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_INET, SOCK_STREAM, 0), reuse = 1;
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) return 1;
    struct sockaddr_in address = {.sin_family=AF_INET, .sin_port=htons(8787), .sin_addr={htonl(INADDR_ANY)}};
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 4)) return 1;
    for (;;) {
        int client = accept(fd, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; return 1; }
        char request[1024]; size_t used = 0; double deadline = monotonic()+2;
        int complete = 0;
        while (monotonic() < deadline && used < sizeof(request)-1) {
            struct pollfd event = {client, POLLIN, 0};
            if (poll(&event, 1, 50) <= 0) continue;
            ssize_t n = recv(client, request+used, sizeof(request)-1-used, MSG_DONTWAIT);
            if (n <= 0) break;
            used += (size_t)n; request[used] = 0;
            if (strstr(request, "\r\n\r\n")) { complete = 1; break; }
        }
        if (complete) {
            int first = open("/run/first-http-epoch", O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0600);
            if (first >= 0) { dprintf(first, "%lld\n", (long long)time(NULL)); close(first); }
            char mode[32]; int failed = !read_file("/run/sms-status", mode, sizeof(mode)) && !strcmp(mode, "503\n");
            const char *response = failed ?
                "HTTP/1.0 503 Unavailable\r\nContent-Length: 13\r\n\r\nlab-sms-ready" :
                "HTTP/1.0 200 OK\r\nContent-Length: 13\r\n\r\nlab-sms-ready";
            if (send(client, response, strlen(response), MSG_NOSIGNAL) < 0 && errno != EPIPE) { close(client); return 1; }
        }
        close(client);
    }
}
static int inject(const char *kind, const char *log, pid_t guard, unsigned long long start) {
    if (wait_text(log, "COEXIST_READY seconds=30", 50) ||
        !matches(guard, start, "/run/qdc-tun-LAB01/qdc-ike-guard")) return 1;
    printf("COEXIST_VM_INJECT %s\n", kind);
    if (!strcmp(kind, "http_failure")) {
        int fd = open("/run/sms-status", O_WRONLY|O_TRUNC|O_CREAT|O_CLOEXEC, 0600);
        if (fd < 0) return 1;
        int rc = dprintf(fd, "503\n") < 0 ? 1 : 0;
        close(fd); return rc;
    }
    if (!strcmp(kind, "signal_abort")) return kill(guard, SIGTERM) ? 1 : 0;
    if (!strcmp(kind, "sms_restart")) {
        char text[64], exe[512]; unsigned long long sms_start;
        if (read_file("/run/sms-child.pid", text, sizeof(text))) return 1;
        pid_t pid = (pid_t)strtol(text, NULL, 10);
        if (pid < 1 || identity(pid, &sms_start, exe, sizeof(exe)) || strcmp(exe, "/data/smsweb/smsweb") ||
            !matches(pid, sms_start, exe)) return 1;
        return kill(pid, SIGTERM) ? 1 : 0;
    }
    if (strcmp(kind, "memory_abort")) return 1;
    volatile unsigned char *pages = malloc(4*1024*1024);
    if (!pages) return 1;
    for (size_t i = 0; i < 4*1024*1024; i += 4096) pages[i] = (unsigned char)i;
    double deadline = monotonic()+40;
    while (monotonic() < deadline && matches(guard, start, "/run/qdc-tun-LAB01/qdc-ike-guard")) usleep(50000);
    free((void *)pages);
    return matches(guard, start, "/run/qdc-tun-LAB01/qdc-ike-guard") ? 1 : 0;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char cmdline[4096];
    if (read_file("/proc/cmdline", cmdline, sizeof(cmdline))) return 77;
    int lab = 0; char *save = NULL;
    for (char *p = strtok_r(cmdline, " \n", &save); p; p = strtok_r(NULL, " \n", &save))
        if (!strcmp(p, "qdc.coexist.lab=1")) lab = 1;
    if (!lab) return 77;
    if (argc == 1) return sms();
    if (argc == 3 && !strcmp(argv[1], "pidinfo")) {
        pid_t pid = (pid_t)strtol(argv[2], NULL, 10); unsigned long long start; char exe[512];
        if (pid < 1 || identity(pid, &start, exe, sizeof(exe))) return 1;
        printf("%llu\n", start); return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "matches"))
        return matches((pid_t)strtol(argv[2], NULL, 10), strtoull(argv[3], NULL, 10), argv[4]) ? 0 : 1;
    if (argc == 5 && !strcmp(argv[1], "wait-text"))
        return wait_text(argv[2], argv[3], atoi(argv[4])) ? 1 : 0;
    if (argc == 6 && !strcmp(argv[1], "inject"))
        return inject(argv[2], argv[3], (pid_t)strtol(argv[4], NULL, 10), strtoull(argv[5], NULL, 10));
    return 64;
}
