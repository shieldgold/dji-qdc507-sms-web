#include "qdc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    qdc_buf b = {0};
    const unsigned char raw[] = {'A', 0, 'B'};
    assert(qdc_buf_append(&b, raw, 3) == 0 && b.len == 3 && !memcmp(b.data, raw, 3));
    assert(qdc_buf_reserve(&b, SIZE_MAX) == -1);
    qdc_buf_free(&b);
    qdc_error e = {0};
    const char *s = "{\"n\":18446744073709551615,\"s\":\"A\\u0000B\"}";
    yyjson_doc *d = qdc_json_read(s, strlen(s), &e);
    assert(d);
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(d), "n")) == UINT64_MAX);
    qdc_slice v = qdc_json_slice(yyjson_obj_get(yyjson_doc_get_root(d), "s"));
    assert(v.len == 3 && !memcmp(v.ptr, raw, 3));
    char out[4];
    assert(qdc_json_cstr(yyjson_obj_get(yyjson_doc_get_root(d), "s"), out, 4) == -1);
    yyjson_doc_free(d);
    yyjson_mut_doc *m = qdc_json_object();
    const unsigned char bad[] = {0xff, 'A', 0, 0xe2, 0x82};
    assert(qdc_json_add_string(m, yyjson_mut_doc_get_root(m), "body", bad, sizeof(bad)));
    qdc_response r = {0};
    assert(qdc_response_json(&r, 200, m) == 0);
    d = qdc_json_read(r.body.data, r.body.len, &e);
    assert(d);
    v = qdc_json_slice(yyjson_obj_get(yyjson_doc_get_root(d), "body"));
    const unsigned char expected[] = {0xef, 0xbf, 0xbd, 'A', 0, 0xef, 0xbf, 0xbd, 0xef, 0xbf, 0xbd};
    assert(v.len == sizeof(expected) && !memcmp(v.ptr, expected, v.len));
    yyjson_doc_free(d);
    qdc_response_free(&r);
    const char *requests[] = {"{\"number\":\"10010\",\"NUMBER\":\"10086\",\"number\":\"10000\"}",
                              "{\"number\":\"10010\",\"number\":null}",
                              "{\"Number\":123,\"number\":\"10010\"}"};
    for (size_t i = 0; i < 3; i++) {
        qdc_request req = {.path = "/api/send", .body = {requests[i], strlen(requests[i])}};
        yyjson_val *v = qdc_request_json(&req, &e);
        if (i == 2)
            assert(!v);
        else {
            assert(v);
            assert(
                qdc_slice_eq(qdc_json_slice(yyjson_obj_get(v, "number")), i ? "10010" : "10000"));
        }
        if (req.json)
            yyjson_doc_free(req.json);
    }
    char tmp[] = "/tmp/qdc-common-XXXXXX";
    assert(mkdtemp(tmp));
    char path[4096];
    assert(qdc_path(path, sizeof(path), tmp, "state.json") == 0);
    assert(qdc_atomic_write(path, raw, 3, &e) == 0);
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(qdc_read_file(path, 2, &b, &e) == -1);
    qdc_buf_free(&b);
    assert(qdc_read_file(path, 3, &b, &e) == 0 && b.len == 3 && !memcmp(b.data, raw, 3));
    qdc_buf_free(&b);
    assert(qdc_query("a=hello+world&b=A%00B", "b", &b) == 1 && b.len == 3 &&
           !memcmp(b.data, raw, 3));
    qdc_buf_free(&b);
    int64_t ms;
    assert(qdc_parse_rfc3339((qdc_slice){"2026-11-15T12:34:56+08:00", 25}, &ms) == 0);
    char stamp[40];
    qdc_rfc3339(ms, stamp);
    assert(!strcmp(stamp, "2026-11-15T04:34:56Z"));
    char pending[4096];
    assert(snprintf(pending, sizeof(pending), "%s.tmp", path) > 0);
    assert(mkdir(pending, 0700) == 0);
    assert(qdc_atomic_write(path, "new", 3, &e) == -1 && !e.uncertain);
    assert(qdc_read_file(path, 3, &b, &e) == 0 && !memcmp(b.data, raw, 3));
    qdc_buf_free(&b);
    rmdir(pending);
    FILE *left = fopen(pending, "wb");
    assert(left);
    assert(fwrite("stale", 1, 5, left) == 5);
    fclose(left);
    assert(qdc_atomic_write(path, "new", 3, &e) == 0);
    assert(qdc_read_file(pending, 5, &b, &e) == 0 && b.len == 5 && !memcmp(b.data, "stale", 5));
    qdc_buf_free(&b);
    unlink(pending);
    unlink(path);
    rmdir(tmp);
    puts("common: PASS");
    return 0;
}
