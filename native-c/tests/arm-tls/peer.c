/* Single-connection TLS/H2 peer for the isolated ARM VM, never production. */
#include "lab.h"
#include "tls_record.h"
#include "qdc_push.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <nghttp2/nghttp2.h>
#include <poll.h>
#include <psa/crypto.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

struct io { int fd; long deadline; struct lab_tls_records records; };
struct h2 {
    int32_t stream;
    unsigned fields, mask;
    size_t request_bytes, response_bytes;
    char request[2], date[40];
    int complete, sent_end, closed, failed;
};
static const char body[] = "{\"arm\":true}";

static int wait_io(struct io *io, short events) {
    for (;;) {
        long remaining = io->deadline - lab_now_ms();
        if (remaining <= 0) return -1;
        struct pollfd p = {io->fd, events, 0};
        int rc = poll(&p, 1, (int)remaining);
        if (rc < 0 && errno == EINTR) continue;
        return rc > 0 && (p.revents & events) ? 0 : -1;
    }
}
static int rng(void *unused, unsigned char *p, size_t n) {
    (void)unused;
    return qdc_random(p, n, NULL);
}
static int send_bytes(void *context, const unsigned char *p, size_t n) {
    struct io *io = context;
    if (lab_now_ms() >= io->deadline) return MBEDTLS_ERR_SSL_TIMEOUT;
    ssize_t rc = send(io->fd, p, n, MSG_NOSIGNAL);
    if (rc >= 0) return (int)rc;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}
static int recv_bytes(void *context, unsigned char *p, size_t n) {
    struct io *io = context;
    if (lab_now_ms() >= io->deadline) return MBEDTLS_ERR_SSL_TIMEOUT;
    ssize_t rc = recv(io->fd, p, n, 0);
    if (rc > 0) lab_tls_records_feed(&io->records, p, (size_t)rc);
    if (rc >= 0) return (int)rc;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}
