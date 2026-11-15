#define _GNU_SOURCE
#include "qdc_audio.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
typedef struct {
    qdc_app *app;
    pthread_mutex_t mu;
    pthread_cond_t idle;
    pthread_t listener;
    bool started, listening;
    int listen_fd, active_fd, fds[3];
    unsigned workers, port;
    char helper[QDC_PATH_MAX], pcm[QDC_PATH_MAX], marker[QDC_PATH_MAX], proc[QDC_PATH_MAX],
        log[QDC_PATH_MAX];
    char ticket[49], call_id[128], state[24], error[256];
    uint64_t expires;
    uint64_t up, down;
} audio;
typedef struct {
    audio *a;
    unsigned slot;
    int fd;
} audio_conn;
typedef struct {
    pid_t pid;
    int input, output, log;
} audio_child;
typedef struct {
    int fd;
    size_t remaining;
    char tail[256];
    size_t tail_len;
    bool ready;
} audio_log;
typedef struct {
    unsigned char in[320], queue[4][320], down[320];
    size_t in_len, head, count, write_off, down_len;
    uint64_t last_read;
} audio_frames;

bool qdc_calibration_alive(const char *marker, const char *proc_root) {
    qdc_error e = {0};
    qdc_buf b = {0};
    bool alive = false;
    char pid[24], start[32], extra[2];
    if (qdc_read_file(marker, 256, &b, &e) || !b.data)
        goto done;
    if (sscanf((char *)b.data, "%23s %31s %1s", pid, start, extra) != 2)
        goto done;
    for (char *p = pid; *p; p++)
        if (*p < '0' || *p > '9')
            goto done;
    if (strtoul(pid, NULL, 10) < 1)
        goto done;
    for (char *p = start; *p; p++)
        if (*p < '0' || *p > '9')
            goto done;
    char base[QDC_PATH_MAX], path[QDC_PATH_MAX];
    if (qdc_path(base, sizeof(base), proc_root, pid) || qdc_path(path, sizeof(path), base, "stat"))
        goto done;
    qdc_buf_free(&b);
    if (qdc_read_file(path, 32768, &b, &e) || !b.data)
        goto done;
    char *end = strrchr((char *)b.data, ')');
    if (!end)
        goto done;
    char *save = NULL, *field = strtok_r(end + 1, " \t\r\n", &save);
    if (!field || !strcmp(field, "Z"))
        goto done;
    for (unsigned i = 0; i < 19; i++) {
        field = strtok_r(NULL, " \t\r\n", &save);
        if (!field)
            goto done;
    }
    if (strcmp(field, start))
        goto done;
    qdc_buf_free(&b);
    if (qdc_path(path, sizeof(path), base, "cmdline"))
        goto done;
    if (qdc_read_file(path, 32768, &b, &e) || !b.data)
        goto done;
    const char expected[] = "/usr/bin/alsaucm_test";
    size_t first = strnlen((char *)b.data, b.len);
    alive = first == sizeof(expected) - 1 && !memcmp(b.data, expected, first);
done:
    qdc_buf_free(&b);
    return alive;
}
static void state(audio *a, const char *s, const char *error) {
    pthread_mutex_lock(&a->mu);
    snprintf(a->state, sizeof(a->state), "%s", s);
    snprintf(a->error, sizeof(a->error), "%s", error);
    pthread_mutex_unlock(&a->mu);
}
bool qdc_audio_available(qdc_app *app) {
    audio *a = app->audio;
    if (!a)
        return false;
    pthread_mutex_lock(&a->mu);
    bool listening = a->listening;
    pthread_mutex_unlock(&a->mu);
    struct stat st;
    return listening && !stat(a->helper, &st) && !stat(a->pcm, &st) &&
           qdc_calibration_alive(a->marker, a->proc);
}
yyjson_doc *qdc_audio_snapshot(qdc_app *app) {
    audio *a = app->audio;
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return NULL;
    yyjson_mut_val *v = yyjson_mut_doc_get_root(d);
    bool available = qdc_audio_available(app);
    yyjson_mut_obj_add_bool(d, v, "available", available);
    if (a) {
        pthread_mutex_lock(&a->mu);
        yyjson_mut_obj_add_strcpy(d, v, "state", a->state);
        if (a->error[0])
            yyjson_mut_obj_add_strcpy(d, v, "error", a->error);
        yyjson_mut_obj_add_uint(d, v, "up_bytes", a->up);
        yyjson_mut_obj_add_uint(d, v, "down_bytes", a->down);
        pthread_mutex_unlock(&a->mu);
    } else {
        yyjson_mut_obj_add_str(d, v, "state", "idle");
        yyjson_mut_obj_add_uint(d, v, "up_bytes", 0);
        yyjson_mut_obj_add_uint(d, v, "down_bytes", 0);
    }
    yyjson_doc *out = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    return out;
}
static bool current_call(audio *a, const char *id, bool *active, bool *clean) {
    bool found = false;
    *active = false;
    *clean = false;
    yyjson_doc *d = qdc_calls_snapshot(a->app);
    if (!d)
        return false;
    yyjson_val *root = yyjson_doc_get_root(d), *calls = yyjson_obj_get(root, "calls");
    yyjson_val *error = yyjson_obj_get(root, "error");
    *clean = !yyjson_is_str(error) || !yyjson_get_len(error);
    size_t i, max;
    yyjson_val *v;
    yyjson_arr_foreach(calls, i, max,
                       v) if (qdc_slice_eq(qdc_json_slice(yyjson_obj_get(v, "id")), id)) {
        found = true;
        *active = qdc_slice_eq(qdc_json_slice(yyjson_obj_get(v, "state")), "active");
        break;
    }
    yyjson_doc_free(d);
    return found;
}
static int issue(audio *a, char ticket[49]) {
    yyjson_doc *d = qdc_calls_snapshot(a->app);
    if (!d)
        return -1;
    yyjson_val *r = yyjson_doc_get_root(d), *calls = yyjson_obj_get(r, "calls"),
               *error = yyjson_obj_get(r, "error");
    char id[128];
    bool valid = yyjson_arr_size(calls) == 1 && (!yyjson_is_str(error) || !yyjson_get_len(error)) &&
                 !qdc_json_cstr(yyjson_obj_get(yyjson_arr_get_first(calls), "id"), id, sizeof(id));
    yyjson_doc_free(d);
    if (!valid)
        return -1;
    qdc_error e = {0};
    if (qdc_random_hex(ticket, 24, &e))
        return -1;
    pthread_mutex_lock(&a->mu);
    strcpy(a->ticket, ticket);
    strcpy(a->call_id, id);
    a->expires = qdc_mono_ms() + 30000;
    pthread_mutex_unlock(&a->mu);
    return 0;
}
static bool consume(audio *a, const char *ticket, char id[128]) {
    pthread_mutex_lock(&a->mu);
    bool ok =
        a->ticket[0] && qdc_mono_ms() <= a->expires && qdc_constant_equal(ticket, a->ticket, 48);
    if (ok) {
        a->ticket[0] = 0;
        strcpy(id, a->call_id);
    }
    pthread_mutex_unlock(&a->mu);
    bool active, clean;
    return ok && current_call(a, id, &active, &clean);
}
static int wait_io(int fd, short events, uint64_t until) {
    for (;;) {
        uint64_t now = qdc_mono_ms();
        if (now >= until)
            return -1;
        struct pollfd p = {fd, events, 0};
        int z = poll(&p, 1, (int)(until - now));
        if (z < 0 && errno == EINTR)
            continue;
        if (z <= 0)
            return -1;
        return p.revents & events ? 0 : -1;
    }
}
static int socket_write(int fd, const void *data, size_t len, uint64_t until) {
    const char *p = data;
    size_t off = 0;
    while (off < len) {
        if (wait_io(fd, POLLOUT, until))
            return -1;
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, p + off, len - off, 0);
#endif
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}
static bool dead(audio_child *p) {
    if (!p->pid)
        return true;
    int status;
    pid_t n = waitpid(p->pid, &status, WNOHANG);
    if (n == p->pid || (n < 0 && errno == ECHILD)) {
        p->pid = 0;
        return true;
    }
    return false;
}
static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}
static void stop_child(audio_child *p) {
    close_fd(&p->input);
    close_fd(&p->output);
    close_fd(&p->log);
    if (dead(p))
        return;
    kill(p->pid, SIGTERM);
    uint64_t until = qdc_mono_ms() + 5000;
    while (!dead(p) && qdc_mono_ms() < until) {
        struct timespec t = {0, 20000000};
        nanosleep(&t, NULL);
    }
    if (p->pid) {
        kill(p->pid, SIGKILL);
        while (waitpid(p->pid, NULL, 0) < 0 && errno == EINTR) {
        }
        p->pid = 0;
    }
}
static int make_pipe(int p[2]) {
#ifdef __linux__
    return pipe2(p, O_CLOEXEC);
#else
    if (pipe(p))
        return -1;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}
