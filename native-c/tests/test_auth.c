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
static const char CARD[] = "iccid:8900000000000000001";
static qdc_sim_identity current = {.key = "iccid:8900000000000000001",
                                   .iccid = "8900000000000000001"};
static bool sim_missing;
int qdc_domain_identity(qdc_app *a, bool fresh, qdc_sim_identity *s, qdc_error *e) {
    if (sim_missing) {
        qdc_error_set(e, "未能实时识别 SIM 卡，未使用保存的凭据");
        return -1;
    }
    *s = current;
    return fresh ? qdc_auth_bind(a, s->key, e) : 0;
}
yyjson_doc *qdc_pin_snapshot(qdc_app *a) {
    (void)a;
    const char *v = "{\"state\":\"READY\",\"blocked\":false}";
    return yyjson_read(v, strlen(v), 0);
}
static void app_open(qdc_app *a, const char *dir) {
    memset(a, 0, sizeof(*a));
    a->config.data_dir = dir;
    qdc_error e = {0};
    pthread_mutex_init(&a->action_mu, NULL);
    pthread_mutex_init(&a->op_mu, NULL);
    CHECK(qdc_auth_init(a, &e) == 0);
    CHECK(qdc_clock_init(a, &e) == 0);
}
static void app_close(qdc_app *a) {
    qdc_auth_free(a);
    qdc_clock_free(a);
    pthread_mutex_destroy(&a->action_mu);
    pthread_mutex_destroy(&a->op_mu);
}
static qdc_slice lit(const char *s) {
    return (qdc_slice){s, strlen(s)};
}
static yyjson_doc *read_auth(const char *dir) {
    char p[4096];
    qdc_buf b = {0};
    qdc_error e = {0};
    CHECK(!qdc_path(p, sizeof(p), dir, "auth.json"));
    CHECK(qdc_read_file(p, 4194304, &b, &e) == 0);
    yyjson_doc *d = qdc_json_read(b.data, b.len, &e);
    qdc_buf_free(&b);
    CHECK(d);
    return d;
}
static void fixture_go(const char *base) {
    char dir[4096], p[4096], tok[65] = {0};
    qdc_error e = {0};
    qdc_buf b = {0};
    CHECK(!qdc_path(dir, sizeof(dir), base, "go-generated"));
    qdc_app a;
    app_open(&a, dir);
    static const char pw[] = "中\0文ab";
    CHECK(qdc_auth_login(&a, CARD, (qdc_slice){pw, sizeof(pw) - 1}, false, tok, &e) == 0);
    CHECK(qdc_path(p, sizeof(p), dir, "go-token.txt") == 0);
    CHECK(qdc_read_file(p, 65, &b, &e) == 0);
    CHECK(b.len == 64);
    memcpy(tok, b.data, 64);
    qdc_buf_free(&b);
    CHECK(qdc_auth_token_valid(&a, tok, CARD));
    app_close(&a);
}
static void fixture_c(const char *base) {
    char dir[4096], p[4096], tok[65] = {0};
    qdc_error e = {0};
    CHECK(!qdc_path(dir, sizeof(dir), base, "c-generated"));
    CHECK(qdc_mkdir_private(dir, &e) == 0);
    qdc_app a;
    app_open(&a, dir);
    static const char pw[] = "汉\0字ab";
    CHECK(!qdc_auth_bind(&a, CARD, &e));
    CHECK(!qdc_auth_setup(&a, &current, (qdc_slice){pw, sizeof(pw) - 1}, false, tok, &e));
    CHECK(qdc_path(p, sizeof(p), dir, "c-token.txt") == 0);
    CHECK(!qdc_atomic_write(p, tok, 64, &e));
    app_close(&a);
}
static qdc_response route(qdc_app *a, const char *method, const char *path, const char *cookie,
                          const char *body) {
    qdc_request r = {
        .method = method, .path = path, .cookie = cookie, .body = {body, body ? strlen(body) : 0}};
    qdc_response s = {0};
    CHECK(qdc_auth_route(a, &r, &s) == 1);
    if (r.json)
        yyjson_doc_free(r.json);
    return s;
}
static void test_core(const char *base) {
    char dir[4096], p[4096], tmp[4096], tok[65], other[65], ticket[65], child[65];
    qdc_error e = {0};
    CHECK(!qdc_path(dir, sizeof(dir), base, "core"));
    CHECK(!qdc_mkdir_private(dir, &e));
    qdc_app a;
    app_open(&a, dir);
    CHECK(!qdc_auth_bind(&a, CARD, &e));
    CHECK(!qdc_auth_setup(&a, &current, lit("abcd"), false, tok, &e));
    CHECK(qdc_auth_registered(&a, CARD));
    CHECK(qdc_auth_token_valid(&a, tok, CARD));
    yyjson_doc *d = read_auth(dir);
    yyjson_val *t = yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(d), "tokens"), tok);
    CHECK(t && !yyjson_get_bool(yyjson_obj_get(t, "remember")));
    int64_t ex = yyjson_get_sint(yyjson_obj_get(t, "expires"));
    CHECK(ex - qdc_system_ms() / 1000 >= 43198 && ex - qdc_system_ms() / 1000 <= 43200);
    yyjson_doc_free(d);
    app_close(&a);
    app_open(&a, dir);
    CHECK(qdc_auth_token_valid(&a, tok, CARD));
    CHECK(qdc_auth_login(&a, CARD, lit("wrong"), false, other, &e) < 0);
    CHECK(strstr(e.message, "还可尝试 4 次"));
    CHECK(!qdc_auth_login(&a, CARD, lit("abcd"), true, other, &e));
    CHECK(!qdc_auth_ticket(&a, tok, CARD, ticket, &e));
    char old_ticket[65];
    memcpy(old_ticket, ticket, 65);
    CHECK(!qdc_auth_ticket(&a, tok, CARD, ticket, &e));
    CHECK(qdc_auth_redeem(&a, old_ticket, CARD, child, &e) < 0);
    CHECK(!qdc_auth_redeem(&a, ticket, CARD, child, &e));
    CHECK(strcmp(child, tok));
    CHECK(qdc_auth_redeem(&a, ticket, CARD, child, &e) < 0);
    CHECK(!qdc_path(p, sizeof(p), dir, "auth.json"));
    CHECK(!qdc_path(tmp, sizeof(tmp), dir, "auth.json.tmp"));
    CHECK(!mkdir(tmp, 0700));
    CHECK(qdc_auth_logout(&a, tok, &e) < 0);
    CHECK(qdc_auth_token_valid(&a, tok, CARD));
    CHECK(qdc_auth_bind(&a, "iccid:8900000000000000002", &e) < 0);
    CHECK(qdc_auth_token_valid(&a, tok, CARD));
    CHECK(qdc_auth_change(&a, CARD, lit("abcd"), lit("new-password"), &e) < 0);
    CHECK(!rmdir(tmp));
    CHECK(!qdc_auth_login(&a, CARD, lit("abcd"), false, other, &e));
    CHECK(!qdc_auth_bind(&a, "iccid:8900000000000000002", &e));
    CHECK(!qdc_auth_bind(&a, CARD, &e));
    CHECK(!qdc_auth_token_valid(&a, tok, CARD));
    CHECK(!qdc_auth_login(&a, CARD, lit("abcd"), false, tok, &e));
    CHECK(!qdc_auth_change(&a, CARD, lit("abcd"), lit("new-password"), &e));
    CHECK(!qdc_auth_token_valid(&a, tok, CARD));
    CHECK(!qdc_auth_login(&a, CARD, lit("new-password"), false, tok, &e));
    for (int i = 0; i < 5; i++)
        CHECK(qdc_auth_login(&a, CARD, lit("bad"), false, other, &e) < 0);
    CHECK(strstr(e.message, "已锁定 60 秒"));
    CHECK(qdc_auth_login(&a, CARD, lit("new-password"), false, other, &e) < 0);
    app_close(&a);
    CHECK(!qdc_path(dir, sizeof(dir), base, "corrupt"));
    CHECK(!qdc_mkdir_private(dir, &e));
    CHECK(!qdc_path(p, sizeof(p), dir, "auth.json"));
    CHECK(!qdc_atomic_write(p, "{broken", 7, &e));
    app_open(&a, dir);
    CHECK(qdc_auth_setup(&a, &current, lit("abcd"), true, tok, &e) < 0);
    CHECK(strstr(e.message, "已保留原文件"));
    qdc_buf b = {0};
    CHECK(!qdc_read_file(p, 32, &b, &e));
    CHECK(b.len == 7 && !memcmp(b.data, "{broken", 7));
    qdc_buf_free(&b);
    app_close(&a);
}
static void test_http(const char *base) {
    char dir[4096], cookie[128];
    qdc_error e = {0};
    CHECK(!qdc_path(dir, sizeof(dir), base, "http"));
    CHECK(!qdc_mkdir_private(dir, &e));
    qdc_app a;
    app_open(&a, dir);
    qdc_response r =
        route(&a, "POST", "/api/auth/setup", "",
              "{\"password\":\"ab\\u0000中\",\"confirm\":\"ab\\u0000中\",\"remember\":true}");
    CHECK(r.status == 200);
    bool seen = false;
    for (size_t i = 0; i < r.header_count; i++)
        if (!strcmp(r.headers[i].name, "Set-Cookie")) {
            CHECK(strstr(r.headers[i].value, "HttpOnly; SameSite=Lax; Max-Age=31536000"));
            char *semi = strchr(r.headers[i].value, ';');
            CHECK(semi);
            size_t n = (size_t)(semi - r.headers[i].value);
            CHECK(n < sizeof(cookie));
            memcpy(cookie, r.headers[i].value, n);
            cookie[n] = 0;
            seen = true;
        }
    CHECK(seen);
    qdc_response_free(&r);
    r = route(&a, "GET", "/api/auth/status", cookie, NULL);
    CHECK(r.status == 200);
    CHECK(strstr((char *)r.body.data, "\"authenticated\":true"));
    qdc_response_free(&r);
    r = route(&a, "POST", "/api/auth/login", "",
              "{\"password\":\"unused\",\"expected_iccid\":\"wrong\"}");
    CHECK(r.status == 409);
    CHECK(!r.header_count);
    qdc_response_free(&r);
    sim_missing = true;
    r = route(&a, "GET", "/api/auth/status", cookie, NULL);
    CHECK(r.status == 503);
    qdc_response_free(&r);
    sim_missing = false;
    r = route(&a, "POST", "/api/auth/change", cookie,
              "{\"old\":\"ab\\u0000中\",\"password\":\"新密码四\",\"confirm\":\"新密码四\"}");
    CHECK(r.status == 200);
    qdc_response_free(&r);
    r = route(&a, "POST", "/api/auth/logout", cookie, "{}");
    CHECK(r.status == 200);
    CHECK(r.header_count && strstr(r.headers[0].value, "Max-Age=0"));
    qdc_response_free(&r);
    app_close(&a);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    fixture_go(argv[1]);
    test_core(argv[1]);
    test_http(argv[1]);
    fixture_c(argv[1]);
    puts("auth: PASS (Go fixtures, NUL/UTF8, persistence, rollback, tickets, HTTP)");
    return 0;
}
