#include "qdc_domain.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct line {
    qdc_buf text;
    struct line *next;
} line;
struct qdc_at {
    qdc_app *app;
    int fd, stop[2];
    pthread_t reader;
    bool started, closed;
    pthread_mutex_t serial, mu;
    pthread_cond_t ready;
    line *head, *tail;
    size_t count, queued_bytes;
};
static void drain(qdc_at *);
static bool closed(qdc_at *at) {
    pthread_mutex_lock(&at->mu);
    bool value = at->closed;
    pthread_mutex_unlock(&at->mu);
    return value;
}
static bool prefix(qdc_slice s, const char *p) {
    size_t n = strlen(p);
    return s.len >= n && !memcmp(s.ptr, p, n);
}
static bool urc(qdc_slice s) {
    return prefix(s, "+CMTI:") || prefix(s, "+CMT:") || prefix(s, "+CDS:") || prefix(s, "RING") ||
           prefix(s, "+CLIP:");
}
static void push(qdc_at *at, const unsigned char *p, size_t n) {
    qdc_slice s = {(const char *)p, n};
    if (urc(s)) {
        if (prefix(s, "+CMTI:"))
            qdc_sms_notify(at->app);
        return;
    }
    pthread_mutex_lock(&at->mu);
    if (at->closed) {
        pthread_mutex_unlock(&at->mu);
        return;
    }
    if (at->count >= 4096 || n > (1024u * 1024u) - at->queued_bytes) {
        at->closed = true;
        pthread_cond_broadcast(&at->ready);
        pthread_mutex_unlock(&at->mu);
        drain(at);
        return;
    }
    pthread_mutex_unlock(&at->mu);
    line *l = calloc(1, sizeof *l);
    if (!l || qdc_buf_append(&l->text, p, n)) {
        if (l)
            free(l);
        pthread_mutex_lock(&at->mu);
        at->closed = true;
        pthread_cond_broadcast(&at->ready);
        pthread_mutex_unlock(&at->mu);
        return;
    }
    pthread_mutex_lock(&at->mu);
    if (at->closed || at->count >= 4096 || n > (1024u * 1024u) - at->queued_bytes) {
        at->closed = true;
        qdc_buf_free(&l->text);
        free(l);
    } else {
        if (at->tail)
            at->tail->next = l;
        else
            at->head = l;
        at->tail = l;
        at->count++;
        at->queued_bytes += n;
    }
    pthread_cond_broadcast(&at->ready);
    pthread_mutex_unlock(&at->mu);
}
static void reset(qdc_buf *b) {
    if (b->cap > 8192)
        qdc_buf_free(b);
    else {
        b->len = 0;
        if (b->data)
            b->data[0] = 0;
    }
}
static void *read_worker(void *arg) {
    qdc_at *at = arg;
    qdc_buf pending = {0};
    size_t prompt_start = 0;
    unsigned char bytes[4096];
    while (!atomic_load(&at->app->stopping)) {
        struct pollfd p[2] = {{at->fd, POLLIN, 0}, {at->stop[0], POLLIN, 0}};
        int rc = poll(p, 2, 500);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0 || p[1].revents)
            break;
        if (!rc)
            continue;
        ssize_t n = read(at->fd, bytes, sizeof bytes);
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            unsigned char b = bytes[i];
            if (closed(at) || pending.len > 1024u * 1024u || qdc_buf_append(&pending, &b, 1))
                goto done;
            if (b == '\n') {
                size_t end = pending.len;
                while (end && (pending.data[end - 1] == '\r' || pending.data[end - 1] == '\n'))
                    end--;
                if (end)
                    push(at, pending.data, end);
                reset(&pending);
                prompt_start = 0;
                continue;
            }
            if (prompt_start == pending.len - 1 && b == '\r')
                prompt_start++;
            size_t pl = pending.len - prompt_start;
            if ((pl == 1 && pending.data[prompt_start] == '>') ||
                (pl == 2 && pending.data[prompt_start] == '>' &&
                 pending.data[prompt_start + 1] == ' ')) {
                push(at, (const unsigned char *)">", 1);
                reset(&pending);
                prompt_start = 0;
            }
        }
    }