static int again(struct io *io, int rc) {
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) return !wait_io(io, POLLIN);
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) return !wait_io(io, POLLOUT);
    return 0;
}
static int receive_certificate_alert(struct io *io) {
    while (lab_now_ms() < io->deadline && !io->records.malformed) {
        if (io->records.certificate_alert) return 0;
        unsigned char bytes[1024];
        int rc = recv_bytes(io, bytes, sizeof(bytes));
        if (rc > 0) continue;
        if (rc == 0 || !again(io, rc)) return -1;
    }
    return -1;
}
static int write_all(mbedtls_ssl_context *ssl, struct io *io, const uint8_t *p, size_t n) {
    size_t done = 0;
    while (done < n) {
        int rc = mbedtls_ssl_write(ssl, p + done, n - done);
        if (rc > 0) done += (size_t)rc;
        else if (!again(io, rc)) return -1;
    }
    return 0;
}
static int read_some(mbedtls_ssl_context *ssl, struct io *io, uint8_t *p, size_t n) {
    for (;;) {
        int rc = mbedtls_ssl_read(ssl, p, n);
        if (rc > 0) return rc;
        if (!again(io, rc)) return -1;
    }
}
static nghttp2_nv nv(const char *name, const char *value) {
    nghttp2_nv h = {(uint8_t *)name, (uint8_t *)value, strlen(name), strlen(value), 0};
    return h;
}
static int begin_headers(nghttp2_session *s, const nghttp2_frame *f, void *ctx) {
    (void)s;
    struct h2 *h = ctx;
    if (f->hd.type != NGHTTP2_HEADERS || f->headers.cat != NGHTTP2_HCAT_REQUEST || h->stream)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    h->stream = f->hd.stream_id;
    return 0;
}
static int header(nghttp2_session *s, const nghttp2_frame *f, const uint8_t *name, size_t nn,
                  const uint8_t *value, size_t vn, uint8_t flags, void *ctx) {
    (void)s; (void)flags;
    struct h2 *h = ctx;
    const char *names[] = {":method", ":scheme", ":authority", ":path", "authorization", "content-type"};
    const char *values[] = {"POST", "https", QDC_APNS_HOST, "/3/device/arm-loopback-synthetic",
                           "bearer synthetic-probe", "application/json"};
    if (f->hd.stream_id != h->stream || ++h->fields > 6) return NGHTTP2_ERR_CALLBACK_FAILURE;
    for (unsigned i = 0; i < 6; i++)
        if (nn == strlen(names[i]) && !memcmp(name, names[i], nn)) {
            if ((h->mask & (1u << i)) || vn != strlen(values[i]) || memcmp(value, values[i], vn))
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            h->mask |= 1u << i;
            return 0;
        }
    return NGHTTP2_ERR_CALLBACK_FAILURE;
}
static int request_data(nghttp2_session *s, uint8_t flags, int32_t stream, const uint8_t *p,
                        size_t n, void *ctx) {
    (void)s; (void)flags;
    struct h2 *h = ctx;
    if (stream != h->stream || n > sizeof(h->request) - h->request_bytes)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    memcpy(h->request + h->request_bytes, p, n);
    h->request_bytes += n;
    return 0;
}
static nghttp2_ssize response_data(nghttp2_session *s, int32_t stream, uint8_t *out, size_t n,
                                   uint32_t *flags, nghttp2_data_source *source, void *ctx) {
    (void)s; (void)source;
    struct h2 *h = ctx;
    if (stream != h->stream) return NGHTTP2_ERR_CALLBACK_FAILURE;
    size_t left = sizeof(body) - 1 - h->response_bytes;
    if (n > left) n = left;
    memcpy(out, body + h->response_bytes, n);
    h->response_bytes += n;
    if (h->response_bytes == sizeof(body) - 1) *flags |= NGHTTP2_DATA_FLAG_EOF;
    return (nghttp2_ssize)n;
}
static int frame_received(nghttp2_session *s, const nghttp2_frame *f, void *ctx) {
    struct h2 *h = ctx;
    if (f->hd.stream_id == h->stream && (f->hd.type == NGHTTP2_HEADERS || f->hd.type == NGHTTP2_DATA) &&
        (f->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        if (h->complete || h->mask != 63 || h->fields != 6 || h->request_bytes != 2 ||
            memcmp(h->request, "{}", 2)) return NGHTTP2_ERR_CALLBACK_FAILURE;
        h->complete = 1;
        nghttp2_nv headers[] = {nv(":status", "200"), nv("date", h->date),
                                 nv("content-type", "application/json"), nv("content-length", "12")};
        nghttp2_data_provider2 provider = {.read_callback = response_data};
        if (nghttp2_submit_response2(s, h->stream, headers, 4, &provider))
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        puts("QDC_TLS_PEER_REQUEST_COMPLETE method=POST headers=6 body_bytes=2 request_end_stream=1");
    }
    return 0;
}
static int frame_sent(nghttp2_session *s, const nghttp2_frame *f, void *ctx) {
    (void)s;
    struct h2 *h = ctx;
    if (f->hd.stream_id == h->stream && (f->hd.type == NGHTTP2_HEADERS || f->hd.type == NGHTTP2_DATA) &&
        (f->hd.flags & NGHTTP2_FLAG_END_STREAM)) h->sent_end = 1;
    return 0;
}
static int stream_closed(nghttp2_session *s, int32_t stream, uint32_t code, void *ctx) {
    (void)s;
    struct h2 *h = ctx;
    if (stream != h->stream || code != NGHTTP2_NO_ERROR) h->failed = 1;
    else h->closed = 1;
    return 0;
}
static int serve_h2(mbedtls_ssl_context *ssl, struct io *io) {
    struct h2 h = {0};
    time_t now = time(NULL);
    struct tm tm;
    if (!gmtime_r(&now, &tm) || !strftime(h.date, sizeof(h.date), "%a, %d %b %Y %H:%M:%S GMT", &tm))
        return -1;
    nghttp2_session_callbacks *cb = NULL;
    nghttp2_session *session = NULL;
    int rc = -1;
    if (nghttp2_session_callbacks_new(&cb)) goto done;
    nghttp2_session_callbacks_set_on_begin_headers_callback(cb, begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cb, header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, request_data);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cb, frame_received);
    nghttp2_session_callbacks_set_on_frame_send_callback(cb, frame_sent);
    nghttp2_session_callbacks_set_on_stream_close_callback(cb, stream_closed);
    if (nghttp2_session_server_new(&session, cb, &h)) goto done;
    nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 1},
                                         {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, 2048}};
    if (nghttp2_submit_settings(session, 0, settings, 2)) goto done;
    while (lab_now_ms() < io->deadline && !h.failed) {
        const uint8_t *p;
        nghttp2_ssize n;
        while ((n = nghttp2_session_mem_send2(session, &p)) > 0)
            if (write_all(ssl, io, p, (size_t)n)) goto done;
        if (n < 0) goto done;
        if (h.complete && h.sent_end && h.closed && h.response_bytes == sizeof(body) - 1) {
            puts("QDC_TLS_PEER_RESPONSE_COMPLETE status=200 body_bytes=12 response_end_stream=1");
            /* Keep reading SETTINGS acknowledgements until the client has
             * consumed END_STREAM and closed. Closing with unread ACKs can
             * reset this real TCP connection and discard the response. */
            while (lab_now_ms() < io->deadline) {
                uint8_t trailing[4096];
                int got = mbedtls_ssl_read(ssl, trailing, sizeof(trailing));
                if (got == 0 || got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
                    got == MBEDTLS_ERR_NET_CONN_RESET) {
                    printf("QDC_TLS_PEER_CLIENT_CLOSED closed=1 tls_code=%d\n", got);
                    rc = 0;
                    goto done;
                }
                if (got > 0) {
                    if (nghttp2_session_mem_recv2(session, trailing, (size_t)got) != got) goto done;
                } else if (!again(io, got)) goto done;
            }
            goto done;
        }
        uint8_t input[4096];
        int got = read_some(ssl, io, input, sizeof(input));
        if (got < 0 || nghttp2_session_mem_recv2(session, input, (size_t)got) != got) goto done;
    }
