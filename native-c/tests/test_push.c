#include "qdc_push.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef QDC_TEST_RED
void qdc_call_push_uuid(const char *id, char out[37]) {
    (void)id;
    out[0] = 0;
}
int main(void) {
    char uuid[37];
    qdc_call_push_uuid("abcd1234abcd1234abcd1234", uuid);
    assert(!strcmp(uuid, "0162a0a6-3d46-4427-95df-6cea76f87699"));
    return 0;
}
#else
static int64_t test_now = 1794700800000LL;
#ifdef QDC_PUSH_STANDALONE
int64_t qdc_now_ms(qdc_app *a) {
    (void)a;
    return test_now;
}
int qdc_clock_sync(qdc_app *a, int64_t ms, qdc_error *e) {
    (void)a;
    (void)e;
    test_now = ms;
    return 0;
}
int qdc_domain_identity(qdc_app *a, bool fresh, qdc_sim_identity *s, qdc_error *e) {
    (void)a;
    (void)fresh;
    (void)e;
    memset(s, 0, sizeof(*s));
    strcpy(s->iccid, "card-a");
    strcpy(s->key, "iccid:card-a");
    return 0;
}
bool qdc_auth_check(qdc_app *a, const char *cookie, const char *key) {
    (void)a;
    (void)cookie;
    (void)key;
    return true;
}
yyjson_doc *qdc_push_messages(qdc_app *a) {
    (void)a;
    return yyjson_read("[]", 2, 0);
}
yyjson_doc *qdc_calls_snapshot(qdc_app *a) {
    (void)a;
    return yyjson_read("{\"calls\":[]}", 12, 0);
}
#endif
static qdc_push_subscription subscription(void) {
    qdc_push_subscription s = {0};
    strcpy(s.iccid, "card-a");
    strcpy(s.device_id, "phone-a");
    memset(s.token, 'a', 64);
    memset(s.voip_token, 'b', 64);
    s.enabled = s.calls_enabled = true;
    return s;
}
static yyjson_doc *json(const char *s) {
    yyjson_doc *d = yyjson_read(s, strlen(s), 0);
    assert(d);
    return d;
}
static size_t queued(qdc_app *a) {
    yyjson_mut_doc *d = qdc_push_status(a);
    assert(d);
    size_t n =
        (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_doc_get_root(d), "queued"));
    yyjson_mut_doc_free(d);
    return n;
}
static size_t accepted(qdc_app *a) {
    yyjson_mut_doc *d = qdc_push_status(a);
    assert(d);
    size_t n =
        (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_doc_get_root(d), "accepted"));
    yyjson_mut_doc_free(d);
    return n;
}
static void init(qdc_app *a, char *dir) {
    memset(a, 0, sizeof(*a));
    a->config.data_dir = dir;
    pthread_mutex_init(&a->op_mu, NULL);
    pthread_mutex_init(&a->action_mu, NULL);
    atomic_init(&a->stopping, false);
    qdc_error e = {0};
    assert(qdc_push_init(a, &e) == 0);
}
static void free_app(qdc_app *a) {
    qdc_push_free(a);
    pthread_mutex_destroy(&a->op_mu);
    pthread_mutex_destroy(&a->action_mu);
}
struct delivery {
    int calls;
    qdc_push_result result;
    char id[37], token[513];
};
static qdc_push_result deliver(const qdc_push_delivery *d, void *context, qdc_error *e) {
    struct delivery *x = context;
    x->calls++;
    if (x->id[0])
        assert(!strcmp(x->id, d->event_id));
    else
        strcpy(x->id, d->event_id);
    strcpy(x->token, d->token);
    qdc_error_set(e, "synthetic offline");
    return x->result;
}
static void clear_dir(char *dir) {
    char path[4096];
    const char *files[] = {"push-state.json", "push-apns.json", "push-apns.p8", "push-ca.pem"};
    for (size_t i = 0; i < 4; i++) {
        qdc_path(path, sizeof(path), dir, files[i]);
        unlink(path);
    }
    rmdir(dir);
}
static int sender_cli(int argc, char **argv) {
    if (argc < 3)
        return 2;
    test_now = qdc_system_ms();
    qdc_app app;
    init(&app, argv[2]);
    qdc_error e = {0};
    if (!strcmp(argv[1], "inspect")) {
        yyjson_mut_doc *d = qdc_push_status(&app);
        char *s = yyjson_mut_write(d, 0, NULL);
        assert(s);
        puts(s);
        free(s);
        yyjson_mut_doc_free(d);
        free_app(&app);
        return 0;
    }
    if (!strcmp(argv[1], "jwt")) {
        int64_t offsets[] = {0, 49 * 60000, 51 * 60000, -60000};
        qdc_buf out[4] = {{0}};
        for (size_t i = 0; i < 4; i++)
            if (qdc_push_test_jwt(&app, test_now + offsets[i], &out[i], &e)) {
                for (size_t j = 0; j < 4; j++)
                    qdc_buf_free(&out[j]);
                free_app(&app);
                return 1;
            }
        assert(!strcmp((char *)out[0].data, (char *)out[1].data));
        assert(strcmp((char *)out[0].data, (char *)out[2].data));
        assert(strcmp((char *)out[2].data, (char *)out[3].data));
        for (size_t i = 0; i < 4; i++) {
            puts((char *)out[i].data);
            qdc_buf_free(&out[i]);
        }
        free_app(&app);
        return 0;
    }
    if (argc != 5)
        return 2;
    assert(qdc_tls_test_endpoint("127.0.0.1", (unsigned)atoi(argv[3]), "localhost") == 0);
    qdc_push_subscription s = subscription();
    yyjson_doc *empty = json("[]");
    assert(qdc_push_register(&app, &s, yyjson_doc_get_root(empty), &e) == 0);
    yyjson_doc_free(empty);
    int64_t now = qdc_system_ms();
    if (!strcmp(argv[4], "call")) {
        yyjson_doc *c = json("{\"calls\":[{\"id\":\"cccccccccccccccccccccccc\",\"direction\":"
                             "\"in\",\"state\":\"incoming\"}]}");
        assert(qdc_push_observe(&app, NULL, yyjson_doc_get_root(c), now, &e) == 0);
        yyjson_doc_free(c);
    } else {
        assert(qdc_push_test_event(&app, argv[4], now, &e) == 0);
        if (!strcmp(argv[4], "test"))
            now += 8000;
    }
    qdc_push_send_next(&app, "card-a", now);
    yyjson_mut_doc *d = qdc_push_status(&app);
    char *out = yyjson_mut_write(d, 0, NULL);
    assert(out);
    puts(out);
    free(out);
    yyjson_mut_doc_free(d);
    free_app(&app);
    return 0;
}
int main(int argc, char **argv) {
    if (argc > 1)
        return sender_cli(argc, argv);
    char uuid[37];
    qdc_call_push_uuid("abcd1234abcd1234abcd1234", uuid);
    assert(!strcmp(uuid, "0162a0a6-3d46-4427-95df-6cea76f87699"));
    qdc_push_delivery pd = {.count = 2};
    strcpy(pd.kind, "sms");
    strcpy(pd.event_id, "12345678-1234-1234-1234-123456789abc");
    memset(pd.token, 'a', 64);
    qdc_buf payload = {0};
    qdc_error e = {0};
    assert(qdc_apns_payload(&pd, &payload, &e) == 0);
    assert(!strstr((char *)payload.data, pd.token) && strstr((char *)payload.data, "2"));
    qdc_buf_free(&payload);
    pd.count = 0;
    assert(qdc_apns_payload(&pd, &payload, &e) == -1);
    char dir[] = "/tmp/qdc-push-XXXXXX";
    assert(mkdtemp(dir));
    qdc_app app;
    init(&app, dir);
    qdc_push_subscription s = subscription();
    yyjson_doc *old = json("[{\"key\":\"old\",\"direction\":\"in\"}]");
    assert(qdc_push_register(&app, &s, yyjson_doc_get_root(old), &e) == 0);
    yyjson_doc *messages = json(
        "[{\"key\":\"old\",\"direction\":\"in\"},{\"key\":\"new\",\"direction\":\"in\",\"body\":"
        "\"SECRET\",\"sender\":\"13800000000\"},{\"key\":\"out\",\"direction\":\"out\"}]");
    yyjson_doc *calls = json("{\"calls\":[]}");
    int64_t now = 1794700800000LL;
    assert(qdc_push_observe(&app, yyjson_doc_get_root(messages), yyjson_doc_get_root(calls), now,
                            &e) == 0 &&
           queued(&app) == 1);
    char path[4096];
    qdc_path(path, sizeof(path), dir, "push-state.json");
    qdc_buf state = {0};
    assert(qdc_read_file(path, 100000, &state, &e) == 0);
    assert(!strstr((char *)state.data, "SECRET") && !strstr((char *)state.data, "13800000000"));
    qdc_buf_free(&state);
    free_app(&app);
    init(&app, dir);
    assert(qdc_push_observe(&app, yyjson_doc_get_root(messages), yyjson_doc_get_root(calls), now,
                            &e) == 0 &&
           queued(&app) == 1);
    struct delivery d = {.result = QDC_PUSH_RETRY};
    qdc_push_test_deliver(&app, deliver, &d);
    qdc_push_send_next(&app, "", now);
    assert(d.calls == 0 && queued(&app) == 1);
    qdc_push_send_next(&app, "card-a", now);
    qdc_push_send_next(&app, "card-a", now);
    assert(d.calls == 1 && queued(&app) == 1);
    qdc_push_send_next(&app, "card-a", now + 5000);
    assert(d.calls == 2);
    memset(s.token, 'f', 64);
    assert(qdc_push_register(&app, &s, yyjson_doc_get_root(messages), &e) == 0);
    d.result = QDC_PUSH_OK;
    qdc_push_send_next(&app, "card-a", now + 15000);
    assert(d.calls == 3 && queued(&app) == 0 && accepted(&app) == 1 && !strcmp(d.token, s.token));
    assert(qdc_push_test_event(&app, "sms", now, &e) == 0);
    memset(&d, 0, sizeof(d));
    d.result = QDC_PUSH_RETRY;
    qdc_push_test_deliver(&app, deliver, &d);
    int offsets[] = {0, 5000, 15000, 35000, 75000};
    for (size_t i = 0; i < 5; i++)
        qdc_push_send_next(&app, "card-a", now + offsets[i]);
    assert(d.calls == 5 && queued(&app) == 0);
    yyjson_doc_free(calls);
    calls = json("{\"calls\":[{\"id\":\"cccccccccccccccccccccccc\",\"direction\":\"in\",\"state\":"
                 "\"incoming\"}]} ");
    assert(qdc_push_observe(&app, yyjson_doc_get_root(messages), yyjson_doc_get_root(calls), now,
                            &e) == 0 &&
           queued(&app) == 1);
    assert(qdc_push_observe(&app, yyjson_doc_get_root(messages), yyjson_doc_get_root(calls), now,
                            &e) == 0 &&
           queued(&app) == 1);
    qdc_push_send_next(&app, "card-a", now + 21000);
    assert(queued(&app) == 0 && d.calls == 5);
    yyjson_doc_free(calls);
    calls = json("{\"calls\":[]}");
    assert(qdc_push_observe(&app, yyjson_doc_get_root(messages), yyjson_doc_get_root(calls), now,
                            &e) == 0);
    assert(qdc_push_test_event(&app, "sms", now, &e) == 0);
    qdc_push_send_next(&app, "card-b", now);
    assert(queued(&app) == 0 && d.calls == 5);
    qdc_push_bind(&app, "card-a");
    free_app(&app);
    init(&app, dir);
    assert(queued(&app) == 0);
    assert(qdc_push_register(&app, &s, yyjson_doc_get_root(messages), &e) == 0);
    assert(qdc_push_test_event(&app, "sms", now, &e) == 0);
    s.enabled = false;
    assert(qdc_push_register(&app, &s, yyjson_doc_get_root(messages), &e) == 0 &&
           queued(&app) == 0);
    s.enabled = true;
    assert(qdc_push_register(&app, &s, yyjson_doc_get_root(messages), &e) == 0);
    assert(qdc_push_observe(&app, yyjson_doc_get_root(messages), yyjson_doc_get_root(calls), now,
                            &e) == 0 &&
           queued(&app) == 0);
    s.calls_enabled = false;
    assert(qdc_push_register(&app, &s, yyjson_doc_get_root(messages), &e) == 0);
    yyjson_doc_free(calls);
    calls = json("{\"calls\":[{\"id\":\"dddddddddddddddddddddddd\",\"direction\":\"in\",\"state\":"
                 "\"incoming\"}],\"error\":\"uncertain\"}");
    assert(qdc_push_observe(&app, yyjson_doc_get_root(messages), yyjson_doc_get_root(calls), now,
                            &e) == 0 &&
           queued(&app) == 0);
    s.calls_enabled = true;
    assert(qdc_push_register(&app, &s, yyjson_doc_get_root(messages), &e) == 0);
    assert(qdc_push_observe(&app, yyjson_doc_get_root(messages), yyjson_doc_get_root(calls), now,
                            &e) == 0 &&
           queued(&app) == 1);
    yyjson_doc_free(calls);
    calls = json("{\"calls\":[],\"error\":\"uncertain\"}");
    assert(qdc_push_observe(&app, yyjson_doc_get_root(messages), yyjson_doc_get_root(calls), now,
                            &e) == 0 &&
           queued(&app) == 1);
    yyjson_doc_free(calls);
    calls = json("{\"calls\":[]}");
    assert(qdc_push_observe(&app, yyjson_doc_get_root(messages), yyjson_doc_get_root(calls), now,
                            &e) == 0 &&
           queued(&app) == 0);
    free_app(&app);
    assert(qdc_atomic_write(path, "broken", 6, &e) == 0);
    init(&app, dir);
    assert(qdc_push_register(&app, &s, yyjson_doc_get_root(messages), &e) == -1);
    assert(qdc_read_file(path, 100, &state, &e) == 0 && state.len == 6 &&
           !memcmp(state.data, "broken", 6));
    qdc_buf_free(&state);
    free_app(&app);
    yyjson_doc_free(old);
    yyjson_doc_free(messages);
    yyjson_doc_free(calls);
    clear_dir(dir);
    puts("push: PASS (privacy, restart dedupe, stable retries/token rotation, TTL, 5 attempts, SIM "
         "revocation, corrupt preservation)");
    return 0;
}
#endif
