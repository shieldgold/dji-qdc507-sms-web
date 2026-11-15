#include "qdc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Parser tests use a real socketpair and a pure echo handler. Full application
 * authentication is covered separately with the modem PTY fixture. */
int qdc_dispatch(qdc_app *app, qdc_request *req, qdc_response *resp) {
    (void)app;
    resp->status = 200;
    resp->content_type = "application/octet-stream";
    return qdc_buf_append(&resp->body, req->body.ptr, req->body.len);
}
struct work {
    qdc_app *app;
    int fd;
};
static void *serve(void *p) {
    struct work *w = p;
    qdc_http_conn(w->app, w->fd);
    return NULL;
}
static qdc_buf exchange(const void *data, size_t len) {
    int f[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, f) == 0);
    qdc_app a = {0};
    struct work w = {&a, f[1]};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, serve, &w) == 0);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(f[0], (const char *)data + off, len - off);
        assert(n > 0);
        off += (size_t)n;
    }
    shutdown(f[0], SHUT_WR);
    qdc_buf out = {0};
    char b[512];
    ssize_t n;
    while ((n = read(f[0], b, sizeof(b))) > 0)
        assert(qdc_buf_append(&out, b, (size_t)n) == 0);
    close(f[0]);
    pthread_join(thread, NULL);
    return out;
}
int main(void) {
    const char raw[] = "POST /echo?q=1 HTTP/1.1\r\nContent-Length: 3\r\nCookie: t=x\r\n\r\nA\0B";
    qdc_buf b = exchange(raw, sizeof(raw) - 1);
    assert(b.len > 3 && strstr((char *)b.data, "200 OK"));
    assert(!memcmp(b.data + b.len - 3, "A\0B", 3));
    qdc_buf_free(&b);
    const char *cases[] = {"Content-Length: -1", "Content-Length: 99999999",
                           "Transfer-Encoding: chunked", "Content-Length: 1\r\nContent-Length: 2"};
    const char *status[] = {"400 Bad Request", "413 Payload Too Large", "400 Bad Request",
                            "400 Bad Request"};
    for (size_t i = 0; i < 4; i++) {
        char r[256];
        int z = snprintf(r, sizeof(r), "POST /echo HTTP/1.1\r\n%s\r\n\r\n", cases[i]);
        b = exchange(r, (size_t)z);
        assert(b.len && strstr((char *)b.data, status[i]));
        qdc_buf_free(&b);
    }
    qdc_error e = {0};
    assert(qdc_listen_address("127.0.0.1", "8787", &e) == 0);
    assert(qdc_listen_address("0.0.0.0", "8787", &e) == -1);
    assert(qdc_listen_address("192.168.225.1", "65536", &e) == -1);
    puts("http: PASS");
    return 0;
}
