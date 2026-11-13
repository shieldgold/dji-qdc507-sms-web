/* Fault injection for computer-only VMs. Never included in the hardware bundle. */
#define _GNU_SOURCE
#include "proc.h"
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char cmdline[4096];
    if (read_complete("/proc/cmdline", cmdline, sizeof(cmdline)) < 1 || !strstr(cmdline, "qdc.runtime.lab=1")) return 65;
    if (argc == 3 && !strcmp(argv[1], "pidinfo")) {
        struct proc_id id;
        if (proc_identity((pid_t)atoi(argv[2]), &id)) return 1;
        printf("%llu\n", id.start); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fill")) {
        for (int i = 0; i < 160 && memory_available() >= 7600; i++) {
            void *p = malloc(256*1024);
            if (!p) return 1;
            memset(p, i+1, 256*1024);
            __asm__ volatile("" : : "r"(p) : "memory");
        }
        int fd = open("/run/fill.ready", O_WRONLY|O_CREAT|O_EXCL, 0600);
        if (fd < 0) return 1;
        dprintf(fd, "%ld\n", memory_available()); close(fd);
        for (;;) pause();
    }
    if (argc == 3 && !strcmp(argv[1], "block") && !strcmp(argv[2], "/run/qdc-tun-VM01")) {
        int output[2], input[2];
        if (pipe(output) || pipe(input) || fcntl(output[1], F_SETPIPE_SZ, 4096) < 0) return 1;
        pid_t pid = fork(); if (pid < 0) return 1;
        if (!pid) {
            close(output[0]); close(input[1]);
            dup2(input[0], 0); dup2(output[1], 1); dup2(output[1], 2);
            close(input[0]); close(output[1]);
            execl("/bin/qdc-ike-guard", "qdc-ike-guard", "idle", argv[2], "VM_BLOCKED", "1", (char *)NULL);
            _exit(127);
        }
        close(input[0]);
        if (write(input[1], "GO\n", 3) != 3) return 1;
        close(input[1]);
        char text[30000]; size_t used = 0;
        for (;;) {
            ssize_t n = read(output[0], text+used, 1);
            if (n != 1 || used >= sizeof(text)-2) return 1;
            text[++used] = 0;
            if (strstr(text, "IDLE_READY")) break;
        }
        /* Fill the pipe after client startup, then stop consuming all output. */
        if (fcntl(output[1], F_SETFL, O_NONBLOCK)) return 1;
        char fill[4096]; memset(fill, 'x', sizeof(fill));
        while (write(output[1], fill, sizeof(fill)) > 0) {}
        /* PIPE_BUF writes are atomic: fill the remaining sub-page space too. */
        while (write(output[1], fill, 1) > 0) {}
        if (errno != EAGAIN || fcntl(output[1], F_SETFL, 0)) return 1;
        close(output[1]);
        int status = 0; struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (;;) {
            if (waitpid(pid, &status, WNOHANG) == pid) break;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec-start.tv_sec > 30) { kill(pid, SIGKILL); waitpid(pid, &status, 0); return 1; }
            usleep(100000);
        }
        close(output[0]);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 124) {
            printf("BLOCKED_UNEXPECTED_STATUS raw=%d\n", status); return 1;
        }
        char state[2048];
        if (read_complete("/run/qdc-tun-VM01/VM_BLOCKED.state", state, sizeof(state)) < 1) return 1;
        char *p = strstr(state, "charon "); long child; unsigned long long ticks; char exe[512];
        if (!p || sscanf(p, "charon %ld %llu %511s", &child, &ticks, exe) != 3 || proc_matches((pid_t)child, ticks, exe)) return 1;
        puts("BLOCKED_OUTPUT_DEADLINE_OK exit=124 owned_charon_not_live=1"); return 0;
    }
    return 64;
}
