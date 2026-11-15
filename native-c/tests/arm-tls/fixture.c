/* Supervisor for synthetic loopback connections only; no hardware access. */
#include "lab.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct child { pid_t pid; int done, code; long rss, pss; };
static long baseline, minimum;
static const char *failure;

static int guard(long deadline) {
    long now = lab_now_ms(), available = lab_available_kib();
    failure = lab_memory_reason(baseline, available);
    if (available >= 0 && available < minimum) minimum = available;
    if (!failure && (now < 0 || now >= deadline)) failure = "deadline";
    return failure ? -1 : 0;
}
static int pause_ms(unsigned ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t)) if (errno != EINTR) return -1;
    return 0;
}
static int capture(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap - 1);
    char extra;
    ssize_t tail = n < 0 ? -1 : read(fd, &extra, 1);
    close(fd);
    if (n < 0 || tail != 0 || memchr(out, 0, (size_t)n)) return -1;
    out[n] = 0;
    return 0;
}
static void sample(struct child *c) {
    if (c->done || c->pid <= 0) return;
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%ld/smaps", (long)c->pid);
    FILE *f = fopen(path, "r");
    if (f) {
        long value, rss = 0, pss = 0;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "Rss: %ld kB", &value) == 1) rss += value;
            if (sscanf(line, "Pss: %ld kB", &value) == 1) pss += value;
        }
        if (rss > c->rss) c->rss = rss;
        if (pss > c->pss) c->pss = pss;
        fclose(f);
    }
}
static int poll_child(struct child *c) {
    if (c->done || c->pid <= 0) return 0;
    int status;
    pid_t result = waitpid(c->pid, &status, WNOHANG);
    if (result < 0) return -1;
    if (result == c->pid) {
        c->done = 1;
        c->code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    return 0;
}
static void stop_child(struct child *c) {
    if (c->pid <= 0 || c->done) return;
    kill(c->pid, SIGTERM);
    for (unsigned i = 0; i < 10; i++) {
        if (poll_child(c) || c->done) break;
        pause_ms(20);
    }
    if (!c->done) {
        kill(c->pid, SIGKILL);
        int status;
        while (waitpid(c->pid, &status, 0) < 0 && errno == EINTR) {}
        c->done = 1;
    }
}
static pid_t spawn(char *const argv[], const char *log, int inherited) {
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    pid_t pid = fork();
    if (!pid) {
        if (inherited >= 0 && fcntl(inherited, F_SETFD, 0) < 0) _exit(126);
        if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(126);
        close(fd);
        execv(argv[0], argv);
        _exit(127);
    }
    close(fd);
    return pid;
}
static int await_ready(struct child *peer, int fd, long deadline) {
    while (!guard(deadline)) {
        sample(peer);
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 1) return c == 'R' ? 0 : -1;
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) return -1;
        if (poll_child(peer) || peer->done || pause_ms(20)) return -1;
    }
    return -1;
}
static int await_children(struct child *peer, struct child *client, long deadline) {
    while (!guard(deadline)) {
        sample(peer); sample(client);
        if (poll_child(peer) || poll_child(client)) return -1;
        if (peer->done && client->done) return 0;
        if (pause_ms(20)) return -1;
    }
    return -1;
}
static int socket_released(void) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(8443),
                                 .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    int yes = 1;
    int rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ||
             bind(fd, (struct sockaddr *)&address, sizeof(address));
    close(fd);
    return rc ? -1 : 0;
}
static int unit(void) {
    char *argv[] = {"/fixtures/test_tls", NULL};
    struct child client = {.pid = spawn(argv, "/run/unit.log", -1)};
    struct child none = {.done = 1};
    int rc = -1;
    char text[4096];
    if (client.pid > 0 && !await_children(&none, &client, lab_now_ms() + 5000) && !client.code &&
        !capture("/run/unit.log", text, sizeof(text)) && strstr(text, "tls unit: PASS")) {
        fputs(text, stdout);
        rc = 0;
    }
    stop_child(&client);
    if (unlink("/run/unit.log")) rc = -1;
    return rc;
}
static int run_case(const char *name) {
    char peer_log[80], client_log[80], fd_text[24];
    snprintf(peer_log, sizeof(peer_log), "/run/%s-peer.log", name);
    snprintf(client_log, sizeof(client_log), "/run/%s-client.log", name);
    int ready[2] = {-1, -1}, rc = -1;
    struct child peer = {0}, client = {0};
    long started = lab_now_ms(), deadline = started + 12000;
    if (pipe2(ready, O_CLOEXEC | O_NONBLOCK)) goto done;
    snprintf(fd_text, sizeof(fd_text), "%d", ready[1]);
    char *peer_argv[] = {"/bin/tls-peer", (char *)name, fd_text, NULL};
    peer.pid = spawn(peer_argv, peer_log, ready[1]);
    close(ready[1]); ready[1] = -1;
    if (peer.pid <= 0 || await_ready(&peer, ready[0], deadline)) goto done;
    close(ready[0]); ready[0] = -1;
    int negative = !strcmp(name, "wrongname") || !strcmp(name, "wrongca");
    char *client_argv[] = {"/fixtures/test_tls", "fetch", "8443",
        !strcmp(name, "wrongca") ? "/fixtures/wrong.pem" : "/fixtures/ca.pem",
        !strcmp(name, "wrongname") ? "wrong.invalid" : "localhost", "1",
        "/3/device/arm-loopback-synthetic", "7000", NULL};
    client.pid = spawn(client_argv, client_log, -1);
    if (client.pid <= 0 || await_children(&peer, &client, deadline)) goto done;
    char server[8192], output[8192];
    if (capture(peer_log, server, sizeof(server)) || capture(client_log, output, sizeof(output)) ||
        peer.code || client.code != negative) goto done;
    if (negative) {
        if (!strstr(server, "QDC_TLS_PEER_CERTIFICATE_REJECT version=TLSv1.2 fatal_alert=1 ") ||
            !strstr(server, "h2_requests=0\n") || strstr(server, "REQUEST_COMPLETE") ||
            strstr(server, "PEER_PASS") ||
            strcmp(output, "TLS rejected: 苹果推送 TLS 验证失败或超时\n")) goto done;
    } else {
        const char *version = !strcmp(name, "tls12") ? "version=TLSv1.2 alpn=h2\n" :
                                                                    "version=TLSv1.3 alpn=h2\n";
        if (!strstr(server, version) ||
            !strstr(server, "QDC_TLS_PEER_REQUEST_COMPLETE method=POST headers=6 body_bytes=2 request_end_stream=1\n") ||
            !strstr(server, "QDC_TLS_PEER_RESPONSE_COMPLETE status=200 body_bytes=12 response_end_stream=1\n") ||
            !strstr(server, "QDC_TLS_PEER_PASS case=") ||
            strncmp(output, "status=200 h2=1 date=", 21) ||
            !strstr(output, " GMT body={\"arm\":true}\n")) goto done;
    }
    printf("QDC_TLS_CASE_BEGIN case=%s\n", name);
    fputs(server, stdout);
    printf("QDC_TLS_CLIENT case=%s\n", name);
    fputs(output, stdout);
    printf("QDC_TLS_SAMPLE case=%s peer_rss_kib=%ld peer_pss_kib=%ld client_rss_kib=%ld client_pss_kib=%ld interval_ms=20\n",
           name, peer.rss, peer.pss, client.rss, client.pss);
    printf("QDC_TLS_CASE_PASS case=%s outcome=%s client_exit=%d peer_exit=%d elapsed_ms=%ld min_available_kib=%ld\n",
           name, negative ? "rejected" : "accepted", client.code, peer.code, lab_now_ms() - started, minimum);
    rc = 0;
done:
    stop_child(&client); stop_child(&peer);
    if (ready[0] >= 0) close(ready[0]);
    if (ready[1] >= 0) close(ready[1]);
    if (rc) {
        char text[8192];
        printf("QDC_TLS_CASE_FAILED case=%s reason=%s peer_exit=%d client_exit=%d\n", name,
               failure ? failure : "peer-client-validation", peer.code, client.code);
        if (!capture(peer_log, text, sizeof(text))) fputs(text, stdout);
        if (!capture(client_log, text, sizeof(text))) fputs(text, stdout);
    }
    if (unlink(peer_log) && errno != ENOENT) rc = -1;
    if (unlink(client_log) && errno != ENOENT) rc = -1;
    if (socket_released()) rc = -1;
    return rc;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (lab_runtime()) { puts("QDC_TLS_FAILED reason=lab-gate-or-kernel"); return 64; }
    if (argc == 3 && !strcmp(argv[1], "--prepare")) {
        char *end;
        errno = 0;
        long epoch = strtol(argv[2], &end, 10);
        struct timespec t = {(time_t)epoch, 0};
        if (errno || *end || epoch < 1704067200 || epoch > 2145916800 ||
            clock_settime(CLOCK_REALTIME, &t)) return 64;
        struct utsname u;
        if (uname(&u)) return 1;
        printf("QDC_TLS_KERNEL release=%s machine=%s epoch=%ld dual_gate=1 network=loopback-only\n",
               u.release, u.machine, epoch);
        return 0;
    }
    if (argc != 2 || strcmp(argv[1], "--run")) return 64;
    alarm(65);
    baseline = minimum = lab_available_kib();
    failure = lab_memory_reason(baseline, baseline);
    unsigned char random[32];
    ssize_t got = getrandom(random, sizeof(random), GRND_NONBLOCK);
    volatile unsigned char *wipe = random;
    for (size_t i = 0; i < sizeof(random); i++) wipe[i] = 0;
    if (failure || got != 32) {
        printf("QDC_TLS_FAILED reason=%s getrandom_return=%ld\n", failure ? failure : "random-not-ready", (long)got);
        return 1;
    }
    printf("QDC_TLS_RUNTIME_BEGIN admission_kib=8192 absolute_floor_kib=6144 max_drop_kib=3072 baseline_available_kib=%ld getrandom_return=32\n", baseline);
    if (unit()) { puts("QDC_TLS_FAILED reason=production-unit"); return 1; }
    const char *cases[] = {"tls12", "tls13", "wrongname", "wrongca"};
    for (unsigned i = 0; i < 4; i++)
        if (run_case(cases[i])) { puts("QDC_TLS_FAILED reason=case"); return 1; }
    if (guard(lab_now_ms() + 1000) || socket_released()) {
        puts("QDC_TLS_FAILED reason=final-guard-or-socket"); return 1;
    }
    puts("QDC_TLS_CLEANUP children=0 sockets=0 files=0");
    printf("QDC_TLS_RUNTIME_PASS cases=4 baseline_available_kib=%ld min_available_kib=%ld\n", baseline, minimum);
    return 0;
}
