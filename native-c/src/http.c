#include "qdc.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd;
    uint64_t deadline;
    unsigned char data[512];
    size_t pos, len;
} http_reader;

static int wait_fd(int fd, short events, uint64_t deadline) {
    for (;;) {
        uint64_t now = qdc_mono_ms();
        if (now >= deadline)
            return -1;
        uint64_t left = deadline - now;
        int timeout = left > INT_MAX ? INT_MAX : (int)left;
        struct pollfd p = {fd, events, 0};
        int n = poll(&p, 1, timeout);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        if (p.revents & events)
            return 0;
        return -1;
    }
}
static ssize_t receive(http_reader *r, void *out, size_t n) {
    if (r->pos < r->len) {
        size_t take = r->len - r->pos;
        if (take > n)
            take = n;
        memcpy(out, r->data + r->pos, take);
        r->pos += take;
        return (ssize_t)take;
    }
    for (;;) {
        if (wait_fd(r->fd, POLLIN, r->deadline))
            return -1;
        ssize_t z = recv(r->fd, r->data, sizeof(r->data), 0);
        if (z < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            continue;
        if (z <= 0)
            return z;
        r->pos = 0;
        r->len = (size_t)z;
        return receive(r, out, n);
    }
}
static int line(http_reader *r, qdc_buf *out) {
    out->len = 0;
    while (out->len < 4096) {
        unsigned char c;
        if (receive(r, &c, 1) != 1)
            return -1;
        if (!c || qdc_buf_append(out, &c, 1))
            return -1;
        if (c == '\n')
            return 0;
    }
    return -1;
}
static int send_all(int fd, const void *data, size_t n, uint64_t deadline) {
    const unsigned char *p = data;
    size_t off = 0;
    while (off < n) {
        if (wait_fd(fd, POLLOUT, deadline))
            return -1;
#ifdef MSG_NOSIGNAL
        ssize_t z = send(fd, p + off, n - off, MSG_NOSIGNAL);
#else
        ssize_t z = send(fd, p + off, n - off, 0);
#endif
        if (z < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (z <= 0)
            return -1;
        off += (size_t)z;
    }
    return 0;
}
static const char *reason(int s) {
    switch (s) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 404:
        return "Not Found";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 429:
        return "Too Many Requests";
    case 503:
        return "Service Unavailable";
    default:
        return "Internal Server Error";
    }
}
static void write_response(int fd, qdc_response *resp, uint64_t deadline) {
    qdc_buf header = {0};
    size_t size = resp->static_body ? resp->static_len : resp->body.len;
    if (!resp->status)
        resp->status = 500;
    if (qdc_buf_printf(&header,
                       "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: "
                       "close\r\nCache-Control: no-store\r\n",
                       resp->status, reason(resp->status),
                       resp->content_type ? resp->content_type : "text/plain; charset=utf-8", size))
        goto done;
    for (size_t i = 0; i < resp->header_count; i++)
        if (qdc_buf_printf(&header, "%s: %s\r\n", resp->headers[i].name, resp->headers[i].value))
            goto done;
    if (qdc_buf_puts(&header, "\r\n"))
        goto done;
    if (send_all(fd, header.data, header.len, deadline) == 0)
        send_all(fd, resp->static_body ? resp->static_body : resp->body.data, size, deadline);
done:
    qdc_buf_free(&header);
}
static void plain(qdc_response *r, int status, const char *message) {
    r->status = status;
    r->content_type = "text/plain; charset=utf-8";
    qdc_buf_puts(&r->body, message);
}
static void trim_line(qdc_buf *b) {
    while (b->len && (b->data[b->len - 1] == '\r' || b->data[b->len - 1] == '\n'))
        b->data[--b->len] = 0;
}
static char *trim_header(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    return s;
}

void qdc_http_conn(qdc_app *app, int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
        close(fd);
        return;
    }
#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    http_reader reader = {.fd = fd, .deadline = qdc_mono_ms() + 30000};
    qdc_buf first = {0}, h = {0}, body = {0}, cookie = {0};
    qdc_request req = {0};
    qdc_response resp = {0};
    if (line(&reader, &first))
        goto done;
    trim_line(&first);
    char *method = (char *)first.data, *path = strchr(method, ' ');
    if (!path)
        goto done;
    *path++ = 0;
    char *version = strchr(path, ' ');
    if (!version)
        goto done;
    *version++ = 0;
    if (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))
        goto done;
    if (!*method || *path != '/')
        goto done;
    req.method = method;
    req.path = path;
    req.query = "";
    char *query = strchr(path, '?');
    if (query) {
        *query++ = 0;
        req.query = query;
    }
    size_t header_bytes = 0, content_length = 0;
    bool have_length = false;
    for (;;) {
        if (line(&reader, &h))
            goto done;
        header_bytes += h.len;
        if (header_bytes > 8192)
            goto done;
        trim_line(&h);
        if (!h.len)
            break;
        char *key = (char *)h.data, *colon = strchr(key, ':');
        if (!colon || colon == key) {
            plain(&resp, 400, "invalid header");
            goto reply;
        }
        *colon++ = 0;
        key = trim_header(key);
        char *value = trim_header(colon);
        if (!strcasecmp(key, "transfer-encoding") && *value) {
            plain(&resp, 400, "unsupported transfer encoding");
            goto reply;
        }
        if (!strcasecmp(key, "content-length")) {
            if (have_length || !*value) {
                plain(&resp, 400, "invalid content length");
                goto reply;
            }
            have_length = true;
            for (char *v = value; *v; v++) {
                if (*v < '0' || *v > '9' || content_length > (SIZE_MAX - (size_t)(*v - '0')) / 10) {
                    plain(&resp, 400, "invalid content length");
                    goto reply;
                }
                content_length = content_length * 10 + (size_t)(*v - '0');
            }
        } else if (!strcasecmp(key, "cookie")) {
            cookie.len = 0;
            if (qdc_buf_puts(&cookie, value))
                goto done;
        }
    }
    size_t limit = !strcmp(req.path, "/api/restore") ? QDC_RESTORE_MAX : QDC_BODY_MAX;
    if (content_length > limit) {
        plain(&resp, 413, "request too large");
        goto reply;
    }
    if (qdc_buf_reserve(&body, content_length))
        goto done;
    while (body.len < content_length) {
        ssize_t z = receive(&reader, body.data + body.len, content_length - body.len);
        if (z <= 0)
            goto done;
        body.len += (size_t)z;
    }
    body.data[body.len] = 0;
    req.body = (qdc_slice){(char *)body.data, body.len};
    req.cookie = cookie.data ? (char *)cookie.data : "";
    if (qdc_dispatch(app, &req, &resp) < 0) {
        qdc_response_free(&resp);
        plain(&resp, 500, "internal error");
    }