static void nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f >= 0)
        fcntl(fd, F_SETFL, f | O_NONBLOCK);
}
static int spawn(audio *a, bool pcm, audio_child *out) {
    int in[2] = {-1, -1}, data[2] = {-1, -1}, logs[2] = {-1, -1};
    *out = (audio_child){.input = -1, .output = -1, .log = -1};
    if (make_pipe(logs) || (pcm && (make_pipe(in) || make_pipe(data))))
        goto fail;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (pcm) {
        posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, data[1], STDOUT_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, logs[1], STDOUT_FILENO);
    }
    posix_spawn_file_actions_adddup2(&actions, logs[1], STDERR_FILENO);
    char *route_args[] = {a->helper, "--network-route-session", "--verbose", NULL};
    char *pcm_args[] = {a->helper, "--stdio",          "--no-mixers", "--playback-device",
                        "hw:0,5",  "--capture-device", "hw:0,6",      "--verbose",
                        NULL};
    pid_t child = 0;
    int rc = posix_spawn(&child, a->helper, &actions, NULL, pcm ? pcm_args : route_args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc)
        goto fail;
    close_fd(&logs[1]);
    close_fd(&in[0]);
    close_fd(&data[1]);
    out->pid = child;
    out->input = in[1];
    out->output = data[0];
    out->log = logs[0];
    nonblock(out->log);
    if (pcm) {
        nonblock(out->input);
        nonblock(out->output);
    }
    return 0;
