#include "qdc_push.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef QDC_TLS_CLOCK_STUB
int64_t qdc_now_ms(qdc_app *app) {
    (void)app;
    return qdc_system_ms();
}
#endif
#ifdef QDC_TEST_RED
int qdc_http_date(qdc_slice s, int64_t *m) {
    (void)s;
    (void)m;
    return -1;
}
int qdc_dns_answer(const unsigned char *p, size_t n, uint16_t id, const char *h, char a[16],
                   char c[254], bool *tc, qdc_error *e) {
    (void)p;
    (void)n;
    (void)id;
    (void)h;
    (void)a;
    (void)c;
    (void)tc;
    (void)e;
    return -1;
}
#endif
static void unit(void) {
    int64_t ms = 0;
    assert(qdc_http_date((qdc_slice){"Sun, 06 Nov 1994 08:49:37 GMT", 29}, &ms) == 0 &&
           ms == 784111777000LL);
    assert(qdc_http_date((qdc_slice){"Fri, 31 Feb 2026 00:00:00 GMT", 29}, &ms) == -1);
    assert(qdc_http_date((qdc_slice){"Sun, 06 Nov 1994 08:49:37 GMTjunk", 33}, &ms) == -1);
    unsigned char p[] = {0x12, 0x34, 0x81, 0x80, 0,  1,   0,   1,    0,   0,   0, 0,
                         3,    'a',  'p',  'i',  5,  'a', 'p', 'p',  'l', 'e', 3, 'c',
                         'o',  'm',  0,    0,    1,  0,   1,   0xc0, 12,  0,   1, 0,
                         1,    0,    0,    0,    60, 0,   4,   127,  0,   0,   1};
    char a[16], c[254];
    bool tc;
    qdc_error e = {0};
    assert(qdc_dns_answer(p, sizeof(p), 0x1234, "api.apple.com", a, c, &tc, &e) == 0 &&
           !strcmp(a, "127.0.0.1"));
    assert(qdc_dns_answer(p, sizeof(p), 0x1235, "api.apple.com", a, c, &tc, &e) == -1);
    assert(qdc_dns_answer(p, sizeof(p), 0x1234, "other.apple.com", a, c, &tc, &e) == -1);
    assert(qdc_dns_answer(p, sizeof(p) - 1, 0x1234, "api.apple.com", a, c, &tc, &e) == -1);
    p[12] = 0xc0;
    p[13] = 12;
    assert(qdc_dns_answer(p, sizeof(p), 0x1234, "api.apple.com", a, c, &tc, &e) == -1);
    puts(
        "tls unit: PASS (HTTP Date validation, DNS ID/question/truncation/pointer-loop rejection)");
}
#ifndef QDC_TEST_RED
int main(int argc, char **argv) {
    if (argc == 1) {
        unit();
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "dns")) {
        char ip[16];
        qdc_error e = {0};
        assert(qdc_dns_test_server("127.0.0.1", (unsigned)atoi(argv[2])) == 0);
        int rc = qdc_dns_resolve(QDC_APNS_HOST, ip, qdc_mono_ms() + 2500, &e);
        if (rc) {
            fprintf(stderr, "DNS rejected: %s\n", e.message);
            return 1;
        }
        puts(ip);
        return 0;
    }
    if (argc != 8)
        return 2;
    qdc_app app = {0};
    qdc_error e = {0};
    qdc_tls_response out = {0};
    assert(qdc_tls_test_endpoint("192.0.2.1", 443, "localhost") == -1);
    assert(qdc_tls_test_endpoint("127.0.0.1", (unsigned)atoi(argv[2]), argv[4]) == 0);
    qdc_tls_header h[] = {{"authorization", "bearer synthetic-probe"},
                          {"content-type", "application/json"}};
    qdc_tls_request r = {.app = &app,
                         .host = atoi(argv[5]) ? QDC_APNS_HOST : QDC_CLOCK_HOST,
                         .method = atoi(argv[5]) ? "POST" : "GET",
                         .path = argv[6],
                         .ca_file = argv[3],
                         .headers = h,
                         .header_count = 2,
                         .body = {"{}", 2},
                         .timeout_ms = (unsigned)atoi(argv[7]),
                         .require_h2 = atoi(argv[5]) != 0};
    int rc = qdc_tls_fetch(&r, &out, &e);
    if (rc)
        fprintf(stderr, "TLS rejected: %s\n", e.message);
    else
        printf("status=%d h2=%d date=%s body=%.*s\n", out.status, out.http2, out.date,
               (int)out.body.len, out.body.data ? (char *)out.body.data : "");
    qdc_tls_response_free(&out);
    return rc ? 1 : 0;
}
#else
int main(void) {
    unit();
    return 0;
}
#endif
