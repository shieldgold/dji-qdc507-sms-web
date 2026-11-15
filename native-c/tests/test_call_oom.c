#include "qdc_domain.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static bool fail_request_id;
static bool injected_json_add_string(yyjson_mut_doc *, yyjson_mut_val *, const char *, const void *,
                                     size_t);
#define qdc_json_add_string injected_json_add_string
#include "../src/calls.c"
#undef qdc_json_add_string
static bool injected_json_add_string(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k,
                                     const void *v, size_t n) {
    if (fail_request_id && !strcmp(k, "request_id")) {
        fail_request_id = false;
        return false;
    }
    return qdc_json_add_string(d, o, k, v, n);
}
static unsigned dial_count;
int qdc_command(qdc_app *a, const char *cmd, unsigned ms, qdc_buf *out, qdc_error *e) {
    (void)a;
    (void)ms;
    (void)out;
    (void)e;
    if (!strncmp(cmd, "ATD", 3))
        dial_count++;
    return 0;
}
int64_t qdc_now_ms(qdc_app *a) {
    (void)a;
    return 1794690000000LL;
}
int main(void) {
    char dir[] = "/tmp/qdc-review-call-XXXXXX";
    assert(mkdtemp(dir));
    qdc_domain domain = {0};
    qdc_app app = {.domain = &domain, .config = {.data_dir = dir}};
    qdc_error e = {0};
    assert(!qdc_calls_init(&app, &e));
    yyjson_doc *snap = NULL;
    fail_request_id = true;
    int first =
        qdc_calls_dial(&app, (qdc_slice){"10086", 5}, (qdc_slice){"request-once", 12}, &snap, &e);
    if (snap)
        yyjson_doc_free(snap);
    char path[QDC_PATH_MAX];
    qdc_path(path, sizeof(path), dir, "call-attempts.json");
    assert(first == -1 && dial_count == 0);
    qdc_calls_free(&app);
    assert(!qdc_calls_init(&app, &e));
    int retry =
        qdc_calls_dial(&app, (qdc_slice){"10086", 5}, (qdc_slice){"request-once", 12}, &snap, &e);
    if (snap)
        yyjson_doc_free(snap);
    printf("retry_rc=%d total_dials=%u\n", retry, dial_count);
    qdc_calls_free(&app);
    unlink(path);
    rmdir(dir);
    assert(first == -1 && retry == 0 && dial_count == 1);
    return 0;
}
