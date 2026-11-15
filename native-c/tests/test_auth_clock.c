#include "qdc_auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void response(qdc_app *a, const char *body, int status) {
    qdc_request r = {.method = "POST", .path = "/api/time", .body = {body, strlen(body)}};
    qdc_response s = {0};
    CHECK(qdc_clock_route(a, &r, &s) == 1 && s.status == status);
    if (r.json)
        yyjson_doc_free(r.json);
    qdc_response_free(&s);
}
int main(void) {
    char dir[] = "/tmp/qdc-clock-XXXXXX", path[4096], tmp[4096];
    CHECK(mkdtemp(dir));
    qdc_app a = {.config = {.data_dir = dir}};
    qdc_error e = {0};
    CHECK(qdc_clock_init(&a, &e) == 0);
    int64_t ref = 1794704523123LL, wall = qdc_system_ms();
    CHECK(llabs(qdc_now_ms(&a) - wall) < 1000);
    CHECK(qdc_clock_sync(&a, ref, &e) == 0);
    CHECK(qdc_now_ms(&a) >= ref && qdc_now_ms(&a) - ref < 1000);
    CHECK(qdc_path(path, sizeof(path), dir, "clock.json") == 0);
    struct stat st;
    CHECK(!stat(path, &st) && (st.st_mode & 0777) == 0600);
    qdc_buf b = {0};
    CHECK(!qdc_read_file(path, 4096, &b, &e));
    yyjson_doc *d = qdc_json_read(b.data, b.len, &e);
    CHECK(d);
    int64_t saved;
    CHECK(
        !qdc_parse_rfc3339(qdc_json_slice(yyjson_obj_get(yyjson_doc_get_root(d), "utc")), &saved));
    CHECK(saved == ref);
    yyjson_doc_free(d);
    qdc_buf_free(&b);
    qdc_clock_free(&a);
    CHECK(!qdc_clock_init(&a, &e));
    CHECK(qdc_now_ms(&a) >= ref && qdc_now_ms(&a) - ref < 1000);
    CHECK(!qdc_path(tmp, sizeof(tmp), dir, "clock.json.tmp"));
    CHECK(!mkdir(tmp, 0700));
    CHECK(qdc_clock_sync(&a, ref + 86400000, &e) < 0);
    CHECK(qdc_now_ms(&a) < ref + 1000);
    CHECK(!rmdir(tmp));
    CHECK(qdc_clock_sync(&a, 0, &e) < 0);
    CHECK(strstr(e.message, "设备时间无效"));
    response(&a, "{\"unix_ms\":1794704523123}", 200);
    response(&a, "{\"unix_ms\":1794704523123.5}", 400);
    response(&a, "{\"unix_ms\":9223372036854775808}", 400);
    response(&a, "{\"unix_ms\":\"1794704523123\"}", 400);
    response(&a, "null", 400);
    qdc_clock_free(&a);
    const char *future =
        "{\"utc\":\"2026-11-15T09:02:03.123456789+08:00\",\"system\":\"2099-01-01T00:00:00Z\"}";
    CHECK(!qdc_atomic_write(path, future, strlen(future), &e));
    CHECK(!qdc_clock_init(&a, &e));
    CHECK(qdc_now_ms(&a) >= ref && qdc_now_ms(&a) - ref < 1000);
    CHECK(llabs(qdc_system_ms() - wall) < 3000);
    qdc_clock_free(&a);
    unlink(path);
    rmdir(dir);
    puts("clock: PASS (fraction, restart, reset, rollback, HTTP int64)");
    return 0;
}
