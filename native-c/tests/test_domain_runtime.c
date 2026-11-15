#include "qdc_auth.h"
#include "qdc_domain.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static qdc_error e;
static yyjson_doc *json(const char *s) {
    yyjson_doc *d = qdc_json_read(s, strlen(s), &e);
    assert(d);
    return d;
}
static void command(qdc_app *a, const char *s) {
    qdc_buf b = {0};
    assert(qdc_command(a, s, 2000, &b, &e) == 0);
    qdc_buf_free(&b);
}
static void counts(qdc_app *a, int pin, int sms, int dial, int hang) {
    qdc_buf b = {0};
    assert(!qdc_command(a, "AT+LABCOUNTS", 2000, &b, &e));
    char expected[100];
    snprintf(expected, sizeof expected, "+LAB: %d,%d,%d,%d\n", pin, sms, dial, hang);
    assert(b.len >= strlen(expected) && !memcmp(b.data, expected, strlen(expected)));
    qdc_buf_free(&b);
}
static yyjson_doc *finish(qdc_app *a, const char *id, const char *state) {
    uint64_t end = qdc_mono_ms() + 5000;
    while (qdc_outbox_busy(a) && qdc_mono_ms() < end)
        qdc_sleep(a, 1);
    assert(!qdc_outbox_busy(a));
    char key[128];
    snprintf(key, sizeof key, "out:%s", id);
    yyjson_doc *m = qdc_store_get(a, (qdc_slice){key, strlen(key)});
    assert(m);
    assert(
        qdc_slice_eq(qdc_json_slice(yyjson_obj_get(yyjson_doc_get_root(m), "send_state")), state));
    return m;
}
static void enqueue(qdc_app *a, const char *id, const char *text, size_t len) {
    yyjson_doc *m = NULL;
    assert(!qdc_outbox_enqueue(a, (qdc_slice){id, strlen(id)}, (qdc_slice){"10010", 5},
                               (qdc_slice){text, len}, &m, &e));
    assert(m);
    yyjson_doc_free(m);
}
static void init(qdc_app *a, const char *dir, const char *path) {
    memset(a, 0, sizeof *a);
    a->config = (qdc_config){
        .data_dir = dir, .at_path = path, .listen = "127.0.0.1", .port = "8787", .poll_ms = 3000};
    pthread_mutex_init(&a->action_mu, NULL);
    pthread_mutex_init(&a->op_mu, NULL);
    atomic_init(&a->stopping, false);
    assert(!qdc_clock_init(a, &e));
    assert(!qdc_domain_init(a, &e));
    assert(!qdc_auth_init(a, &e));
    assert(!qdc_at_open(a, &e));
}
static void release(qdc_app *a) {
    qdc_domain_free(a);
    qdc_auth_free(a);
    qdc_clock_free(a);
    pthread_mutex_destroy(&a->action_mu);
    pthread_mutex_destroy(&a->op_mu);
}
int main(int argc, char **argv) {
    assert(argc == 3 && getenv("QDC_SYNTHETIC_PTY"));
    qdc_app a;
    init(&a, argv[1], argv[2]);
    yyjson_doc *m = NULL;
    /* Intent persistence failure must not reach CMGS. */
    char path[QDC_PATH_MAX];
    qdc_path(path, sizeof path, argv[1], "messages.json.tmp");
    assert(!mkdir(path, 0700));
    assert(qdc_outbox_enqueue(&a, (qdc_slice){"request-savefail", 16}, (qdc_slice){"10010", 5},
                              (qdc_slice){"test", 4}, &m, &e) < 0);
    assert(!m);
    counts(&a, 0, 0, 0, 0);
    assert(!rmdir(path));
    /* A blocked guard must survive and must not consume a PIN attempt. */
    qdc_path(path, sizeof path, argv[1], "pin-unlock.pending");
    assert(!mkdir(path, 0700));
    assert(qdc_pin_unlock(&a, (qdc_slice){"1234", 4}, (qdc_slice){"guard", 5}, &m, &e) < 0);
    yyjson_doc_free(m);
    counts(&a, 0, 0, 0, 0);
    assert(!rmdir(path));
    assert(!qdc_pin_unlock(&a, (qdc_slice){"1234", 4}, (qdc_slice){"unlock", 6}, &m, &e));
    yyjson_doc_free(m);
    assert(!qdc_pin_unlock(&a, (qdc_slice){"1234", 4}, (qdc_slice){"again", 5}, &m, &e));
    yyjson_doc_free(m);
    counts(&a, 1, 0, 0, 0);
    char chinese[301];
    for (size_t i = 0; i < 100; i++)
        memcpy(chinese + i * 3, "中", 3);
    chinese[300] = 0;
    enqueue(&a, "request-once", chinese, 300);
    m = finish(&a, "request-once", "submitted");
    yyjson_doc_free(m);
    enqueue(&a, "request-once", chinese, 300);
    counts(&a, 1, 2, 0, 0);
    const char nul[] = {'A', 0, 'B'}, other[] = {'A', 0, 'C'};
    enqueue(&a, "request-NUL", nul, 3);
    m = finish(&a, "request-NUL", "submitted");
    qdc_slice body = qdc_json_slice(yyjson_obj_get(yyjson_doc_get_root(m), "body"));
    assert(body.len == 3 && !memcmp(body.ptr, nul, 3));
    yyjson_doc_free(m);
    assert(qdc_outbox_enqueue(&a, (qdc_slice){"request-NUL", 11}, (qdc_slice){"10010", 5},
                              (qdc_slice){other, 3}, &m, &e) < 0);
    counts(&a, 1, 3, 0, 0);
    command(&a, "AT+LABMODE=partial");
    enqueue(&a, "request-partial", chinese, 300);
    m = finish(&a, "request-partial", "partial");
    assert(yyjson_get_sint(yyjson_obj_get(yyjson_doc_get_root(m), "sent_parts")) == 1);
    yyjson_doc_free(m);
    enqueue(&a, "request-partial", chinese, 300);
    counts(&a, 1, 5, 0, 0);
    command(&a, "AT+LABMODE=okonly");
    enqueue(&a, "request-uncertain", "test", 4);
    m = finish(&a, "request-uncertain", "uncertain");
    yyjson_doc_free(m);
    enqueue(&a, "request-uncertain", "test", 4);
    counts(&a, 1, 6, 0, 0);
    command(&a, "AT+LABCALL=incoming");
    assert(!qdc_calls_refresh(&a, &e));
    m = qdc_calls_snapshot(&a);
    yyjson_val *arr = yyjson_obj_get(yyjson_doc_get_root(m), "calls");
    assert(yyjson_arr_size(arr) == 1);
    char id[32];
    assert(!qdc_json_cstr(yyjson_obj_get(yyjson_arr_get(arr, 0), "id"), id, sizeof id));
    yyjson_doc_free(m);
    assert(qdc_calls_hangup(&a, "old-id", &e) < 0);
    counts(&a, 1, 6, 0, 0);
    assert(qdc_calls_action(&a, id, "dtmf", (qdc_slice){"1", 1}, &e) < 0);
    assert(!qdc_calls_action(&a, id, "answer", (qdc_slice){"", 0}, &e));
    assert(!qdc_calls_action(&a, id, "dtmf", (qdc_slice){"#", 1}, &e));
    assert(qdc_calls_action(&a, id, "dtmf", (qdc_slice){"1\rATH", 5}, &e) < 0);
    assert(!qdc_calls_hangup(&a, id, &e));
    counts(&a, 1, 6, 0, 1);
    command(&a, "AT+LABCALL=none");
    assert(qdc_calls_dial(&a, (qdc_slice){"10086", 5}, (qdc_slice){"request-dial", 12}, &m, &e) <
           0);
    yyjson_doc_free(m);
    assert(qdc_calls_dial(&a, (qdc_slice){"10086", 5}, (qdc_slice){"request-dial", 12}, &m, &e) <
           0);
    yyjson_doc_free(m);
    counts(&a, 1, 6, 1, 1);
    /* Validate every record before touching live archive or the backup. */
    yyjson_doc *before = qdc_messages(&a);
    size_t before_count = yyjson_arr_size(yyjson_doc_get_root(before));
    yyjson_doc_free(before);
    int added = 0;
    m = json("[{\"key\":\"new\",\"sender\":\"10010\",\"body\":\"ok\",\"direction\":\"in\"},{"
             "\"key\":\"bad\",\"sender\":\"10010\",\"body\":42,\"direction\":\"in\"}]");
    assert(qdc_store_restore(&a, yyjson_doc_get_root(m), &added, &e) < 0);
    yyjson_doc_free(m);
    m = qdc_messages(&a);
    assert(yyjson_arr_size(yyjson_doc_get_root(m)) == before_count);
    yyjson_doc_free(m);
    m = json("[{\"key\":\"import\",\"sender\":\"10010\",\"body\":\"ok\",\"direction\":\"out\","
             "\"index\":99,\"send_state\":\"sending\",\"request_id\":\"request-import\"}]");
    assert(!qdc_store_restore(&a, yyjson_doc_get_root(m), &added, &e) && added == 1);
    yyjson_doc_free(m);
    m = qdc_store_get(&a, (qdc_slice){"import", 6});
    assert(yyjson_get_sint(yyjson_obj_get(yyjson_doc_get_root(m), "index")) == 0);
    assert(qdc_slice_eq(qdc_json_slice(yyjson_obj_get(yyjson_doc_get_root(m), "send_state")),
                        "uncertain"));
    yyjson_doc_free(m);
    qdc_buf long_line = {0};
    assert(!qdc_command(&a, "AT+LABLONG", 3000, &long_line, &e));
    assert(long_line.len == 65540);
    for (size_t i = 0; i < 65536; i++)
        assert(long_line.data[i] == 'A');
    qdc_buf_free(&long_line);
    release(&a);
    init(&a, argv[1], argv[2]);
    assert(qdc_calls_dial(&a, (qdc_slice){"10086", 5}, (qdc_slice){"request-dial", 12}, &m, &e) <
           0);
    yyjson_doc_free(m);
    enqueue(&a, "request-uncertain", "test", 4);
    counts(&a, 1, 6, 1, 1);
    release(&a);
    puts("DOMAIN_RUNTIME_PASS persistence PIN NUL outbox_partial_uncertain stale_call restore "
         "long_AT restart");
    return 0;
}
