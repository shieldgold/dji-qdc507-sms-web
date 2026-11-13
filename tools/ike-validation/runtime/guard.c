/* Prepared on the computer. Running idle/resume on hardware needs authorization. */
#define _GNU_SOURCE
#include "proc.h"
#include "../../tun-validation/runtime/log_watch.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TMPFS_MAGIC 0x01021994
static volatile sig_atomic_t interrupted, daemon_pid, cli_pid, deadline_fired;
static int kmsg_fd = -1, output_fd = -1, state_fd = -1;
static long baseline;
static double sampled;
static uint64_t log_seq;
static int log_seen;
static size_t log_bytes;
static long daemon_peak, cli_peak, min_available = LONG_MAX;

static double monotonic(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) _exit(70);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void signal_stop(int sig) { interrupted = sig; }
static void hard_deadline(int sig) {
    (void)sig;
    /* Works even if stdout is blocked. Only unreaped direct-child groups. */
    if (deadline_fired) {
        if (daemon_pid > 0) kill(-(pid_t)daemon_pid, SIGKILL);
        if (cli_pid > 0) kill(-(pid_t)cli_pid, SIGKILL);
        _exit(124);
    }
    deadline_fired = 1; interrupted = SIGALRM;
    if (daemon_pid > 0) kill(-(pid_t)daemon_pid, SIGTERM);
    if (cli_pid > 0) kill(-(pid_t)cli_pid, SIGTERM);
    alarm(3);
}
static int identifier(const char *s) {
    size_t n = strlen(s);
    return n > 0 && n <= 48 && strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == n;
}
static unsigned long long number(const char *s) {
    if (!*s || strspn(s, "0123456789") != strlen(s)) return 0;
    char *end; errno = 0; unsigned long long n = strtoull(s, &end, 10);
    return errno || *end ? 0 : n;
}
static int trusted_file(const char *path, int executable) {
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 &&
           !(st.st_mode & 0022) && (!executable || (st.st_mode & S_IXUSR));
}
static int trusted_root(const char *root) {
    const char *prefix = "/run/qdc-tun-";
    struct stat st; struct statfs fs;
    if (strncmp(root, prefix, strlen(prefix)) || !identifier(root + strlen(prefix))) return 0;
    if (lstat("/run", &st) || !S_ISDIR(st.st_mode) || st.st_uid || (st.st_mode & 0022)) return 0;
    return !lstat(root, &st) && S_ISDIR(st.st_mode) && !st.st_uid && (st.st_mode & 0777) == 0700 &&
           !statfs(root, &fs) && (unsigned long)fs.f_type == TMPFS_MAGIC;
}
static int ns_name(char *text, size_t cap) {
    ssize_t n = readlink("/proc/self/ns/net", text, cap-1);
    if (n < 1 || n == (ssize_t)cap-1) return -1;
    text[n] = 0; return 0;
}
static int interfaces(int with_tun) {
    struct if_nameindex *list = if_nameindex();
    if (!list) return -1;
    int lo = 0, tun = 0, bad = 0;
    for (struct if_nameindex *p = list; p->if_index; p++) {
        printf("NS_INTERFACE name=%s index=%u\n", p->if_name, p->if_index);
        if (!strcmp(p->if_name, "lo")) lo++;
        else if (!strcmp(p->if_name, "ipsec0")) tun++;
        else bad++;
    }
    if_freenameindex(list);
    return lo == 1 && tun == with_tun && !bad ? 0 : -1;
}
static int wait_go(void) {
    char in[4]; size_t n = 0; double until = monotonic() + 5;
    while (!interrupted && monotonic() < until && n < sizeof(in)) {
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        int rc = poll(&p, 1, 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return -1;
        if (!rc) continue;
        if (read(STDIN_FILENO, in+n, 1) != 1) return -1;
        if (in[n++] == '\n') return n == 3 && !memcmp(in, "GO\n", 3) ? 0 : -1;
    }
    return -1;
}
static int drain(void) {
    char buf[8193];
    for (int i = 0; i < 64; i++) {
        ssize_t n = read(kmsg_fd, buf, sizeof(buf)-1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) { puts("STOP kmsg_read_error_or_overrun"); return -1; }
        buf[n] = 0; printf("KMSG %s", buf); log_bytes += (size_t)n;
        if (qdc_log_problem(buf, &log_seq, &log_seen) || strstr(buf, "Kernel panic") ||
            strstr(buf, "oom-killer") || strstr(buf, "Killed process") || log_bytes > 1048576) {
            puts("STOP kmsg_problem_or_gap"); return -1;
        }
    }
    if (output_fd >= 0) for (int i = 0; i < 32; i++) {
        ssize_t n = read(output_fd, buf, sizeof(buf)-1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0) return -1;
        if (!n) break;
        buf[n] = 0; printf("APP %s", buf); log_bytes += (size_t)n;
        if (log_bytes > 1048576) { puts("STOP log_budget"); return -1; }
    }
    return 0;
}
static int sample(pid_t pid, const char *label, long *peak) {
    if (pid < 1) return 0;
    struct qdc_smaps sm;
    if (proc_smaps(pid, &sm)) {
        /* A CLI can finish between waitpid and smaps. Its rusage is collected. */
        if (!strcmp(label, "cli")) return 0;
        printf("STOP smaps_unavailable label=%s\n", label); return -1;
    }
    if (sm.rss > *peak) *peak = sm.rss;
    printf("SMAPS label=%s pid=%ld rss_kib=%ld pss_kib=%ld anonymous_kib=%ld dirty_kib=%ld\n",
           label, (long)pid, sm.rss, sm.pss, sm.anonymous, sm.dirty);
    return 0;
}
static int tick(void) {
    if (interrupted || drain()) return -1;
    long avail = memory_available();
    if (avail < min_available) min_available = avail;
    if (!memory_safe(avail, baseline)) {
        printf("STOP memory available_kib=%ld baseline_kib=%ld\n", avail, baseline); return -1;
    }
    double now = monotonic();
    if (now - sampled >= 1) {
        sampled = now;
        printf("MEM monotonic=%.3f available_kib=%ld baseline_kib=%ld\n", now, avail, baseline);
        if (sample(getpid(), "guard", &(long){0}) ||
            sample((pid_t)daemon_pid, "charon", &daemon_peak) ||
            sample((pid_t)cli_pid, "cli", &cli_peak)) return -1;
    }
    struct pollfd fds[2] = {{kmsg_fd, POLLIN, 0}, {output_fd, POLLIN, 0}};
    int rc = poll(fds, output_fd >= 0 ? 2 : 1, 250);
    return rc < 0 && errno != EINTR ? -1 : 0;
}
static pid_t reap(volatile sig_atomic_t *slot, int *status, int options) {
    sigset_t mask, old;
    sigemptyset(&mask); sigaddset(&mask, SIGALRM);
    sigprocmask(SIG_BLOCK, &mask, &old);
    struct rusage usage; pid_t pid = (pid_t)*slot;
    pid_t rc = pid > 0 ? wait4(pid, status, options, &usage) : -1;
    if (rc == pid && pid > 0) {
        long *peak = slot == &cli_pid ? &cli_peak : &daemon_peak;
        if (usage.ru_maxrss > *peak) *peak = usage.ru_maxrss;
        *slot = 0;
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    return rc;
}
static int stop_child(volatile sig_atomic_t *slot) {
    if (*slot < 1) return 0;
    kill(-(pid_t)*slot, SIGTERM);
    int status; double until = monotonic() + 4;
    while (monotonic() < until) {
        if (reap(slot, &status, WNOHANG) > 0) return 0;
        if (drain()) break;
        struct timespec d = {0, 100000000}; nanosleep(&d, NULL);
    }
    if (*slot > 0) kill(-(pid_t)*slot, SIGKILL);
    until = monotonic() + 1;
    while (monotonic() < until) {
        if (reap(slot, &status, WNOHANG) > 0) return 0;
        struct timespec d = {0, 100000000}; nanosleep(&d, NULL);
    }
    printf("RECOVERY_REQUIRED unreaped_child=%ld\n", (long)*slot); return -1;
}
static int spawn(const char *root, int daemon, const char *arg) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", root, daemon ? "charon" : "swanctl");
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) return -1;
    pid_t parent = getpid(), pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (!pid) {
        if (setsid() < 0 || prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
        signal(SIGALRM, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL);
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) _exit(125);
        close(pipefd[1]);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0) _exit(125);
        if (nullfd > 2) close(nullfd);
        if (daemon) execl(path, path, (char *)NULL);
        else execl(path, path, arg, (char *)NULL);
        perror("exec"); _exit(127);
    }
    close(pipefd[1]);
    /* stdout pipe is replaced only after the previous child output was drained. */
    if (daemon) daemon_pid = pid; else cli_pid = pid;
    struct proc_id id;
    if (!proc_identity(pid, &id)) {
        printf("OWNED label=%s pid=%ld start=%llu expected_exe=%s\n", daemon ? "charon" : "cli", (long)pid, id.start, path);
        if (state_fd >= 0) dprintf(state_fd, "%s %ld %llu %s\n", daemon ? "charon" : "cli", (long)pid, id.start, path);
    }
    if (daemon) {
        output_fd = pipefd[0];
        if (fcntl(output_fd, F_SETFL, O_NONBLOCK)) return -1;
        return 0;
    }
    /* VICI writes use a distinct bounded pipe, drained alongside daemon logs. */
    int cli_fd = pipefd[0];
    if (fcntl(cli_fd, F_SETFL, O_NONBLOCK)) { close(cli_fd); return -1; }
    double until = monotonic() + 5; int status = 0, result = -1;
    while (monotonic() < until) {
        char buf[4097]; ssize_t n = read(cli_fd, buf, sizeof(buf)-1);
        if (n > 0) { buf[n] = 0; printf("VICI %s", buf); log_bytes += (size_t)n; }
        if (n < 0 && errno != EAGAIN && errno != EINTR) break;
        if (log_bytes > 1048576 || tick()) break;
        if (reap(&cli_pid, &status, WNOHANG) > 0) {
            /* Drain final data after exit as well. */
            while ((n = read(cli_fd, buf, sizeof(buf)-1)) > 0) {
                buf[n] = 0; printf("VICI %s", buf); log_bytes += (size_t)n;
                if (log_bytes > 1048576) break;
            }
            result = WIFEXITED(status) && !WEXITSTATUS(status) && log_bytes <= 1048576 ? 0 : -1;
            break;
        }
    }
    if (cli_pid > 0) stop_child(&cli_pid);
    close(cli_fd);
    printf("VICI_EXIT command=%s success=%d\n", arg, result == 0); return result;
}
static int create_runtime(const char *token) {
    if (mkdir("/run/qdc-ike", 0700)) return -1;
    int fd = open("/run/qdc-ike/.owner", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { rmdir("/run/qdc-ike"); return -1; }
    int rc = dprintf(fd, "%s\n", token) < 0 ? -1 : 0; close(fd); return rc;
}
static int cleanup_runtime(const char *token) {
    char text[128];
    ssize_t n = read_complete("/run/qdc-ike/.owner", text, sizeof(text));
    if (n != (ssize_t)strlen(token)+1 || strncmp(text, token, strlen(token)) || text[n-1] != '\n') return -1;
    /* Only names inside the exclusively created directory; never recursive rm. */
    if (unlink("/run/qdc-ike/charon.pid") && errno != ENOENT) return -1;
    if (unlink("/run/qdc-ike/charon.vici") && errno != ENOENT) return -1;
    DIR *dir = opendir("/run/qdc-ike"); if (!dir) return -1;
    int unexpected = 0; struct dirent *ent;
    while ((ent = readdir(dir))) if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..") && strcmp(ent->d_name, ".owner")) unexpected = 1;
    closedir(dir);
    if (unexpected || unlink("/run/qdc-ike/.owner") || rmdir("/run/qdc-ike")) return -1;
    return 0;
}
static int idle(const char *root, const char *token, int seconds) {
    char path[256], before[128], after[128]; struct stat st; struct rlimit limit;
    if (geteuid() || !trusted_root(root)) return 65;
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/%s", root, i == 0 ? "charon" : i == 1 ? "swanctl" : "strongswan.conf");
        if (!trusted_file(path, i < 2)) return 65;
    }
    if (!lstat("/run/qdc-ike", &st) || errno != ENOENT) return 65;
    char device[64];
    if (read_complete("/sys/class/misc/tun/dev", device, sizeof(device)) < 1 || strcmp(device, "10:200\n") ||
        lstat("/dev/net/tun", &st) || !S_ISCHR(st.st_mode) || st.st_uid ||
        major(st.st_rdev) != 10 || minor(st.st_rdev) != 200) return 65;
    baseline = memory_available();
    if (baseline < 8192) { printf("PREFLIGHT_MEMORY_REFUSED available_kib=%ld required_kib=8192\n", baseline); return 87; }
    limit = (struct rlimit){0, 0}; if (setrlimit(RLIMIT_CORE, &limit)) return 65;
    limit = (struct rlimit){64, 64}; if (setrlimit(RLIMIT_NOFILE, &limit)) return 65;
    limit = (struct rlimit){512*1024, 512*1024}; if (setrlimit(RLIMIT_STACK, &limit)) return 65;
    limit = (struct rlimit){64*1024*1024, 64*1024*1024}; if (setrlimit(RLIMIT_AS, &limit)) return 65;
    kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kmsg_fd < 0 || lseek(kmsg_fd, 0, SEEK_END) < 0) return 65;
    printf("LOGGER_READY %s pid=%ld baseline_kib=%ld\n", token, (long)getpid(), baseline);
    if (wait_go()) { puts("LOGGER_NOT_ARMED"); close(kmsg_fd); return 66; }
    if (memory_available() < 8192 || !memory_safe(memory_available(), baseline)) {
        puts("PREFLIGHT_MEMORY_REFUSED after_GO=1"); close(kmsg_fd); return 87;
    }
    signal(SIGALRM, hard_deadline); alarm((unsigned)(seconds + 20));
    int rc = 1, created = 0, ns_entered = 0;
    snprintf(path, sizeof(path), "%s/%s.state", root, token);
    state_fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct proc_id self;
    if (state_fd < 0 || proc_identity(getpid(), &self)) goto done;
    dprintf(state_fd, "guard %ld %llu %s\n", (long)getpid(), self.start, self.exe);
    if (ns_name(before, sizeof(before)) || unshare(CLONE_NEWNET) || ns_name(after, sizeof(after)) || !strcmp(before, after)) goto done;
    ns_entered = 1;
    printf("NETNS original=%s private=%s\n", before, after);
    if (interfaces(0) || create_runtime(token)) goto done;
    created = 1;
    snprintf(path, sizeof(path), "%s/strongswan.conf", root);
    if (setenv("STRONGSWAN_CONF", path, 1) || spawn(root, 1, NULL)) goto done;
    double until = monotonic()+8; int status;
    while (monotonic() < until) {
        if (tick()) goto done;
        if (reap(&daemon_pid, &status, WNOHANG) > 0) { puts("STOP charon_early_exit"); goto done; }
        if (!lstat("/run/qdc-ike/charon.vici", &st) && S_ISSOCK(st.st_mode)) break;
    }
    if (lstat("/run/qdc-ike/charon.vici", &st) || !S_ISSOCK(st.st_mode) || interfaces(1) ||
        spawn(root, 0, "--stats") || spawn(root, 0, "--list-conns") || spawn(root, 0, "--list-sas")) goto done;
    puts("IDLE_READY no_connections_configured=1");
    until = monotonic()+seconds;
    while (monotonic() < until) {
        if (tick()) goto done;
        if (reap(&daemon_pid, &status, WNOHANG) > 0) { puts("STOP charon_early_exit"); goto done; }
    }
    if (spawn(root, 0, "--stats")) goto done;
    rc = 0;
