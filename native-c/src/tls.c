#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif
#include "qdc_push.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <nghttp2/nghttp2.h>
#include <poll.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DNS_MAX 4096
#define HTTP_HEADERS_MAX 16384
#define APNS_BODY_MAX 8192
static const char *const dns_address = "192.168.225.1";

static bool apple_host(const char *host) {
    return host && (!strcmp(host, QDC_APNS_HOST) || !strcmp(host, QDC_CLOCK_HOST));
}
static bool clean_value(const char *s) {
    if (!s)
        return false;
    for (; *s; s++)
        if ((unsigned char)*s < 32 || (unsigned char)*s == 127)
            return false;
    return true;
}
static int wait_fd(int fd, short events, uint64_t deadline) {
    for (;;) {
        uint64_t now = qdc_mono_ms();
        if (now >= deadline)
            return -1;
        uint64_t remaining = deadline - now;
        int ms = remaining > INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd p = {.fd = fd, .events = events};
        int rc = poll(&p, 1, ms);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0)
            return -1;
        if (p.revents & events)
            return 0;
        return -1;
    }
}
static int new_socket(int type) {
    int fd = socket(AF_INET, type, 0);
    if (fd < 0)
        return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
#ifdef SO_NOSIGPIPE
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0) {
        close(fd);
        return -1;
    }
#endif
    return fd;
}
static ssize_t socket_send(int fd, const void *p, size_t n) {
#ifdef MSG_NOSIGNAL
    return send(fd, p, n, MSG_NOSIGNAL);
#else
    return send(fd, p, n, 0);
#endif
}
static int connect_ip(const char *address, unsigned port, int type, uint64_t deadline) {
    struct sockaddr_in target = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (!port || port > 65535 || inet_pton(AF_INET, address, &target.sin_addr) != 1)
        return -1;
    int fd = new_socket(type);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&target, sizeof(target)) != 0) {
        int error = 0;
        socklen_t n = sizeof(error);
        if (errno != EINPROGRESS || wait_fd(fd, POLLOUT, deadline) != 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &n) != 0 || error != 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}
static int socket_all(int fd, unsigned char *p, size_t len, bool writing, uint64_t deadline) {
    size_t done = 0;
    while (done < len) {
        if (qdc_mono_ms() >= deadline)
            return -1;
        ssize_t n =
            writing ? socket_send(fd, p + done, len - done) : recv(fd, p + done, len - done, 0);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(fd, writing ? POLLOUT : POLLIN, deadline) == 0)
                continue;
        }
        return -1;
    }
    return 0;
}

#ifdef QDC_TESTING
static pthread_mutex_t hook_mu = PTHREAD_MUTEX_INITIALIZER;
static char test_address[16], test_name[254], test_dns[16];
static unsigned test_port, test_dns_port;
static bool loopback(const char *s) {
    struct in_addr a;
    return s && inet_pton(AF_INET, s, &a) == 1 && (ntohl(a.s_addr) >> 24) == 127;
}
int qdc_tls_test_endpoint(const char *ip, unsigned port, const char *name) {
    if (!loopback(ip) || !port || port > 65535 || !name || strlen(name) > 253 || !clean_value(name))
        return -1;
    pthread_mutex_lock(&hook_mu);
    snprintf(test_address, sizeof(test_address), "%s", ip);
    snprintf(test_name, sizeof(test_name), "%s", name);
    test_port = port;
    pthread_mutex_unlock(&hook_mu);
    return 0;
}
int qdc_dns_test_server(const char *ip, unsigned port) {
    if (!loopback(ip) || !port || port > 65535)
        return -1;
    pthread_mutex_lock(&hook_mu);
    snprintf(test_dns, sizeof(test_dns), "%s", ip);
    test_dns_port = port;
    pthread_mutex_unlock(&hook_mu);
    return 0;
}
#endif

