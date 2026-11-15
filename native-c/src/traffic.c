#include "qdc_monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool number(yyjson_val *v, uint64_t *out) {
    if (!v) {
        *out = 0;
        return true;
    }
    if (!yyjson_is_uint(v))
        return false;
    *out = yyjson_get_uint(v);
    return true;
}
static bool count_read(yyjson_val *v, qdc_count *out) {
    if (!v || yyjson_is_null(v)) {
        *out = (qdc_count){0};
        return true;
    }
    return yyjson_is_obj(v) && number(yyjson_obj_get(v, "upload"), &out->upload) &&
           number(yyjson_obj_get(v, "download"), &out->download);
}
static yyjson_mut_val *count_json(yyjson_mut_doc *d, qdc_count count) {
    yyjson_mut_val *v = yyjson_mut_obj(d);
    if (!v || !yyjson_mut_obj_add_uint(d, v, "upload", count.upload) ||
        !yyjson_mut_obj_add_uint(d, v, "download", count.download))
        return NULL;
    return v;
}
static bool periods_read(yyjson_val *v, qdc_period **out, size_t *count) {
    if (!v || yyjson_is_null(v))
        return true;
    if (!yyjson_is_obj(v) || yyjson_obj_size(v) > 4096)
        return false;
    size_t n = yyjson_obj_size(v);
    qdc_period *p = calloc(n ? n : 1, sizeof(*p));
    if (!p)
        return false;
    size_t i, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(v, i, max, key, value) {
        if (qdc_json_cstr(key, p[i].key, sizeof(p[i].key)) || !count_read(value, &p[i].count)) {
            free(p);
            return false;
        }
    }
    *out = p;
    *count = n;
    return true;
}
qdc_traffic *qdc_traffic_load(const char *dir, qdc_error *e) {
    qdc_traffic *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    pthread_mutex_init(&t->mu, NULL);
    if (qdc_path(t->path, sizeof(t->path), dir, "traffic.json")) {
        qdc_traffic_free(t);
        return NULL;
    }
    qdc_buf b = {0};
    int rc = qdc_read_file(t->path, 4 * 1024 * 1024, &b, e);
    if (rc == 1)
        return t;
    yyjson_doc *d = rc == 0 ? qdc_json_read(b.data, b.len, e) : NULL;
    qdc_buf_free(&b);
    yyjson_val *root = d ? yyjson_doc_get_root(d) : NULL;
    bool ok = yyjson_is_obj(root);
    if (ok) {
        yyjson_val *v = yyjson_obj_get(root, "boot_id");
        if (v && qdc_json_cstr(v, t->boot, sizeof(t->boot)))
            ok = false;
        v = yyjson_obj_get(root, "since");
        if (v && qdc_json_cstr(v, t->since, sizeof(t->since)))
            ok = false;
        v = yyjson_obj_get(root, "initialized");
        if (v && !yyjson_is_bool(v))
            ok = false;
        t->initialized = yyjson_get_bool(v);
        ok = ok && number(yyjson_obj_get(root, "last_rx"), &t->last_rx) &&
             number(yyjson_obj_get(root, "last_tx"), &t->last_tx) &&
             count_read(yyjson_obj_get(root, "total"), &t->total) &&
             count_read(yyjson_obj_get(root, "session"), &t->session) &&
             periods_read(yyjson_obj_get(root, "days"), &t->days, &t->day_count) &&
             periods_read(yyjson_obj_get(root, "months"), &t->months, &t->month_count);
    }
    if (d)
        yyjson_doc_free(d);
    if (!ok) {
        t->load_failed = true;
        qdc_error_set(e, "流量统计无法读取，原文件已保留");
    }
    return t;
}
void qdc_traffic_free(qdc_traffic *t) {
    if (t) {
        free(t->days);
        free(t->months);
        pthread_mutex_destroy(&t->mu);
        free(t);
    }
}
static qdc_period *period(qdc_period **items, size_t *count, const char *key) {
    for (size_t i = 0; i < *count; i++)
        if (!strcmp((*items)[i].key, key))
            return &(*items)[i];
    if (*count >= 4096)
        return NULL;
    qdc_period *p = realloc(*items, (*count + 1) * sizeof(*p));
    if (!p)
        return NULL;
    *items = p;
    qdc_period *last = &p[(*count)++];
    memset(last, 0, sizeof(*last));
    strcpy(last->key, key);
    return last;
}
static void add(qdc_count *c, uint64_t up, uint64_t down) {
    c->upload += up;
    c->download += down;
}
static bool calendar(int64_t ms, struct tm *utc, struct tm *local, char day[16], char month[16]) {
    time_t now = (time_t)(ms / 1000), china = now + 8 * 3600;
    if (!gmtime_r(&now, utc) || !gmtime_r(&china, local))
        return false;
    strftime(day, 16, "%Y-%m-%d", local);
    strftime(month, 16, "%Y-%m", local);
    return true;
}
static void prune(qdc_period *p, size_t *n, const char *cut) {
    size_t used = 0;
    for (size_t i = 0; i < *n; i++)
        if (strcmp(p[i].key, cut) >= 0)
            p[used++] = p[i];
    *n = used;
}
int qdc_traffic_sample(qdc_traffic *t, const char *boot, uint64_t rx, uint64_t tx, int64_t now) {
    if (!t || strlen(boot) >= sizeof(t->boot))
        return -1;
    pthread_mutex_lock(&t->mu);
    if (t->load_failed) {
        pthread_mutex_unlock(&t->mu);
        return -1;
    }
    struct tm utc = {0}, local = {0};
    char day[16], month[16];
    bool valid = calendar(now, &utc, &local, day, month) && utc.tm_year >= 124;
    qdc_period *d = NULL, *m = NULL;
    if (valid) {
        d = period(&t->days, &t->day_count, day);
        m = period(&t->months, &t->month_count, month);
        if (!d || !m) {
            pthread_mutex_unlock(&t->mu);
            return -1;
        }
    }
    uint64_t up = 0, down = 0;
    if (t->initialized) {
        if (strcmp(t->boot, boot)) {
            up = rx;
            down = tx;
            t->session = (qdc_count){0};
        } else {
            up = rx < t->last_rx ? rx : rx - t->last_rx;
            down = tx < t->last_tx ? tx : tx - t->last_tx;
        }
    } else
        qdc_rfc3339(now, t->since);
    t->initialized = true;
    strcpy(t->boot, boot);
    t->last_rx = rx;
    t->last_tx = tx;
    add(&t->total, up, down);
    add(&t->session, up, down);
    if (valid) {
        add(&d->count, up, down);
        add(&m->count, up, down);
        time_t cutoff = (time_t)(now / 1000 + 8 * 3600 - 62 * 86400);
        struct tm cut;
        if (gmtime_r(&cutoff, &cut)) {
            char key[16];
            strftime(key, sizeof(key), "%Y-%m-%d", &cut);
            prune(t->days, &t->day_count, key);
        }
        int year = local.tm_year + 1900 - 2, mon = local.tm_mon + 1;
        if (mon == 2 && local.tm_mday == 29 && (year % 4 || (!(year % 100) && year % 400)))
            mon = 3;
        char key[16];
        snprintf(key, sizeof(key), "%04d-%02d", year, mon);
        prune(t->months, &t->month_count, key);
    }
    t->rate = (qdc_count){0};
    uint64_t elapsed = now > t->last_ms ? (uint64_t)now - (uint64_t)t->last_ms : 0;
    if (t->last_ms && elapsed > 0 && elapsed < 60000) {
        long double ur = (long double)up * 1000 / elapsed, dr = (long double)down * 1000 / elapsed;
        t->rate.upload = ur >= 0x1p64L ? UINT64_MAX : (uint64_t)ur;
        t->rate.download = dr >= 0x1p64L ? UINT64_MAX : (uint64_t)dr;
    }
    t->last_ms = now;
    pthread_mutex_unlock(&t->mu);
    return 0;
}
static qdc_count find(qdc_period *p, size_t n, const char *key) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(p[i].key, key))
            return p[i].count;
    return (qdc_count){0};
}
yyjson_mut_doc *qdc_traffic_view(qdc_traffic *t, int64_t now) {
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return NULL;
    yyjson_mut_val *v = yyjson_mut_doc_get_root(d);
    struct tm utc = {0}, local = {0};
    char day[16] = "", month[16] = "";
    calendar(now, &utc, &local, day, month);
    pthread_mutex_lock(&t->mu);
    bool ok =
        yyjson_mut_obj_add_strcpy(d, v, "source", "USB 链路（含管理页面流量，不等同运营商账单）") &&
        yyjson_mut_obj_add_strcpy(d, v, "since", t->since) &&
        yyjson_mut_obj_add_val(d, v, "total", count_json(d, t->total)) &&
        yyjson_mut_obj_add_val(d, v, "session", count_json(d, t->session)) &&
        yyjson_mut_obj_add_val(d, v, "rate", count_json(d, t->rate)) &&
        yyjson_mut_obj_add_val(d, v, "today", count_json(d, find(t->days, t->day_count, day))) &&
        yyjson_mut_obj_add_val(d, v, "month",
                               count_json(d, find(t->months, t->month_count, month))) &&
        yyjson_mut_obj_add_bool(d, v, "clock_valid", utc.tm_year >= 124);
    pthread_mutex_unlock(&t->mu);
    if (!ok) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    return d;
}
int qdc_traffic_write(qdc_traffic *t, qdc_error *e) {
    pthread_mutex_lock(&t->mu);
    if (t->load_failed) {
        pthread_mutex_unlock(&t->mu);
        qdc_error_set(e, "流量统计无法读取，原文件已保留");
        return -1;
    }
    yyjson_mut_doc *d = qdc_json_object();
    if (!d) {
        pthread_mutex_unlock(&t->mu);
        return -1;
    }
    yyjson_mut_val *v = yyjson_mut_doc_get_root(d);
    if (!yyjson_mut_obj_add_strcpy(d, v, "boot_id", t->boot) ||
        !yyjson_mut_obj_add_bool(d, v, "initialized", t->initialized) ||
        !yyjson_mut_obj_add_uint(d, v, "last_rx", t->last_rx) ||
        !yyjson_mut_obj_add_uint(d, v, "last_tx", t->last_tx) ||
        !yyjson_mut_obj_add_strcpy(d, v, "since", t->since) ||
        !yyjson_mut_obj_add_val(d, v, "total", count_json(d, t->total)) ||
        !yyjson_mut_obj_add_val(d, v, "session", count_json(d, t->session)))
        goto memory;
    yyjson_mut_val *days = yyjson_mut_obj(d), *months = yyjson_mut_obj(d);
    if (!days || !months || !yyjson_mut_obj_add_val(d, v, "days", days) ||
        !yyjson_mut_obj_add_val(d, v, "months", months))
        goto memory;
    for (size_t i = 0; i < t->day_count; i++)
        if (!yyjson_mut_obj_add_val(d, days, t->days[i].key, count_json(d, t->days[i].count)))
            goto memory;
    for (size_t i = 0; i < t->month_count; i++)
        if (!yyjson_mut_obj_add_val(d, months, t->months[i].key, count_json(d, t->months[i].count)))
            goto memory;
    int rc = qdc_save_json(t->path, d, e);
    if (rc && e && e->uncertain)
        t->load_failed = true;
    yyjson_mut_doc_free(d);
    pthread_mutex_unlock(&t->mu);
    return rc;
memory:
    yyjson_mut_doc_free(d);
    pthread_mutex_unlock(&t->mu);
    qdc_error_set(e, "流量统计内存不足，未覆盖原文件");
    return -1;
}
