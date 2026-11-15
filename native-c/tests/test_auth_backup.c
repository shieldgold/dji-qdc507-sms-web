#include "qdc_auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static bool busy, calls, traffic_fail, store_fail;
static int restores, traffic_saves, previous_checks, launches;
static bool previous = true, launch_ok = true;
yyjson_doc *qdc_messages(qdc_app *a) {
    (void)a;
    const char *v = "[{\"key\":\"keep\",\"body\":\"A\\u0000B\",\"sender\":\"10010\",\"direction\":"
                    "\"in\",\"index\":3,\"time\":\"\",\"status\":\"\"}]";
    return yyjson_read(v, strlen(v), 0);
}
bool qdc_outbox_busy(qdc_app *a) {
    (void)a;
    return busy;
}
bool qdc_calls_busy(qdc_app *a) {
    (void)a;
    return calls;
}
int qdc_traffic_save(qdc_app *a, qdc_error *e) {
    (void)a;
    traffic_saves++;
    if (traffic_fail) {
        qdc_error_set(e, "test traffic error");
        return -1;
    }
    return 0;
}
int qdc_store_restore(qdc_app *a, yyjson_val *array, int *added, qdc_error *e) {
    (void)a;
    CHECK(yyjson_is_arr(array));
    restores++;
    if (store_fail) {
        qdc_error_set(e, "恢复未保存，已撤销本次导入");
        return -1;
    }
    *added = (int)yyjson_arr_size(array);
    return 0;
}
static int check_previous(const char *path, void *ctx) {
    (void)ctx;
    CHECK(!strcmp(path, "/data/smsweb/previous/smsweb"));
    previous_checks++;
    return previous ? 1 : 0;
}
static int launch(const char *path, void *ctx) {
    (void)ctx;
    CHECK(!strcmp(path, "/data/smsweb/restore-version.sh"));
    launches++;
    return launch_ok ? 0 : -1;
}
static void request(qdc_app *a, const char *method, const char *path, const char *body, int status,
                    const char *error) {
    qdc_request r = {.method = method, .path = path, .body = {body, body ? strlen(body) : 0}};
    qdc_response s = {0};
    CHECK(qdc_backup_route(a, &r, &s) == 1);
    CHECK(s.status == status);
    if (error)
        CHECK(strstr((char *)s.body.data, error));
    if (!strcmp(method, "GET") && status == 200) {
        CHECK(s.header_count && strstr(s.headers[0].value, "module-sms-backup.json"));
        qdc_error e = {0};
        yyjson_doc *d = qdc_json_read(s.body.data, s.body.len, &e);
        CHECK(d);
        yyjson_val *root = yyjson_doc_get_root(d);
        CHECK(yyjson_get_uint(yyjson_obj_get(root, "version")) == 1);
        qdc_slice value = qdc_json_slice(
            yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(root, "messages"), 0), "body"));
        CHECK(value.len == 3 && !memcmp(value.ptr, "A\0B", 3));
        yyjson_doc_free(d);
    }
    if (r.json)
        yyjson_doc_free(r.json);
    qdc_response_free(&s);
}
static void rollback(qdc_app *a, const qdc_rollback_ops *ops, int status, const char *error) {
    qdc_response s = {0};
    CHECK(qdc_rollback_with_ops(a, &s, ops) == 1 && s.status == status);
    if (error)
        CHECK(strstr((char *)s.body.data, error));
    qdc_response_free(&s);
}
int main(void) {
    qdc_app a = {.config = {.data_dir = "/tmp/qdc-backup-mock"}};
    pthread_mutex_init(&a.action_mu, NULL);
    pthread_mutex_init(&a.op_mu, NULL);
    request(&a, "GET", "/api/export", NULL, 200, NULL);
    request(&a, "POST", "/api/restore", "{\"version\":999,\"messages\":[]}", 400,
            "不支持的短信备份文件");
    request(&a, "POST", "/api/restore", "{\"version\":1.0,\"messages\":[]}", 400,
            "不支持的短信备份文件");
    request(&a, "POST", "/api/restore", "{\"version\":1,\"exported_at\":7,\"messages\":[]}", 400,
            "不支持的短信备份文件");
    request(&a, "POST", "/api/restore", "{\"version\":1,\"messages\":{}}", 400,
            "不支持的短信备份文件");
    CHECK(restores == 0);
    busy = true;
    request(&a, "POST", "/api/restore", "bad-json", 409, "短信正在提交");
    busy = false;
    request(&a, "POST", "/api/restore", "{\"version\":1}", 200, "已合并 0 条短信");
    request(&a, "POST", "/api/restore", "{\"version\":1,\"messages\":null}", 200,
            "已合并 0 条短信");
    request(&a, "POST", "/api/restore",
            "{\"version\":1,\"messages\":[{\"key\":\"import\",\"sender\":\"10010\",\"body\":"
            "\"record\",\"direction\":\"in\"}]}",
            200, "已合并 1 条短信");
    store_fail = true;
    request(&a, "POST", "/api/restore", "{\"version\":1,\"messages\":[]}", 400, "已撤销本次导入");
    store_fail = false;
    qdc_buf large = {0};
    CHECK(!qdc_buf_puts(&large, "{\"version\":1,\"messages\":["));
    for (int i = 0; i < 301; i++)
        CHECK(!qdc_buf_puts(&large, i ? ",{}" : "{}"));
    CHECK(!qdc_buf_puts(&large, "]}"));
    int before = restores;
    request(&a, "POST", "/api/restore", (char *)large.data, 400, "一次最多恢复 300 条短信");
    CHECK(restores == before);
    qdc_buf_free(&large);
    const qdc_rollback_ops ops = {check_previous, launch, NULL};
    rollback(&a, &ops, 400, "此环境不支持恢复模块版本");
    CHECK(!previous_checks && !launches && !traffic_saves);
    a.config.data_dir = "/data/smsweb";
    calls = true;
    rollback(&a, &ops, 409, "正在处理短信、通话或维护");
    calls = false;
    busy = true;
    rollback(&a, &ops, 409, "正在处理短信、通话或维护");
    busy = false;
    previous = false;
    rollback(&a, &ops, 400, "没有可恢复的旧版本");
    CHECK(!traffic_saves);
    previous = true;
    traffic_fail = true;
    rollback(&a, &ops, 500, "流量统计未能保存");
    CHECK(!launches && !a.maintenance);
    traffic_fail = false;
    launch_ok = false;
    rollback(&a, &ops, 500, "无法启动恢复程序");
    CHECK(!a.maintenance);
    launch_ok = true;
    rollback(&a, &ops, 202, NULL);
    CHECK(a.maintenance && launches == 2);
    rollback(&a, &ops, 409, "正在处理短信、通话或维护");
    CHECK(launches == 2);
    pthread_mutex_destroy(&a.action_mu);
    pthread_mutex_destroy(&a.op_mu);
    puts("backup/rollback: PASS (schema, export NUL, busy, safe mock maintenance)");
    return 0;
}