static uint16_t u16(const unsigned char *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static void put16(unsigned char *p, uint16_t n) {
    p[0] = (unsigned char)(n >> 8);
    p[1] = (unsigned char)n;
}
static int dns_name(const unsigned char *packet, size_t len, size_t *position, char out[254]) {
    size_t pos = *position, written = 0, after = 0;
    bool jumped = false;
    for (unsigned hop = 0; hop < 128; hop++) {
        if (pos >= len)
            return -1;
        unsigned size = packet[pos++];
        if ((size & 0xc0) == 0xc0) {
            if (pos >= len)
                return -1;
            size_t pointer = ((size & 63U) << 8) | packet[pos++];
            if (pointer >= pos - 2 || pointer >= len)
                return -1;
            if (!jumped)
                after = pos;
            jumped = true;
            pos = pointer;
            continue;
        }
        if (size & 0xc0)
            return -1;
        if (!size) {
            out[written] = 0;
            *position = jumped ? after : pos;
            return 0;
        }
        if (size > 63 || size > len - pos || written + size + (written != 0) > 253)
            return -1;
        if (written)
            out[written++] = '.';
        for (unsigned i = 0; i < size; i++) {
            unsigned char c = packet[pos++];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_'))
                return -1;
            out[written++] = (char)tolower(c);
        }
    }
    return -1;
}
int qdc_dns_answer(const unsigned char *packet, size_t len, uint16_t id, const char *host,
                   char address[16], char cname[254], bool *truncated, qdc_error *error) {
    char question[254], current[254];
    size_t begin = 12;
    if (address)
        address[0] = 0;
    if (cname)
        cname[0] = 0;
    if (truncated)
        *truncated = false;
    if (!packet || !host || !address || !cname || !truncated || len < 12 || len > DNS_MAX ||
        u16(packet) != id || !(packet[2] & 0x80) || (packet[2] & 0x78) || (packet[3] & 15) ||
        u16(packet + 4) != 1 || strlen(host) > 253)
        goto bad;
    if (dns_name(packet, len, &begin, question) || strcmp(question, host) || len - begin < 4 ||
        u16(packet + begin) != 1 || u16(packet + begin + 2) != 1)
        goto bad;
    begin += 4;
    if (packet[2] & 2) {
        *truncated = true;
        return 1;
    }
    unsigned records = (unsigned)u16(packet + 6) + u16(packet + 8) + u16(packet + 10);
    if (records > 64)
        goto bad;
    snprintf(current, sizeof(current), "%s", host);
    for (unsigned chain = 0; chain < 9; chain++) {
        size_t pos = begin;
        char alias[254] = "";
        bool found = false;
        for (unsigned i = 0; i < records; i++) {
            char owner[254], target[254];
            if (dns_name(packet, len, &pos, owner) || len - pos < 10)
                goto bad;
            uint16_t type = u16(packet + pos), klass = u16(packet + pos + 2),
                     n = u16(packet + pos + 8);
            pos += 10;
            if (n > len - pos)
                goto bad;
            if (klass == 1 && !strcmp(owner, current)) {
                if (type == 1) {
                    if (n != 4 || !inet_ntop(AF_INET, packet + pos, address, 16))
                        goto bad;
                    found = true;
                } else if (type == 5) {
                    size_t p = pos;
                    if (dns_name(packet, len, &p, target) || p != pos + n || !target[0])
                        goto bad;
                    if (alias[0] && strcmp(alias, target))
                        goto bad;
                    snprintf(alias, sizeof(alias), "%s", target);
                }
            }
            pos += n;
        }
        if (pos != len)
            goto bad;
        if (found) {
            if (alias[0])
                goto bad;
            return 0;
        }
        if (!alias[0]) {
            if (strcmp(current, host)) {
                snprintf(cname, 254, "%s", current);
                return 1;
            }
            goto bad;
        }
        if (!strcmp(alias, current))
            goto bad;
        snprintf(current, sizeof(current), "%s", alias);
    }
bad:
    qdc_error_set(error, "推送 DNS 响应无效");
    return -1;
}
static int dns_query(const char *host, unsigned char packet[512], size_t *len, uint16_t *id,
                     qdc_error *error) {
    size_t pos = 12;
    unsigned char random[2];
    if (!host[0] || strlen(host) > 253 || qdc_random(random, sizeof(random), error))
        return -1;
    *id = u16(random);
    memset(packet, 0, 512);
    put16(packet, *id);
    packet[2] = 1;
    packet[5] = 1;
    const char *part = host;
    for (;;) {
        const char *dot = strchr(part, '.');
        size_t n = dot ? (size_t)(dot - part) : strlen(part);
        if (!n || n > 63 || pos + n + 6 > 512)
            return -1;
        packet[pos++] = (unsigned char)n;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)part[i];
            if (!(isalnum(c) || c == '-'))
                return -1;
            packet[pos++] = (unsigned char)tolower(c);
        }
        if (!dot)
            break;
        part = dot + 1;
    }
    packet[pos++] = 0;
    put16(packet + pos, 1);
    put16(packet + pos + 2, 1);
    pos += 4;
    *len = pos;
    return 0;
}
int qdc_dns_resolve(const char *host, char address[16], uint64_t deadline, qdc_error *error) {
    char server[16], current[254], visited[9][254];
    unsigned port = 53;
    unsigned char query[512], reply[DNS_MAX];
    if (!apple_host(host)) {
        qdc_error_set(error, "推送主机不允许");
        return -1;
    }
    snprintf(server, sizeof(server), "%s", dns_address);
#ifdef QDC_TESTING
    pthread_mutex_lock(&hook_mu);
    if (test_dns_port) {
        snprintf(server, sizeof(server), "%s", test_dns);
        port = test_dns_port;
    }
    pthread_mutex_unlock(&hook_mu);
#endif
    snprintf(current, sizeof(current), "%s", host);
    for (unsigned chain = 0; chain < 8; chain++) {
        for (unsigned i = 0; i < chain; i++)
            if (!strcmp(current, visited[i]))
                goto fail;
        snprintf(visited[chain], sizeof(visited[chain]), "%s", current);
        uint16_t id;
        size_t query_len;
        bool truncated;
        char alias[254];
        if (dns_query(current, query, &query_len, &id, error))
            goto fail;
        int fd = connect_ip(server, port, SOCK_DGRAM, deadline);
        if (fd < 0)
            goto fail;
        if (socket_all(fd, query, query_len, true, deadline)) {
            close(fd);
            goto fail;
        }
        ssize_t n = -1;
        for (;;) {
            if (wait_fd(fd, POLLIN, deadline))
                break;
            n = recv(fd, reply, sizeof(reply), 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (n >= 2 && u16(reply) != id)
                continue;
            break;
        }
        close(fd);
        if (n <= 0 || n == DNS_MAX)
            goto fail;
        int rc = qdc_dns_answer(reply, (size_t)n, id, current, address, alias, &truncated, error);
        if (truncated) {
            fd = connect_ip(server, port, SOCK_STREAM, deadline);
            if (fd < 0)
                goto fail;
            unsigned char length[2];
            put16(length, (uint16_t)query_len);
            if (socket_all(fd, length, 2, true, deadline) ||
                socket_all(fd, query, query_len, true, deadline) ||
                socket_all(fd, length, 2, false, deadline)) {
                close(fd);
                goto fail;
            }
            size_t reply_len = u16(length);
            if (reply_len < 12 || reply_len > sizeof(reply) ||
                socket_all(fd, reply, reply_len, false, deadline)) {
                close(fd);
                goto fail;
            }
            close(fd);
            rc = qdc_dns_answer(reply, reply_len, id, current, address, alias, &truncated, error);
            if (truncated)
                goto fail;
        }
        if (rc == 0)
            return 0;
        if (rc < 0 || !alias[0])
            goto fail;
        snprintf(current, sizeof(current), "%s", alias);
    }
fail:
    qdc_error_set(error, "推送 DNS 查询失败或超时");
    return -1;
}

static int digits(const char *s, unsigned n) {
    int v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + s[i] - '0';
    }
    return v;
}
int qdc_http_date(qdc_slice value, int64_t *out) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const char weekdays[] = "SunMonTueWedThuFriSat";
    if (!value.ptr || value.len != 29 || !out)
        return -1;
    const char *s = value.ptr;
    int month = 0;
    bool weekday = false;
    for (int i = 0; i < 12; i++)
        if (!memcmp(s + 8, months + i * 3, 3))
            month = i + 1;
    for (int i = 0; i < 7; i++)
        if (!memcmp(s, weekdays + i * 3, 3))
            weekday = true;
    if (!weekday || !month || memcmp(s + 3, ", ", 2) || s[7] != ' ' || s[11] != ' ' ||
        s[16] != ' ' || s[19] != ':' || s[22] != ':' || memcmp(s + 25, " GMT", 4))
        return -1;
    int day = digits(s + 5, 2), year = digits(s + 12, 4), hour = digits(s + 17, 2);
    int minute = digits(s + 20, 2), second = digits(s + 23, 2);
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (year < 1601 || day < 1 || day > days[month - 1] + (month == 2 && leap) || hour < 0 ||
        hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
        return -1;
    int y = year - (month <= 2), era = y / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153U * (unsigned)(month + (month > 2 ? -3 : 9)) + 2) / 5 + (unsigned)day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t date = (int64_t)era * 146097 + doe - 719468;
    *out = ((date * 24 + hour) * 60 * 60 + minute * 60 + second) * 1000;
    return 0;
}

