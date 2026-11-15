#define _GNU_SOURCE
#include <errno.h>
#include <linux/capability.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int lab(void) {
    char line[4096];
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return 0;
    int ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok) return 0;
    for (char *p = strtok(line, " \n"); p; p = strtok(NULL, " \n"))
        if (!strcmp(p, "qdc.clockfailure.lab=1")) return geteuid() == 0;
    return 0;
}
static int drop_clock(void) {
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct data[2];
    unsigned bit = 1U << CAP_SYS_TIME;
    if (prctl(PR_CAPBSET_DROP, CAP_SYS_TIME, 0, 0, 0) ||
        syscall(SYS_capget, &header, data)) return -1;
    data[0].effective &= ~bit;
    data[0].permitted &= ~bit;
    data[0].inheritable &= ~bit;
    if (syscall(SYS_capset, &header, data) || syscall(SYS_capget, &header, data)) return -1;
    int bounding = prctl(PR_CAPBSET_READ, CAP_SYS_TIME, 0, 0, 0);
    if (bounding != 0 || (data[0].effective | data[0].permitted | data[0].inheritable) & bit) {
        errno = EPERM; return -1;
    }
    printf("CLOCK_FAILURE_CAPS uid=0 effective=0 permitted=0 inheritable=0 bounding=0 cap_sys_time=25\n");
    return 0;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!lab()) { fputs("CLOCK_FAILURE_REFUSED non_lab_or_non_root\n", stderr); return 65; }
    if (argc == 2 && !strcmp(argv[1], "supervise")) {
        if (setsid() < 0) { perror("setsid"); return 1; }
        char *args[] = {"/bin/busybox", "/bin/sh", "/data/smsweb/start.sh", "supervise", NULL};
        execv(args[0], args);
    } else if (argc == 5 && (!strcmp(argv[1], "guard") || !strcmp(argv[1], "full-guard"))) {
        if (!strcmp(argv[1], "guard") && drop_clock()) { perror("CLOCK_FAILURE_CAP_DROP"); return 1; }
        char *args[] = {"/fixtures/qdc-ike-guard", "resume-clock", argv[2], argv[3], argv[4], NULL};
        execv(args[0], args);
    } else { fputs("usage: clock-failure-launcher supervise | [full-]guard PID START SECONDS\n", stderr); return 64; }
    perror("execv"); return 1;
}
