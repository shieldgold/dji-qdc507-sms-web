#include "qdc_auth.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pthread_mutex_t mu;
    char path[QDC_PATH_MAX];
    bool anchored;
    int64_t anchor;
    uint64_t received;
} app_clock;

static int local_year(int64_t ms) {
    int64_t seconds = ms / 1000;
    time_t value = (time_t)seconds;
    struct tm tm;
    if ((int64_t)value == seconds && localtime_r(&value, &tm))
        return tm.tm_year + 1900;
    /* The modem has a 32-bit time_t; UTC civil conversion remains valid past 2038. */
    char text[40];
    qdc_rfc3339(ms, text);
    if (strlen(text) < 4)
        return 0;
    int year = 0;
    for (size_t i = 0; i < 4; i++) {
        if (text[i] < '0' || text[i] > '9')
            return 0;
        year = year * 10 + text[i] - '0';
    }
    return year;
}
static bool valid_year(int year) {
    return year >= 2024 && year <= 2100;
}
static bool saved_time(yyjson_val *root, const char *key, int64_t *ms, int *year) {
    yyjson_val *v = yyjson_obj_get(root, key);
    if (!yyjson_is_str(v))
        return false;
    qdc_slice text = qdc_json_slice(v);
    if (qdc_parse_rfc3339(text, ms) || text.len < 4)
        return false;
    *year = 0;
    for (size_t i = 0; i < 4; i++) {
        if (text.ptr[i] < '0' || text.ptr[i] > '9')
            return false;
        *year = *year * 10 + text.ptr[i] - '0';
    }
    return true;
}
int qdc_clock_init(qdc_app *app, qdc_error *e) {
    app_clock *c = calloc(1, sizeof(*c));
    if (!c) {
        qdc_error_set(e, "内存不足，请稍后重试");
        return -1;
    }
    if (pthread_mutex_init(&c->mu, NULL)) {
        free(c);
        qdc_error_set(e, "时间服务无法初始化");
        return -1;
    }
    if (qdc_path(c->path, sizeof(c->path), app->config.data_dir, "clock.json")) {
        pthread_mutex_destroy(&c->mu);
        free(c);
        qdc_error_set(e, "数据目录路径过长");
        return -1;
    }
    qdc_buf b = {0};
    qdc_error ignored = {0};
    if (qdc_read_file(c->path, 4096, &b, &ignored) == 0) {
        yyjson_doc *doc = qdc_json_read(b.data, b.len, &ignored);
        if (doc) {
            yyjson_val *r = yyjson_doc_get_root(doc);
            int64_t utc, system;
            int year, system_year;
            if (yyjson_is_obj(r) && saved_time(r, "utc", &utc, &year) && valid_year(year)) {
                int64_t now = qdc_system_ms(), elapsed = 0;
                yyjson_val *system_value = yyjson_obj_get(r, "system");
                bool usable_system = saved_time(r, "system", &system, &system_year);
                /* Missing/null system equals Go time.Time's zero value. */
                bool valid_system = usable_system || !system_value || yyjson_is_null(system_value);
                if (valid_system) {
                    if (usable_system && system <= now &&
                        system >= now - 30LL * 24 * 60 * 60 * 1000)
                        elapsed = now - system;
                    c->anchor = utc + elapsed;
                    c->received = qdc_mono_ms();
                    c->anchored = true;
                }
            }
            yyjson_doc_free(doc);
        }
    }
    qdc_buf_free(&b);
    app->clock = c;
    return 0;
}
void qdc_clock_free(qdc_app *app) {
    app_clock *c = app ? app->clock : NULL;
    if (!c)
        return;
    pthread_mutex_destroy(&c->mu);
    free(c);
    app->clock = NULL;
}
int64_t qdc_now_ms(qdc_app *app) {
    app_clock *c = app ? app->clock : NULL;
    if (!c)
        return qdc_system_ms();
    pthread_mutex_lock(&c->mu);
    uint64_t now = qdc_mono_ms();
    int64_t value = c->anchored ? c->anchor + (int64_t)(now >= c->received ? now - c->received : 0)
                                : qdc_system_ms();
    pthread_mutex_unlock(&c->mu);
    return value;
}
static bool add_timestamp(yyjson_mut_doc *d, yyjson_mut_val *root, const char *key, int64_t ms) {
    char seconds[40], text[48];
    qdc_rfc3339(ms, seconds);
    size_t n = strlen(seconds);
    if (n != 20 || seconds[19] != 'Z')
        return false;
    int fraction = (int)(ms % 1000);
    if (fraction < 0)
        fraction += 1000;
    int size = snprintf(text, sizeof(text), "%.*s.%03dZ", 19, seconds, fraction);
    return size > 0 && (size_t)size < sizeof(text) &&
           qdc_json_add_string(d, root, key, text, (size_t)size);
}
int qdc_clock_sync(qdc_app *app, int64_t ms, qdc_error *e) {
    if (!valid_year(local_year(ms))) {
        qdc_error_set(e, "设备时间无效");
        return -1;
    }
    app_clock *c = app->clock;
    if (!c) {
        qdc_error_set(e, "通知时间未能保存，请重试");
        return -1;
    }
    pthread_mutex_lock(&c->mu);
    yyjson_mut_doc *d = qdc_json_object();
    bool ok = d != NULL;
    if (ok) {
        yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
        ok = add_timestamp(d, root, "utc", ms) && add_timestamp(d, root, "system", qdc_system_ms());
    }
    qdc_error write_error = {0};
    int rc = ok ? qdc_save_json(c->path, d, &write_error) : -1;
    if (d)
        yyjson_mut_doc_free(d);
    if (rc) {
        qdc_error_set(e, "通知时间未能保存，请重试");
        if (e)
            e->uncertain = write_error.uncertain;
        pthread_mutex_unlock(&c->mu);
        return -1;
    }
    c->anchor = ms;
    c->received = qdc_mono_ms();
    c->anchored = true;
    pthread_mutex_unlock(&c->mu);
    return 0;
}
int qdc_clock_route(qdc_app *app, qdc_request *r, qdc_response *s) {
    if (strcmp(r->method, "POST") || strcmp(r->path, "/api/time"))
        return 0;
    qdc_error e = {0};
    yyjson_val *root = qdc_request_json(r, &e);
    if (!root || (!yyjson_is_obj(root) && !yyjson_is_null(root)))
        return qdc_response_error(s, 400, "时间格式错误") ? -1 : 1;
    yyjson_val *v = yyjson_obj_get(root, "unix_ms");
    int64_t ms = 0;
    if (v && !yyjson_is_null(v)) {
        if (yyjson_is_sint(v))
            ms = yyjson_get_sint(v);
        else if (yyjson_is_uint(v) && yyjson_get_uint(v) <= INT64_MAX)
            ms = (int64_t)yyjson_get_uint(v);
        else
            return qdc_response_error(s, 400, "时间格式错误") ? -1 : 1;
    }
    if (qdc_clock_sync(app, ms, &e))
        return qdc_response_error(s, 400, e.message) ? -1 : 1;
    return qdc_response_ok(s, 200) ? -1 : 1;
}