static _Thread_local qdc_app *clock_app;
static pthread_once_t tls_once = PTHREAD_ONCE_INIT;
static int tls_ready = -1;
static mbedtls_time_t app_time(mbedtls_time_t *value) {
    mbedtls_time_t now =
        (mbedtls_time_t)(clock_app ? qdc_now_ms(clock_app) / 1000 : qdc_system_ms() / 1000);
    if (value)
        *value = now;
    return now;
}
static void tls_initialize(void) {
    if (mbedtls_platform_set_time(app_time) == 0 && psa_crypto_init() == PSA_SUCCESS)
        tls_ready = 0;
}
static int rng(void *context, unsigned char *p, size_t n) {
    (void)context;
    return qdc_random(p, n, NULL);
}
struct tls_io {
    int fd;
    uint64_t deadline;
};
static int bio_send(void *context, const unsigned char *p, size_t n) {
    struct tls_io *io = context;
    if (qdc_mono_ms() >= io->deadline)
        return MBEDTLS_ERR_SSL_TIMEOUT;
    ssize_t rc = socket_send(io->fd, p, n);
    if (rc >= 0)
        return (int)rc;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}
static int bio_recv(void *context, unsigned char *p, size_t n) {
    struct tls_io *io = context;
    if (qdc_mono_ms() >= io->deadline)
        return MBEDTLS_ERR_SSL_TIMEOUT;
    ssize_t rc = recv(io->fd, p, n, 0);
    if (rc >= 0)
        return (int)rc;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}