done:
    { int cli_stop = stop_child(&cli_pid), daemon_stop = stop_child(&daemon_pid);
      if (cli_stop || daemon_stop) rc = 1; }
    if (daemon_pid || cli_pid) {
        puts("RECOVERY_REQUIRED children_not_reaped runtime_retained=1");
        /* Keep hard deadline active until process exit. Do not unlink their files. */
        return 1;
    }
    if (drain()) rc = 1;
    if (output_fd >= 0) { close(output_fd); output_fd = -1; }
    if (ns_entered && interfaces(0)) { puts("RECOVERY_REQUIRED tun_interface_retained"); rc = 1; }
    if (created && cleanup_runtime(token)) { puts("RECOVERY_REQUIRED runtime_directory_retained"); rc = 1; }
    double post = monotonic()+2;
    while (monotonic() < post) {
        if (drain()) { rc = 1; break; }
        struct timespec d = {0, 100000000}; nanosleep(&d, NULL);
    }
    alarm(0); close(kmsg_fd); if (state_fd >= 0) close(state_fd);
    printf("MEMORY_SUMMARY charon_peak_kib=%ld cli_peak_kib=%ld min_available_kib=%ld smaps_read_to_eof=1\n", daemon_peak, cli_peak, min_available);
    printf("IDLE_COMPLETE rc=%d children_reaped=1\n", rc);
    return rc;
}
static int resume(pid_t pid, unsigned long long start, int seconds) {
    if (geteuid() || !supervisor_matches(pid, start)) return 65;
    signal(SIGHUP, SIG_IGN); signal(SIGPIPE, SIG_IGN);
    printf("RESUME_GUARD_READY pid=%ld supervisor=%ld start=%llu seconds=%d\n", (long)getpid(), (long)pid, start, seconds);
    double until = monotonic()+seconds;
    while (!interrupted && monotonic() < until) {
        struct timespec d = {0, 100000000}; nanosleep(&d, NULL);
    }
    if (!supervisor_matches(pid, start)) { puts("RESUME_REFUSED identity_changed"); return 1; }
    if (kill(pid, SIGCONT)) return 1;
    puts("RESUME_SENT verify_sms_health_separately=1"); return 0;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGTERM, signal_stop); signal(SIGINT, signal_stop); signal(SIGHUP, signal_stop); signal(SIGPIPE, signal_stop);
    if (argc == 2 && !strcmp(argv[1], "selftest")) {
        struct proc_id id; struct qdc_smaps sm;
        if (proc_identity(getpid(), &id) || !proc_matches(getpid(), id.start, id.exe) ||
            proc_matches(getpid(), id.start+1, id.exe) || proc_smaps(getpid(), &sm) || memory_available() < 0) return 1;
        printf("SELFTEST_OK pid=%ld start=%llu rss_kib=%ld\n", (long)getpid(), id.start, sm.rss); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "describe")) {
        unsigned long long pid = number(argv[2]); struct proc_id id;
        if (!pid || pid > INT_MAX || proc_identity((pid_t)pid, &id)) return 1;
        printf("IDENTITY pid=%ld start=%llu state=%c exe=%s\n", (long)id.pid, id.start, id.state, id.exe);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "identity")) {
        unsigned long long pid = number(argv[2]), start = number(argv[3]);
        if (!pid || pid > INT_MAX || !start || argv[4][0] != '/') return 64;
        return proc_matches((pid_t)pid, start, argv[4]) ? 0 : 1;
    }
    if (argc == 5 && !strcmp(argv[1], "idle")) {
        unsigned long long seconds = number(argv[4]);
        if (!identifier(argv[3]) || !seconds || seconds > 60) return 64;
        return idle(argv[2], argv[3], (int)seconds);
    }
    if (argc == 5 && !strcmp(argv[1], "resume")) {
        unsigned long long pid = number(argv[2]), start = number(argv[3]), seconds = number(argv[4]);
        if (!pid || pid > INT_MAX || !start || !seconds || seconds > 300) return 64;
        return resume((pid_t)pid, start, (int)seconds);
    }
    fputs("usage: qdc-ike-guard selftest | describe PID | identity PID START EXE | idle RAM_ROOT TOKEN 1..60 | resume SUP_PID START 1..300\n", stderr);
    return 64;
}