reply:
    write_response(fd, &resp, reader.deadline);
done:
    if (req.json)
        yyjson_doc_free(req.json);
    qdc_response_free(&resp);
    qdc_buf_free(&first);
    qdc_buf_free(&h);
    qdc_buf_free(&body);
    qdc_buf_free(&cookie);
    close(fd);
}

int qdc_listen_address(const char *host, const char *port, qdc_error *e) {
    if (!host || (strcmp(host, "192.168.225.1") && strcmp(host, "127.0.0.1"))) {
        qdc_error_set(e, "只允许监听模块 USB 地址 192.168.225.1 或本机 127.0.0.1");
        return -1;
    }
    if (!port || !*port) {
        qdc_error_set(e, "无效 HTTP 端口");
        return -1;
    }
    unsigned value = 0;
    for (const char *p = port; *p; p++) {
        if (*p < '0' || *p > '9' || value > 6553) {
            qdc_error_set(e, "无效 HTTP 端口");
            return -1;
        }
        value = value * 10 + (unsigned)(*p - '0');
    }
    if (value < 1 || value > 65535) {
        qdc_error_set(e, "无效 HTTP 端口");
        return -1;
    }
    return 0;
}

typedef struct {
    qdc_app *app;
    pthread_mutex_t mu;
    pthread_cond_t done;
    int fds[8];
    unsigned active;
} server_state;
typedef struct {
    server_state *server;
    unsigned slot;
    int fd;
} connection;
static void *worker(void *p) {
    connection *c = p;
    server_state *s = c->server;
    unsigned slot = c->slot;
    int fd = c->fd;
    free(c);
    qdc_http_conn(s->app, fd);
    pthread_mutex_lock(&s->mu);
    close(s->fds[slot]);
    s->fds[slot] = -1;
    s->active--;
    pthread_cond_broadcast(&s->done);
    pthread_mutex_unlock(&s->mu);
    return NULL;
}
int qdc_http_serve(qdc_app *app, qdc_error *e) {
    if (qdc_listen_address(app->config.listen, app->config.port, e))
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        qdc_error_set(e, "HTTP socket 创建失败");
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = htons((uint16_t)strtoul(app->config.port, NULL, 10))};
    inet_pton(AF_INET, app->config.listen, &addr.sin_addr);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 8)) {
        close(fd);
        qdc_error_set(e, "HTTP 监听失败");
        return -1;
    }
    server_state s = {.app = app};
    pthread_mutex_init(&s.mu, NULL);
    pthread_cond_init(&s.done, NULL);
    for (unsigned i = 0; i < 8; i++)
        s.fds[i] = -1;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    qdc_log("HTTP 服务已就绪");
    while (!atomic_load(&app->stopping)) {
        struct pollfd p = {fd, POLLIN, 0};
        int z = poll(&p, 1, 250);
        if (z < 0 && errno == EINTR)
            continue;
        if (z <= 0)
            continue;
        int client = accept(fd, NULL, NULL);
        if (client < 0)
            continue;
        fcntl(client, F_SETFD, FD_CLOEXEC);
        pthread_mutex_lock(&s.mu);
        unsigned slot = 0;
        while (slot < 8 && s.fds[slot] >= 0)
            slot++;
        if (slot == 8) {
            pthread_mutex_unlock(&s.mu);
            close(client);
            continue;
        }
        connection *c = malloc(sizeof(*c));
        if (!c) {
            pthread_mutex_unlock(&s.mu);
            close(client);
            continue;
        }
        /* Keep a separate control descriptor until the worker has published
         * completion, so shutdown can never target a recycled descriptor. */
        int control = fcntl(client, F_DUPFD_CLOEXEC, 0);
        if (control < 0) {
            free(c);
            pthread_mutex_unlock(&s.mu);
            close(client);
            continue;
        }
        *c = (connection){&s, slot, client};
        s.fds[slot] = control;
        s.active++;
        pthread_t thread;
        if (pthread_create(&thread, &attr, worker, c)) {
            close(control);
            s.fds[slot] = -1;
            s.active--;
            free(c);
            close(client);
        }
        pthread_mutex_unlock(&s.mu);
    }
    close(fd);
    pthread_attr_destroy(&attr);
    pthread_mutex_lock(&s.mu);
    for (unsigned i = 0; i < 8; i++)
        if (s.fds[i] >= 0)
            shutdown(s.fds[i], SHUT_RDWR);
    while (s.active)
        pthread_cond_wait(&s.done, &s.mu);
    pthread_mutex_unlock(&s.mu);
    pthread_cond_destroy(&s.done);
    pthread_mutex_destroy(&s.mu);
    return 0;
}