static bool ssl_again(struct tls_io *io, int rc) {
    short event;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ)
        event = POLLIN;
    else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE)
        event = POLLOUT;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
    else if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        return qdc_mono_ms() < io->deadline;
#endif
    else
        return false;
    return wait_fd(io->fd, event, io->deadline) == 0;
}
static int ssl_write_all(mbedtls_ssl_context *ssl, struct tls_io *io, const void *bytes,
                         size_t len) {
    const unsigned char *p = bytes;
    size_t done = 0;
    while (done < len) {
        int rc = mbedtls_ssl_write(ssl, p + done, len - done);
        if (rc > 0) {
            done += (size_t)rc;
            continue;
        }
        if (!ssl_again(io, rc))
            return -1;
    }
    return 0;
}
static int ssl_read_some(mbedtls_ssl_context *ssl, struct tls_io *io, unsigned char *p,
                         size_t len) {
    for (;;) {
        int rc = mbedtls_ssl_read(ssl, p, len);
        if (rc > 0)
            return rc;
        if (!ssl_again(io, rc))
            return -1;
    }
}
static int load_roots(mbedtls_x509_crt *roots, const char *requested, qdc_error *error) {
    const char *paths[] = {requested, "/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        qdc_buf pem = {0};
        if (!paths[i] || !paths[i][0])
            continue;
        int rc = qdc_read_file(paths[i], 2 * 1024 * 1024, &pem, error);
        if (rc == 1)
            continue;
        if (rc || !pem.len) {
            qdc_buf_free(&pem);
            return -1;
        }
        rc = mbedtls_x509_crt_parse(roots, pem.data, pem.len + 1);
        qdc_buf_free(&pem);
        if (rc != 0) {
            qdc_error_set(error, "推送信任证书无效");
            return -1;
        }
        return 0;
    }
    qdc_error_set(error, "推送信任证书不可用");
    return -1;
}

