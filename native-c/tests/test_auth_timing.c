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
static int64_t wall_ms = 1794704523123LL;
static uint64_t mono_ms = 1000;
static bool uncertain_write;
int64_t qdc_system_ms(void) {
    return wall_ms;
}
uint64_t qdc_mono_ms(void) {
    return mono_ms;
}
int qdc_real_save_json(const char *, yyjson_mut_doc *, qdc_error *);
int qdc_save_json(const char *path, yyjson_mut_doc *doc, qdc_error *e) {
    int rc = qdc_real_save_json(path, doc, e);
    if (!rc && uncertain_write) {
        qdc_error_set(e, "injected directory fsync failure");
        e->uncertain = true;
        return -1;
    }
    return rc;
}
static const char KEY[] = "iccid:8900000000000000001";
static const qdc_sim_identity SIM = {.key = "iccid:8900000000000000001",
                                     .iccid = "8900000000000000001"};
int qdc_domain_identity(qdc_app *a, bool fresh, qdc_sim_identity *s, qdc_error *e) {
    *s = SIM;
    return fresh ? qdc_auth_bind(a, s->key, e) : 0;
}
yyjson_doc *qdc_pin_snapshot(qdc_app *a) {
    (void)a;
    return yyjson_read("{}", 2, 0);
}
int qdc_clock_route(qdc_app *a, qdc_request *r, qdc_response *s) {
    (void)a;
    (void)r;
    (void)s;
    return 0;
}
static qdc_slice str(const char *s) {
    return (qdc_slice){s, strlen(s)};
}
static void open_app(qdc_app *a, const char *dir) {
    memset(a, 0, sizeof(*a));
    a->config.data_dir = dir;
    qdc_error e = {0};
    CHECK(!qdc_mkdir_private(dir, &e));
    CHECK(!qdc_auth_init(a, &e));
}
static void test_expiry(const char *base) {
    char dir[4096], token[65], remembered[65];
    qdc_error e = {0};
    CHECK(!qdc_path(dir, sizeof(dir), base, "expiry"));
    qdc_app a;
    open_app(&a, dir);
    CHECK(!qdc_auth_setup(&a, &SIM, str("abcd"), false, token, &e));
    CHECK(!qdc_auth_login(&a, KEY, str("abcd"), true, remembered, &e));
    int64_t created = wall_ms / 1000;
    mono_ms += 999999999;
    CHECK(qdc_auth_token_valid(&a, token, KEY));
    wall_ms = (created + 43200) * 1000;
    CHECK(qdc_auth_token_valid(&a, token, KEY));
    wall_ms += 1000;
    CHECK(!qdc_auth_token_valid(&a, token, KEY));
    CHECK(qdc_auth_token_valid(&a, remembered, KEY));
    wall_ms = (created + 31536000) * 1000;
    CHECK(qdc_auth_token_valid(&a, remembered, KEY));
    wall_ms += 1000;
    CHECK(!qdc_auth_token_valid(&a, remembered, KEY));
    qdc_auth_free(&a);
}
static void test_lockout(const char *base) {
    char dir[4096], token[65];
    qdc_error e = {0};
    CHECK(!qdc_path(dir, sizeof(dir), base, "lockout"));
    qdc_app a;
    open_app(&a, dir);
    CHECK(!qdc_auth_setup(&a, &SIM, str("abcd"), false, token, &e));
    for (int i = 0; i < 5; i++)
        CHECK(qdc_auth_login(&a, KEY, str("bad"), false, token, &e) < 0);
    CHECK(strstr(e.message, "已锁定 60 秒"));
    wall_ms += 100LL * 86400000;
    CHECK(qdc_auth_login(&a, KEY, str("abcd"), false, token, &e) < 0);
    CHECK(strstr(e.message, "请 61 秒后再试"));
    mono_ms += 59999;
    CHECK(qdc_auth_login(&a, KEY, str("abcd"), false, token, &e) < 0);
    mono_ms++;
    CHECK(!qdc_auth_login(&a, KEY, str("abcd"), false, token, &e));
    qdc_auth_free(&a);
}
typedef struct {
    qdc_app *app;
    const char *ticket;
    atomic_int *successes;
} redeem_work;
static void *redeem_once(void *opaque) {
    redeem_work *w = opaque;
    qdc_error e = {0};
    char token[65];
    if (!qdc_auth_redeem(w->app, w->ticket, KEY, token, &e))
        atomic_fetch_add(w->successes, 1);
    return NULL;
}
static void test_tickets(const char *base) {
    char dir[4096], token[65], ticket[65], child[65], tmp[4096];
    qdc_error e = {0};
    CHECK(!qdc_path(dir, sizeof(dir), base, "tickets"));
    qdc_app a;
    open_app(&a, dir);
    CHECK(!qdc_auth_bind(&a, KEY, &e));
    CHECK(!qdc_auth_setup(&a, &SIM, str("abcd"), true, token, &e));
    CHECK(!qdc_auth_ticket(&a, token, KEY, ticket, &e));
    mono_ms += 30000;
    CHECK(!qdc_auth_redeem(&a, ticket, KEY, child, &e));
    CHECK(!qdc_auth_ticket(&a, token, KEY, ticket, &e));
    mono_ms += 30001;
    CHECK(qdc_auth_redeem(&a, ticket, KEY, child, &e) < 0);
    CHECK(!qdc_auth_ticket(&a, token, KEY, ticket, &e));
    pthread_t threads[8];
    atomic_int successes = 0;
    redeem_work w = {&a, ticket, &successes};
    for (size_t i = 0; i < 8; i++)
        CHECK(!pthread_create(&threads[i], NULL, redeem_once, &w));
    for (size_t i = 0; i < 8; i++)
        CHECK(!pthread_join(threads[i], NULL));
    CHECK(atomic_load(&successes) == 1);
    CHECK(!qdc_auth_ticket(&a, token, KEY, ticket, &e));
    CHECK(!qdc_path(tmp, sizeof(tmp), dir, "auth.json.tmp"));
    CHECK(!mkdir(tmp, 0700));
    CHECK(qdc_auth_redeem(&a, ticket, KEY, child, &e) < 0);
    CHECK(!child[0]);
    CHECK(!rmdir(tmp));
    CHECK(qdc_auth_redeem(&a, ticket, KEY, child, &e) < 0);
    CHECK(qdc_auth_token_valid(&a, token, KEY));
    char parents[9][65], tickets[9][65];
    for (size_t i = 0; i < 9; i++) {
        CHECK(!qdc_auth_issue(&a, KEY, true, parents[i], &e));
        int rc = qdc_auth_ticket(&a, parents[i], KEY, tickets[i], &e);
        if (i < 8)
            CHECK(!rc);
        else {
            CHECK(rc < 0);
            CHECK(strstr(e.message, "免密请求过多"));
        }
    }
    CHECK(!qdc_auth_ticket(&a, parents[0], KEY, ticket, &e));
    CHECK(qdc_auth_redeem(&a, tickets[0], KEY, child, &e) < 0);
    mono_ms += 30001;
    CHECK(!qdc_auth_ticket(&a, parents[8], KEY, ticket, &e));
    CHECK(!qdc_auth_logout(&a, parents[8], &e));
    CHECK(qdc_auth_redeem(&a, ticket, KEY, child, &e) < 0);
    qdc_auth_free(&a);
}
static void test_limits_and_uncertainty(const char *base) {
    char dir[4096], token[65], out[65], path[4096];
    qdc_error e = {0};
    CHECK(!qdc_path(dir, sizeof(dir), base, "uncertain"));
    qdc_app a;
    open_app(&a, dir);
    CHECK(qdc_auth_setup(&a, &SIM, str("三个字"), true, token, &e) < 0);
    CHECK(strstr(e.message, "至少 4 位"));
    CHECK(qdc_auth_setup(&a, &SIM, str("这这这这这这这这这这这这这这这这这这这这这这"), true, token,
                         &e) < 0);
    CHECK(strstr(e.message, "密码太长"));
    CHECK(!qdc_auth_setup(&a, &SIM, str("😀😃😄😁"), true, token, &e));
    uncertain_write = true;
    CHECK(qdc_auth_change(&a, KEY, str("😀😃😄😁"), str("new-password"), &e) < 0);
    CHECK(e.uncertain);
    CHECK(!qdc_auth_token_valid(&a, token, KEY));
    CHECK(qdc_auth_issue(&a, KEY, true, out, &e) < 0);
    uncertain_write = false;
    qdc_auth_free(&a);
    CHECK(!qdc_auth_init(&a, &e));
    CHECK(!qdc_auth_login(&a, KEY, str("new-password"), false, out, &e));
    qdc_auth_free(&a);
    CHECK(!qdc_path(dir, sizeof(dir), base, "semantic-corrupt"));
    CHECK(!qdc_mkdir_private(dir, &e));
    CHECK(!qdc_path(path, sizeof(path), dir, "auth.json"));
    const char *bad = "{\"accounts\":{},\"tokens\":{\"bad\":null}}";
    CHECK(!qdc_atomic_write(path, bad, strlen(bad), &e));
    open_app(&a, dir);
    CHECK(qdc_auth_setup(&a, &SIM, str("abcd"), true, token, &e) < 0);
    CHECK(strstr(e.message, "已保留原文件"));
    qdc_buf b = {0};
    CHECK(!qdc_read_file(path, 1024, &b, &e));
    CHECK(b.len == strlen(bad) && !memcmp(b.data, bad, b.len));
    qdc_buf_free(&b);
    qdc_auth_free(&a);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    test_expiry(argv[1]);
    test_lockout(argv[1]);
    test_tickets(argv[1]);
    test_limits_and_uncertainty(argv[1]);
    puts("auth boundaries: PASS (expiry equality, monotonic lockout/tickets, concurrency, "
         "uncertain commit, Unicode limits)");
    return 0;
}