fail:
    close_fd(&in[0]);
    close_fd(&in[1]);
    close_fd(&data[0]);
    close_fd(&data[1]);
    close_fd(&logs[0]);
    close_fd(&logs[1]);
    return -1;
}
static int drain_log(int fd, audio_log *log) {
    unsigned char b[4096];
    size_t drained = 0;
    while (drained < 65536) {
        ssize_t n = read(fd, b, sizeof(b));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return n == 0 ? 0 : -1;
        drained += (size_t)n;
        size_t remaining = log->remaining, take = (size_t)n;
        if (take > remaining)
            take = remaining;
        size_t off = 0;
        while (off < take) {
            ssize_t z = write(log->fd, b + off, take - off);
            if (z < 0 && errno == EINTR)
                continue;
            if (z <= 0)
                return -1;
            off += (size_t)z;
        }
        log->remaining -= take;
        for (ssize_t i = 0; i < n; i++) {
            if (log->tail_len == sizeof(log->tail) - 1) {
                memmove(log->tail, log->tail + 128, log->tail_len - 128);
                log->tail_len -= 128;
            }
            log->tail[log->tail_len++] = (char)b[i];
            log->tail[log->tail_len] = 0;
            if (strstr(log->tail, "VoLTE route session active"))
                log->ready = true;
        }
    }
    return 0;
}
static int read_frames(audio *a, int fd, audio_frames *f, bool keep) {
    for (;;) {
        ssize_t z = recv(fd, f->in + f->in_len, 320 - f->in_len, 0);
        if (z < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (z < 0 && errno == EINTR)
            continue;
        if (z <= 0)
            return -1;
        f->in_len += (size_t)z;
        if (f->in_len == 320) {
            pthread_mutex_lock(&a->mu);
            a->up += 320;
            pthread_mutex_unlock(&a->mu);
            f->last_read = qdc_mono_ms();
            if (keep && f->count < 4) {
                memcpy(f->queue[(f->head + f->count) % 4], f->in, 320);
                f->count++;
            }
            f->in_len = 0;
        }
    }
}
/* Returns 0 when the bound call ended normally, -1 for connection/helper loss. */
static int stream(audio *a, int fd, const char *id, audio_frames *frames) {
    audio_child route = {.input = -1, .output = -1, .log = -1}, pcm = route;
    audio_log log = {.fd = -1, .remaining = 65536};
    int rc = -1;
    log.fd = open(a->log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (log.fd < 0)
        goto done;
    struct stat st;
    if (fstat(log.fd, &st) || !S_ISREG(st.st_mode) || fchmod(log.fd, 0600))
        goto done;
    if (spawn(a, false, &route))
        goto done;
    uint64_t until = qdc_mono_ms() + 6000;
    while (!atomic_load(&a->app->stopping) && qdc_mono_ms() < until && !log.ready) {
        if (dead(&route) || drain_log(route.log, &log))
            goto done;
        struct pollfd p[2] = {{route.log, POLLIN, 0}, {fd, POLLIN, 0}};
        int n = poll(p, 2, 100);
        if (n < 0 && errno != EINTR)
            goto done;
        if (p[1].revents && (read_frames(a, fd, frames, true)))
            goto done;
        if (qdc_mono_ms() - frames->last_read >= 6000)
            goto done;
    }
    if (!log.ready || spawn(a, true, &pcm))
        goto done;
    state(a, "streaming", "");
    uint64_t check_at = 0;
    while (!atomic_load(&a->app->stopping)) {
        uint64_t now = qdc_mono_ms();
        if (now >= check_at) {
            bool active, clean;
            if (!current_call(a, id, &active, &clean)) {
                rc = 0;
                goto done;
            }
            check_at = now + 250;
        }
        if (now - frames->last_read >= 6000 || dead(&route) || dead(&pcm))
            goto done;
        struct pollfd p[5] = {{fd, POLLIN, 0},
                              {pcm.output, POLLIN, 0},
                              {pcm.input, frames->count ? POLLOUT : 0, 0},
                              {route.log, POLLIN, 0},
                              {pcm.log, POLLIN, 0}};
        int n = poll(p, 5, 100);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            goto done;
        }
        if (p[0].revents && read_frames(a, fd, frames, true))
            goto done;
        if (p[1].revents) {
            ssize_t z = read(pcm.output, frames->down + frames->down_len, 320 - frames->down_len);
            if (z > 0) {
                frames->down_len += (size_t)z;
                if (frames->down_len == 320) {
                    if (socket_write(fd, frames->down, 320, qdc_mono_ms() + 2000))
                        goto done;
                    frames->down_len = 0;
                    pthread_mutex_lock(&a->mu);
                    a->down += 320;
                    pthread_mutex_unlock(&a->mu);
                }
            } else if (!z || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
                goto done;
        }
        if (p[2].revents && frames->count) {
            ssize_t z = write(pcm.input, frames->queue[frames->head] + frames->write_off,
                              320 - frames->write_off);
            if (z > 0) {
                frames->write_off += (size_t)z;
                if (frames->write_off == 320) {
                    frames->write_off = 0;
                    frames->head = (frames->head + 1) % 4;
                    frames->count--;
                }
            } else if (!z || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
                goto done;
        }
        if (p[3].revents && drain_log(route.log, &log))
            goto done;
        if (p[4].revents && drain_log(pcm.log, &log))
            goto done;
    }
done:
    stop_child(&pcm);
    stop_child(&route);
    if (log.fd >= 0)
        close(log.fd);
    return rc;
}
static void serve_conn(audio *a, int fd) {
    char line[128], id[128];
    size_t used = 0;
    uint64_t until = qdc_mono_ms() + 5000;
    while (used < sizeof(line)) {
        if (wait_io(fd, POLLIN, until))
            return;
        ssize_t z = recv(fd, line + used, 1, 0);
        if (z < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            continue;
        if (z <= 0)
            return;
        if (line[used++] == '\n')
            break;
    }
    if (used != 54 || memcmp(line, "PCM1 ", 5) || line[53] != '\n')
        return;
    line[53] = 0;
    if (!consume(a, line + 5, id))
        return;
    pthread_mutex_lock(&a->mu);
    if (a->active_fd >= 0) {
        pthread_mutex_unlock(&a->mu);
        return;
    }
    a->active_fd = fd;
    a->up = a->down = 0;
    strcpy(a->state, "waiting");
    a->error[0] = 0;
    pthread_mutex_unlock(&a->mu);
    bool hangup = true;
    if (socket_write(fd, "OK\n", 3, until))
        goto done;
    audio_frames frames = {.last_read = qdc_mono_ms()};
    uint64_t start = qdc_mono_ms();
    while (!atomic_load(&a->app->stopping)) {
        bool active, clean;
        if (!current_call(a, id, &active, &clean)) {
            hangup = false;
            state(a, "idle", "");
            goto done;
        }
        if (active && clean)
            break;
        struct pollfd p = {fd, POLLIN, 0};
        int n = poll(&p, 1, 100);
        if (n < 0 && errno != EINTR)
            goto done;
        if (p.revents && read_frames(a, fd, &frames, false)) {
            state(a, "idle", "");
            goto done;
        }
        if (qdc_mono_ms() - frames.last_read >= 6000) {
            state(a, "idle", "");
            goto done;
        }
        if (qdc_mono_ms() - start > 90000) {
            state(a, "failed", "等待接通超时");
            goto done;
        }
    }
    if (!stream(a, fd, id, &frames)) {
        hangup = false;
        state(a, "idle", "");
    } else
        state(a, "failed", "音频连接已中断，正在结束本次电话");
done:
    if (hangup) {
        qdc_error e = {0};
        qdc_calls_hangup(a->app, id, &e);
    }
    pthread_mutex_lock(&a->mu);
    a->active_fd = -1;
    pthread_mutex_unlock(&a->mu);
}
static void *handler(void *p) {
    audio_conn *c = p;
    audio *a = c->a;
    int fd = c->fd;
    unsigned slot = c->slot;
    free(c);
    serve_conn(a, fd);
    pthread_mutex_lock(&a->mu);
    a->fds[slot] = -1;
    a->workers--;
    close(fd);
    pthread_cond_broadcast(&a->idle);
    pthread_mutex_unlock(&a->mu);
    return NULL;
}
static void *listener(void *p) {
    audio *a = p;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    while (!atomic_load(&a->app->stopping)) {
        struct pollfd pfd = {a->listen_fd, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0)
            continue;
        int fd = accept(a->listen_fd, NULL, NULL);
        if (fd < 0)
            continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        nonblock(fd);
#ifdef SO_NOSIGPIPE
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
        pthread_mutex_lock(&a->mu);
        unsigned slot = 0;
        while (slot < 3 && a->fds[slot] >= 0)
            slot++;
        audio_conn *c = slot < 3 ? malloc(sizeof(*c)) : NULL;
        if (!c) {
            pthread_mutex_unlock(&a->mu);
            close(fd);
            continue;
        }
        *c = (audio_conn){a, slot, fd};
        a->fds[slot] = fd;
        a->workers++;
        pthread_t t;
        if (pthread_create(&t, &attr, handler, c)) {
            a->fds[slot] = -1;
            a->workers--;
            free(c);
            close(fd);
        }
        pthread_mutex_unlock(&a->mu);
    }
    pthread_attr_destroy(&attr);
    pthread_mutex_lock(&a->mu);
    a->listening = false;
    for (unsigned i = 0; i < 3; i++)
        if (a->fds[i] >= 0)
            shutdown(a->fds[i], SHUT_RDWR);
    while (a->workers)
        pthread_cond_wait(&a->idle, &a->mu);
    pthread_mutex_unlock(&a->mu);
    return NULL;
}
int qdc_audio_init(qdc_app *app, qdc_error *e) {
    audio *a = calloc(1, sizeof(*a));
    if (!a)
        return -1;
    a->app = app;
    a->active_fd = a->listen_fd = -1;
    for (unsigned i = 0; i < 3; i++)
        a->fds[i] = -1;
    a->port = 8788;
    strcpy(a->state, "idle");
    if (qdc_path(a->helper, sizeof(a->helper), app->config.data_dir,
                 "voice-runtime/network-pcm-bridge")) {
        free(a);
        qdc_error_set(e, "语音路径过长");
        return -1;
    }
    strcpy(a->pcm, "/dev/snd/pcmC0D6c");
    strcpy(a->marker, "/tmp/smsweb-voice-ready");
    strcpy(a->proc, "/proc");
    strcpy(a->log, "/tmp/smsweb-voice.log");
    pthread_mutex_init(&a->mu, NULL);
    pthread_cond_init(&a->idle, NULL);
    app->audio = a;
    return 0;
}
int qdc_audio_start(qdc_app *app, qdc_error *e) {
    audio *a = app->audio;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        goto fail;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)a->port)};
    if (inet_pton(AF_INET, app->config.listen, &addr.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 3)) {
        close(fd);
        goto fail;
    }
    a->listen_fd = fd;
    a->listening = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    int rc = pthread_create(&a->listener, &attr, listener, a);
    pthread_attr_destroy(&attr);
    if (rc) {
        a->listening = false;
        close(fd);
        a->listen_fd = -1;
        goto fail;
    }
    a->started = true;
    return 0;
fail:
    qdc_error_set(e, "语音监听暂不可用");
    return -1;
}
void qdc_audio_free(qdc_app *app) {
    audio *a = app->audio;
    if (!a)
        return;
    if (a->started)
        pthread_join(a->listener, NULL);
    if (a->listen_fd >= 0)
        close(a->listen_fd);
    pthread_cond_destroy(&a->idle);
    pthread_mutex_destroy(&a->mu);
    free(a);
    app->audio = NULL;
}
int qdc_audio_route(qdc_app *app, qdc_request *r, qdc_response *s) {
    if (strcmp(r->method, "POST") || strcmp(r->path, "/api/calls/audio"))
        return 0;
    if (!qdc_audio_available(app))
        return qdc_response_error(s, 409, "模块语音运行时尚未准备好") < 0 ? -1 : 1;
    char ticket[49];
    audio *a = app->audio;
    if (issue(a, ticket))
        return qdc_response_error(s, 409, "请等待当前电话状态确认后连接音频") < 0 ? -1 : 1;
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return -1;
    yyjson_mut_val *v = yyjson_mut_doc_get_root(d);
    yyjson_mut_obj_add_strcpy(d, v, "ticket", ticket);
    yyjson_mut_obj_add_uint(d, v, "port", a->port);
    yyjson_mut_obj_add_uint(d, v, "sample_rate", 8000);
    yyjson_mut_obj_add_uint(d, v, "channels", 1);
    yyjson_mut_obj_add_str(d, v, "format", "s16le");
    yyjson_mut_obj_add_uint(d, v, "frame_bytes", 320);
    yyjson_mut_obj_add_uint(d, v, "expires_in", 30);
    return qdc_response_json(s, 200, d) < 0 ? -1 : 1;
}
#ifdef QDC_TESTING
int qdc_audio_test_paths(qdc_app *app, const char *pcm, const char *marker, const char *proc,
                         const char *log, unsigned port) {
    audio *a = app->audio;
    if (!a || a->started || port < 1 || port > 65535 || strlen(pcm) >= sizeof(a->pcm) ||
        strlen(marker) >= sizeof(a->marker) || strlen(proc) >= sizeof(a->proc) ||
        strlen(log) >= sizeof(a->log))
        return -1;
    strcpy(a->pcm, pcm);
    strcpy(a->marker, marker);
    strcpy(a->proc, proc);
    strcpy(a->log, log);
    a->port = port;
    return 0;
}
#endif