struct h2 {
    const qdc_tls_request *request;
    qdc_tls_response *response;
    size_t body_pos, header_bytes, header_count;
    int status, stream;
    bool closed, failed, final_headers;
    char date[128];
};
static nghttp2_ssize h2_body(nghttp2_session *session, int32_t stream, uint8_t *out, size_t len,
                             uint32_t *flags, nghttp2_data_source *source, void *context) {
    struct h2 *h = context;
    (void)session;
    (void)stream;
    (void)source;
    size_t n = h->request->body.len - h->body_pos;
    if (n > len)
        n = len;
    if (n)
        memcpy(out, h->request->body.ptr + h->body_pos, n);
    h->body_pos += n;
    if (h->body_pos == h->request->body.len)
        *flags |= NGHTTP2_DATA_FLAG_EOF;
    return (nghttp2_ssize)n;
}
static int h2_begin(nghttp2_session *session, const nghttp2_frame *frame, void *context) {
    struct h2 *h = context;
    (void)session;
    (void)frame;
    h->status = 0;
    h->date[0] = 0;
    return 0;
}
static int h2_header(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name,
                     size_t nlen, const uint8_t *value, size_t vlen, uint8_t flags, void *context) {
    struct h2 *h = context;
    (void)session;
    (void)frame;
    (void)flags;
    if (nlen > HTTP_HEADERS_MAX || vlen > HTTP_HEADERS_MAX ||
        h->header_bytes > HTTP_HEADERS_MAX - nlen ||
        h->header_bytes + nlen > HTTP_HEADERS_MAX - vlen || ++h->header_count > 100)
        goto fail;
    h->header_bytes += nlen + vlen;
    if (nlen == 7 && !memcmp(name, ":status", 7)) {
        if (vlen != 3 || h->status)
            goto fail;
        h->status = digits((const char *)value, 3);
        if (h->status < 100 || h->status > 599)
            goto fail;
    } else if (nlen == 4 && !memcmp(name, "date", 4)) {
        if (vlen >= sizeof(h->date) || memchr(value, 0, vlen) || h->date[0])
            goto fail;
        memcpy(h->date, value, vlen);
        h->date[vlen] = 0;
    }
    return 0;
fail:
    h->failed = true;
    return NGHTTP2_ERR_CALLBACK_FAILURE;
}
static int h2_data(nghttp2_session *session, uint8_t flags, int32_t stream, const uint8_t *data,
                   size_t len, void *context) {
    struct h2 *h = context;
    (void)session;
    (void)flags;
    if (stream != h->stream)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    size_t limit = h->request->require_h2 ? APNS_BODY_MAX : 1024;
    if (len > limit - h->response->body.len || qdc_buf_append(&h->response->body, data, len)) {
        h->failed = true;
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
static int h2_frame(nghttp2_session *session, const nghttp2_frame *frame, void *context) {
    struct h2 *h = context;
    (void)session;
    if (frame->hd.type == NGHTTP2_HEADERS && frame->hd.stream_id == h->stream && h->status >= 200) {
        if (h->final_headers) {
            h->failed = true;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        h->final_headers = true;
        h->response->status = h->status;
        snprintf(h->response->date, sizeof(h->response->date), "%s", h->date);
    } else if (frame->hd.type == NGHTTP2_GOAWAY && (frame->goaway.error_code != NGHTTP2_NO_ERROR ||
                                                    frame->goaway.last_stream_id < h->stream)) {
        h->failed = true;
    }
    return 0;
}
static int h2_close(nghttp2_session *session, int32_t stream, uint32_t code, void *context) {
    struct h2 *h = context;
    (void)session;
    if (stream == h->stream) {
        h->closed = true;
        if (code != NGHTTP2_NO_ERROR)
            h->failed = true;
    }
    return 0;
}
static nghttp2_nv make_nv(const char *name, const char *value, uint8_t flags) {
    nghttp2_nv h = {(uint8_t *)name, (uint8_t *)value, strlen(name), strlen(value), flags};
    return h;
}
static int fetch_h2(mbedtls_ssl_context *ssl, struct tls_io *io, const qdc_tls_request *request,
                    qdc_tls_response *response) {
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_option *option = NULL;
    nghttp2_session *session = NULL;
    struct h2 h = {.request = request, .response = response};
    int rc = -1;
    nghttp2_nv headers[36];
    if (request->header_count > 32 || nghttp2_session_callbacks_new(&callbacks) ||
        nghttp2_option_new(&option))
        goto done;
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, h2_begin);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, h2_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, h2_data);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, h2_frame);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, h2_close);
    nghttp2_option_set_max_outbound_queue_size(option, 64);
    nghttp2_option_set_max_outbound_ack(option, 32);
    nghttp2_option_set_max_continuations(option, 8);
    nghttp2_option_set_max_reserved_remote_streams(option, 0);
    if (nghttp2_session_client_new2(&session, callbacks, &h, option))
        goto done;
    nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
                                         {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 1},
                                         {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, HTTP_HEADERS_MAX}};
    if (nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings, 3))
        goto done;
    headers[0] = make_nv(":method", request->method, 0);
    headers[1] = make_nv(":scheme", "https", 0);
    headers[2] = make_nv(":authority", request->host, 0);
    headers[3] = make_nv(":path", request->path, 0);
    for (size_t i = 0; i < request->header_count; i++)
        headers[4 + i] = make_nv(
            request->headers[i].name, request->headers[i].value,
            !strcmp(request->headers[i].name, "authorization") ? NGHTTP2_NV_FLAG_NO_INDEX : 0);
    nghttp2_data_provider2 provider = {.read_callback = h2_body};
    h.stream = nghttp2_submit_request2(session, NULL, headers, 4 + request->header_count,
                                       request->body.len ? &provider : NULL, NULL);
    if (h.stream < 0)
        goto done;
    while (!h.closed && !h.failed) {
        const uint8_t *data;
        nghttp2_ssize n;
        while ((n = nghttp2_session_mem_send2(session, &data)) > 0)
            if (ssl_write_all(ssl, io, data, (size_t)n))
                goto done;
        if (n < 0)
            goto done;
        unsigned char input[4096];
        int size = ssl_read_some(ssl, io, input, sizeof(input));
        if (size < 0 || nghttp2_session_mem_recv2(session, input, (size_t)size) != size)
            goto done;
    }
    if (h.closed && h.final_headers && !h.failed) {
        response->http2 = true;
        rc = 0;
    }