done:
    if (session) nghttp2_session_del(session);
    if (cb) nghttp2_session_callbacks_del(cb);
    return rc;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(15);
    if (argc != 3 || lab_runtime()) return 64;
    int negative = !strcmp(argv[1], "wrongname") || !strcmp(argv[1], "wrongca");
    int is12 = negative || !strcmp(argv[1], "tls12");
    if (!is12 && !negative && strcmp(argv[1], "tls13")) return 64;
    char *end;
    long ready = strtol(argv[2], &end, 10);
    if (*end || ready < 3 || ready > 1024) return 64;
    struct io io = {.fd = -1, .deadline = lab_now_ms() + 9000};
    int listener = -1, rc = 1, tls_error = 0;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cert); mbedtls_pk_init(&key);
    if (psa_crypto_init() != PSA_SUCCESS || mbedtls_x509_crt_parse_file(&cert, "/fixtures/server.pem") ||
        mbedtls_pk_parse_keyfile(&key, "/fixtures/server.key", NULL, rng, NULL) ||
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT)) goto done;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, rng, NULL);
    mbedtls_ssl_protocol_version version = is12 ? MBEDTLS_SSL_VERSION_TLS1_2 : MBEDTLS_SSL_VERSION_TLS1_3;
    mbedtls_ssl_conf_min_tls_version(&conf, version);
    mbedtls_ssl_conf_max_tls_version(&conf, version);
    const char *protocols[] = {"h2", NULL};
    if (mbedtls_ssl_conf_own_cert(&conf, &cert, &key) ||
        mbedtls_ssl_conf_alpn_protocols(&conf, protocols) || mbedtls_ssl_setup(&ssl, &conf)) goto done;
    listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int yes = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(8443),
                                 .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1)) goto done;
    if (write((int)ready, "R", 1) != 1) goto done;
    close((int)ready); ready = -1;
    struct io accept_io = {.fd = listener, .deadline = io.deadline};
    if (wait_io(&accept_io, POLLIN)) goto done;
    io.fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    close(listener); listener = -1;
    if (io.fd < 0) goto done;
    mbedtls_ssl_set_bio(&ssl, &io, send_bytes, recv_bytes, NULL);
    while ((tls_error = mbedtls_ssl_handshake(&ssl)) != 0)
        if (!again(&io, tls_error)) {
            const char *actual = mbedtls_ssl_get_version(&ssl);
            if (negative && actual && !strcmp(actual, "TLSv1.2") && !receive_certificate_alert(&io)) {
                printf("QDC_TLS_PEER_CERTIFICATE_REJECT version=%s fatal_alert=1 description=%u tls_code=%d h2_requests=0\n",
                       actual, io.records.description, tls_error);
                rc = 0;
            }
            goto done;
        }
    const char *negotiated = mbedtls_ssl_get_version(&ssl);
    const char *alpn = mbedtls_ssl_get_alpn_protocol(&ssl);
    if (negative || !negotiated || strcmp(negotiated, is12 ? "TLSv1.2" : "TLSv1.3") ||
        !alpn || strcmp(alpn, "h2")) goto done;
    printf("QDC_TLS_PEER_NEGOTIATED version=%s alpn=h2\n", negotiated);
    if (!serve_h2(&ssl, &io)) {
        printf("QDC_TLS_PEER_PASS case=%s\n", argv[1]);
        rc = 0;
    }
done:
    if (rc) printf("QDC_TLS_PEER_FAILED case=%s tls_code=%d\n", argv[1], tls_error);
    if (ready >= 0) close((int)ready);
    if (listener >= 0) close(listener);
    if (io.fd >= 0) close(io.fd);
    mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&cert); mbedtls_pk_free(&key);
    return rc;
}