done:
    qdc_buf_free(&pending);
    pthread_mutex_lock(&at->mu);
    at->closed = true;
    pthread_cond_broadcast(&at->ready);
    pthread_mutex_unlock(&at->mu);
    return NULL;
}
static void drain(qdc_at *at) {
    pthread_mutex_lock(&at->mu);
    line *p = at->head;
    at->head = at->tail = NULL;
    at->count = at->queued_bytes = 0;
    pthread_mutex_unlock(&at->mu);
    while (p) {
        line *n = p->next;
        qdc_buf_free(&p->text);
        free(p);
        p = n;
    }
}
static int write_all(qdc_at *at, const void *ptr, size_t n, uint64_t until) {
    const unsigned char *p = ptr;
    while (n && !atomic_load(&at->app->stopping) && !closed(at)) {
        ssize_t w = write(at->fd, p, n);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            uint64_t now = qdc_mono_ms();
            if (now >= until)
                return -1;
            struct pollfd fd = {at->fd, POLLOUT, 0};
            int rc = poll(&fd, 1, (int)(until - now > 500 ? 500 : until - now));
            if (rc < 0 && errno != EINTR)
                return -1;
            continue;
        }
        return -1;
    }
    return n ? -1 : 0;
}
static line *next(qdc_at *at, uint64_t until) {
    pthread_mutex_lock(&at->mu);
    while (!at->head && !at->closed && !atomic_load(&at->app->stopping)) {
        uint64_t now = qdc_mono_ms();
        if (now >= until)
            break;
        /* Signal handlers only set stopping. Bound the wait slice so a
         * missing modem response cannot delay recovery by a full CMGS minute. */
        uint64_t wake = until - now > 100 ? now + 100 : until;
        qdc_cond_until(&at->ready, &at->mu, wake);
    }
    line *l = at->closed ? NULL : at->head;
    if (l) {
        at->head = l->next;
        if (!at->head)
            at->tail = NULL;
        at->count--;
        at->queued_bytes -= l->text.len;
    }
    pthread_mutex_unlock(&at->mu);
    return l;
}
static int wait_result(qdc_at *at, unsigned timeout, bool submitted, qdc_buf *out, qdc_error *e) {
    uint64_t until = qdc_mono_ms() + timeout;
    bool accepted = false;
    for (;;) {
        line *l = next(at, until);
        if (!l) {
            qdc_error_set(e, submitted ? "等待模块发送结果超时，请核对接收端"
                                       : "AT 响应超时或连接中断");
            e->uncertain = submitted;
            return -1;
        }
        qdc_slice s = {(char *)l->text.data, l->text.len};
        bool ok = qdc_slice_eq(s, "OK"),
             bad = qdc_slice_eq(s, "ERROR") || prefix(s, "+CME ERROR") || prefix(s, "+CMS ERROR");
        if (prefix(s, "+CMGS:"))
            accepted = true;
        int rc = qdc_buf_append(out, s.ptr, s.len) || qdc_buf_append(out, "\n", 1);
        qdc_buf_free(&l->text);
        free(l);
        if (rc) {
            qdc_error_set(e, "内存不足");
            e->uncertain = submitted;
            return -1;
        }
        if (ok) {
            if (submitted && !accepted) {
                qdc_error_set(e, "未收到短信提交编号，请核对接收端");
                e->uncertain = true;
                return -1;
            }
            return 0;
        }
        if (bad) {
            qdc_error_set(e, submitted ? "发送失败：模块返回错误" : "模块返回错误");
            return -1;
        }
    }
}
int qdc_command(qdc_app *app, const char *cmd, unsigned timeout, qdc_buf *out, qdc_error *e) {
    qdc_domain *d = app->domain;
    qdc_at *at = d ? d->at : NULL;
    if (e)
        memset(e, 0, sizeof *e);
    if (!at) {
        qdc_error_set(e, "AT 通道尚未打开");
        return -1;
    }
    pthread_mutex_lock(&at->serial);
    if (closed(at)) {
        pthread_mutex_unlock(&at->serial);
        qdc_error_set(e, "AT 通道已中断，未提交命令");
        return -1;
    }
    drain(at);
    int rc = write_all(at, cmd, strlen(cmd), qdc_mono_ms() + timeout) ||
             write_all(at, "\r", 1, qdc_mono_ms() + timeout);
    if (rc)
        qdc_error_set(e, "AT 写入失败");
    else
        rc = wait_result(at, timeout, false, out, e);
    pthread_mutex_unlock(&at->serial);
    return rc ? -1 : 0;
}
int qdc_at_submit(qdc_app *app, qdc_slice pdu, bool dry, qdc_buf *out, qdc_error *e) {
    qdc_domain *d = app->domain;
    qdc_at *at = d ? d->at : NULL;
    memset(e, 0, sizeof *e);
    if (!at || pdu.len < 2 || pdu.len % 2) {
        qdc_error_set(e, "PDU 或 AT 通道无效");
        return -1;
    }
    pthread_mutex_lock(&at->serial);
    if (closed(at)) {
        pthread_mutex_unlock(&at->serial);
        qdc_error_set(e, "AT 通道已中断，未提交短信");
        return -1;
    }
    drain(at);
    char cmd[64];
    int n = snprintf(cmd, sizeof cmd, "AT+CMGS=%zu\r", pdu.len / 2 - 1);
    int rc = -1;
    if (write_all(at, cmd, (size_t)n, qdc_mono_ms() + 10000)) {
        qdc_error_set(e, "AT 写入失败");
        goto done;
    }
    uint64_t until = qdc_mono_ms() + 10000;
    for (;;) {
        line *l = next(at, until);
        if (!l) {
            qdc_error_set(e, "等待 CMGS 提示符超时");
            goto done;
        }
        qdc_slice s = {(char *)l->text.data, l->text.len};
        bool prompt = memchr(s.ptr, '>', s.len) != NULL,
             bad = qdc_slice_eq(s, "ERROR") || prefix(s, "+CMS ERROR") || prefix(s, "+CME ERROR");
        qdc_buf_free(&l->text);
        free(l);
        if (prompt)
            break;
        if (bad) {
            qdc_error_set(e, "模块拒绝发送");
            goto done;
        }
    }
    if (dry) {
        if (write_all(at, "\x1b", 1, qdc_mono_ms() + 1000)) {
            qdc_error_set(e, "无法取消测试发送");
            goto done;
        }
        qdc_sleep(app, 300);
        drain(at);
        qdc_buf_puts(out, "DRY-RUN\n");
        rc = 0;
        goto done;
    }
    if (write_all(at, pdu.ptr, pdu.len, qdc_mono_ms() + 10000) ||
        write_all(at, "\x1a", 1, qdc_mono_ms() + 1000)) {
        qdc_error_set(e, "短信提交写入未确认");
        e->uncertain = true;
        goto done;
    }
    rc = wait_result(at, 60000, true, out, e);
done:
    pthread_mutex_unlock(&at->serial);
    return rc;
}
int qdc_at_open(qdc_app *app, qdc_error *e) {
    qdc_domain *d = app->domain;
    qdc_at *at = calloc(1, sizeof *at);
    if (!at) {
        qdc_error_set(e, "内存不足");
        return -1;
    }
    at->app = app;
    at->fd = -1;
    at->stop[0] = at->stop[1] = -1;
    pthread_mutex_init(&at->serial, NULL);
    pthread_mutex_init(&at->mu, NULL);
    if (qdc_cond_init(&at->ready)) {
        pthread_mutex_destroy(&at->mu);
        pthread_mutex_destroy(&at->serial);
        free(at);
        qdc_error_set(e, "无法初始化单调 AT 等待器");
        return -1;
    }
    at->fd = open(app->config.at_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (at->fd < 0 || pipe(at->stop)) {
        qdc_error_set(e, "无法打开 AT 通道");
        goto fail;
    }
    fcntl(at->stop[0], F_SETFD, FD_CLOEXEC);
    fcntl(at->stop[1], F_SETFD, FD_CLOEXEC);
    d->at = at;
    if (qdc_domain_thread(&at->reader, read_worker, at)) {
        qdc_error_set(e, "无法启动 AT 读取线程");
        goto fail;
    }
    at->started = true;
    qdc_buf b = {0};
    int rc = qdc_command(app, "AT", 5000, &b, e);
    qdc_buf_free(&b);
    if (!rc)
        rc = qdc_command(app, "ATE0", 5000, &b, e);
    qdc_buf_free(&b);
    if (rc)
        goto fail;
    return 0;
fail:
    d->at = at;
    qdc_at_close(app);
    return -1;
}
void qdc_at_close(qdc_app *app) {
    qdc_domain *d = app->domain;
    qdc_at *at = d ? d->at : NULL;
    if (!at)
        return;
    pthread_mutex_lock(&at->mu);
    at->closed = true;
    pthread_cond_broadcast(&at->ready);
    pthread_mutex_unlock(&at->mu);
    if (at->stop[1] >= 0) {
        unsigned char b = 1;
        (void)write(at->stop[1], &b, 1);
    }
    if (at->started)
        pthread_join(at->reader, NULL);
    if (at->fd >= 0)
        close(at->fd);
    for (int i = 0; i < 2; i++)
        if (at->stop[i] >= 0)
            close(at->stop[i]);
    drain(at);
    pthread_cond_destroy(&at->ready);
    pthread_mutex_destroy(&at->mu);
    pthread_mutex_destroy(&at->serial);
    free(at);
    d->at = NULL;
}