done:
    if (session)
        nghttp2_session_del(session);
    if (option)
        nghttp2_option_del(option);
    if (callbacks)
        nghttp2_session_callbacks_del(callbacks);
    return rc;
}

static bool header_token(unsigned char c) {
    return isalnum(c) || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}
static int parse_h1(char *header, size_t len, qdc_tls_response *response) {
    if (len < 12 || memchr(header, 0, len))
        return -1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)header[i];
        if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127)
            return -1;
    }
    char *line = strstr(header, "\r\n");
    if (!line || line - header < 12 ||
        (memcmp(header, "HTTP/1.1 ", 9) && memcmp(header, "HTTP/1.0 ", 9)))
        return -1;
    response->status = digits(header + 9, 3);
    if (response->status < 100 || response->status > 599 ||
        (header[12] != ' ' && header + 12 != line))
        return -1;
    for (char *c = header + 12; c < line; c++)
        if ((unsigned char)*c < 32 && *c != '\t')
            return -1;
    char *p = line + 2;
    unsigned fields = 0;
    bool content_length = false, transfer = false;
    while (p[0] != '\r' || p[1] != '\n') {
        char *end = strstr(p, "\r\n"), *colon = end ? memchr(p, ':', (size_t)(end - p)) : NULL;
        if (!end || !colon || colon == p || ++fields > 100)
            return -1;
        for (char *c = p; c < colon; c++) {
            if (!header_token((unsigned char)*c))
                return -1;
            *c = (char)tolower((unsigned char)*c);
        }
        *colon = 0;
        char *v = colon + 1;
        while (v < end && (*v == ' ' || *v == '\t'))
            v++;
        while (end > v && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        size_t n = (size_t)(end - v);
        if (memchr(v, '\r', n) || memchr(v, '\n', n))
            return -1;
        if (!strcmp(p, "date")) {
            if (response->date[0] || n >= sizeof(response->date))
                return -1;
            memcpy(response->date, v, n);
            response->date[n] = 0;
        } else if (!strcmp(p, "content-length")) {
            if (content_length || !n || n > 19)
                return -1;
            uint64_t count = 0;
            for (size_t i = 0; i < n; i++) {
                if (v[i] < '0' || v[i] > '9' || count > (UINT64_MAX - 9) / 10)
                    return -1;
                count = count * 10 + (unsigned)(v[i] - '0');
            }
            content_length = true;
        } else if (!strcmp(p, "transfer-encoding")) {
            if (transfer || n != 7 || memcmp(v, "chunked", 7))
                return -1;
            transfer = true;
        }
        p = strstr(colon + 1, "\r\n");
        if (!p)
            return -1;
        p += 2;
    }
    return content_length && transfer ? -1 : 0;
}
static int fetch_h1(mbedtls_ssl_context *ssl, struct tls_io *io, const qdc_tls_request *request,
                    qdc_tls_response *response) {
    qdc_buf outgoing = {0}, incoming = {0};
    int rc = -1;
    if (qdc_buf_printf(
            &outgoing,
            "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n",
            request->method, request->path, request->host) ||
        ssl_write_all(ssl, io, outgoing.data, outgoing.len))
        goto done;
    for (;;) {
        unsigned char bytes[1024];
        int n = ssl_read_some(ssl, io, bytes, sizeof(bytes));
        if (n < 0 || incoming.len + (size_t)n > HTTP_HEADERS_MAX + 1024 ||
            qdc_buf_append(&incoming, bytes, (size_t)n))
            goto done;
        char *end = strstr((char *)incoming.data, "\r\n\r\n");
        if (end) {
            size_t len = (size_t)(end - (char *)incoming.data) + 4;
            if (len > HTTP_HEADERS_MAX || parse_h1((char *)incoming.data, len, response))
                goto done;
            rc = 0;
            break; /* Clock only: authenticated headers suffice; body is never trusted. */
        }
        if (incoming.len > HTTP_HEADERS_MAX)
            goto done;
    }
done:
    qdc_buf_free(&outgoing);
    qdc_buf_free(&incoming);
    return rc;
}
void qdc_tls_response_free(qdc_tls_response *response) {
    if (response) {
        qdc_buf_free(&response->body);
        memset(response, 0, sizeof(*response));
    }
}
int qdc_tls_fetch(const qdc_tls_request *request, qdc_tls_response *response, qdc_error *error) {
    if (!request || !response || !apple_host(request->host) || !request->method || !request->path ||
        request->path[0] != '/' || !clean_value(request->path) || strlen(request->path) > 1024 ||
        request->header_count > 32 || request->body.len > 32768 ||
        (!request->body.ptr && request->body.len) ||
        (strcmp(request->method, "GET") && strcmp(request->method, "POST")))
        goto invalid;
    if ((!strcmp(request->host, QDC_APNS_HOST) &&
         (!request->require_h2 || strcmp(request->method, "POST"))) ||
        (!strcmp(request->host, QDC_CLOCK_HOST) &&
         (request->require_h2 || strcmp(request->method, "GET") ||
          strcmp(request->path, QDC_CLOCK_PATH))))
        goto invalid;
    for (size_t i = 0; i < request->header_count; i++) {
        const qdc_tls_header *h = &request->headers[i];
        if (!h->name || !h->name[0] || strlen(h->name) > 128 || !clean_value(h->value) ||
            strlen(h->value) > 2048)
            goto invalid;
        for (const char *p = h->name; *p; p++)
            if (!header_token((unsigned char)*p) || isupper((unsigned char)*p))
                goto invalid;
    }
    memset(response, 0, sizeof(*response));
    pthread_once(&tls_once, tls_initialize);
    if (tls_ready) {
        qdc_error_set(error, "安全连接初始化失败");
        return -1;
    }
    char address[16], name[254];
    unsigned port = 443;
    bool overridden = false;
    snprintf(name, sizeof(name), "%s", request->host);
#ifdef QDC_TESTING
    pthread_mutex_lock(&hook_mu);
    if (test_port) {
        snprintf(address, sizeof(address), "%s", test_address);
        snprintf(name, sizeof(name), "%s", test_name);
        port = test_port;
        overridden = true;
    }
    pthread_mutex_unlock(&hook_mu);
#endif
    unsigned timeout = request->timeout_ms ? request->timeout_ms : 12000;
    if (timeout > 12000)
        timeout = 12000;
    struct tls_io io = {.fd = -1, .deadline = qdc_mono_ms() + timeout};
    if (!overridden) {
        uint64_t dns_deadline = qdc_mono_ms() + 3000;
        if (dns_deadline > io.deadline)
            dns_deadline = io.deadline;
        if (qdc_dns_resolve(request->host, address, dns_deadline, error))
            return -1;
    }
    io.fd = connect_ip(address, port, SOCK_STREAM, io.deadline);
    if (io.fd < 0) {
        qdc_error_set(error, "苹果推送连接失败或超时");
        return -1;
    }
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt roots;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&roots);
    qdc_app *previous_app = clock_app;
    clock_app = request->app;
    int rc = -1;
    const char *h2_only[] = {"h2", NULL}, *clock_protocols[] = {"h2", "http/1.1", NULL};
    if (load_roots(&roots, request->ca_file, error) ||
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT))
        goto done;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &roots, NULL);
    mbedtls_ssl_conf_rng(&conf, rng, NULL);
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_3);
    if (mbedtls_ssl_conf_alpn_protocols(&conf, request->require_h2 ? h2_only : clock_protocols) ||
        mbedtls_ssl_setup(&ssl, &conf) || mbedtls_ssl_set_hostname(&ssl, name))
        goto done;
    mbedtls_ssl_set_bio(&ssl, &io, bio_send, bio_recv, NULL);
    int handshake;
    while ((handshake = mbedtls_ssl_handshake(&ssl)) != 0)
        if (!ssl_again(&io, handshake)) {
            qdc_error_set(error, "苹果推送 TLS 验证失败或超时");
            goto done;
        }
    if (mbedtls_ssl_get_verify_result(&ssl)) {
        qdc_error_set(error, "苹果推送证书验证失败");
        goto done;
    }
    const char *protocol = mbedtls_ssl_get_alpn_protocol(&ssl);
    if (protocol && !strcmp(protocol, "h2"))
        rc = fetch_h2(&ssl, &io, request, response);
    else if (!request->require_h2 && (!protocol || !strcmp(protocol, "http/1.1")))
        rc = fetch_h1(&ssl, &io, request, response);
    else
        qdc_error_set(error, "苹果推送未建立 HTTP/2 连接");
    if (rc && (!error || !error->message[0]))
        qdc_error_set(error, "苹果推送结果未确认或超时");
done:
    if (rc && (!error || !error->message[0]))
        qdc_error_set(error, "苹果推送安全连接失败");
    /* Closing the socket is bounded even if the peer stops reading. */
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&roots);
    close(io.fd);
    clock_app = previous_app;
    if (rc)
        qdc_tls_response_free(response);
    return rc;
invalid:
    qdc_error_set(error, "推送请求格式或固定主机无效");
    return -1;
}
