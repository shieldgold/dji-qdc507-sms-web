#include "qdc_domain.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int index, stat;
    bool incoming;
    qdc_buf number;
    char id[25], request_id[81];
} voice;
struct qdc_calls {
    pthread_mutex_t mu;
    voice *items;
    size_t count;
    bool pending, load_error;
    uint64_t pending_at;
    char pending_request[81], updated[40], error[512], path[QDC_PATH_MAX];
    yyjson_mut_doc *attempts;
};
static const char *states[] = {"active", "held", "dialing", "alerting", "incoming", "waiting"};
static void voices_free(voice *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        qdc_buf_free(&p[i].number);
    free(p);
}
static yyjson_doc *snapshot_locked(qdc_calls *c) {
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return NULL;
    yyjson_mut_val *r = yyjson_mut_doc_get_root(d), *arr = yyjson_mut_obj_add_arr(d, r, "calls");
    if (!arr)
        goto fail;
    for (size_t i = 0; i < c->count; i++) {
        voice *v = &c->items[i];
        yyjson_mut_val *m = yyjson_mut_arr_add_obj(d, arr);
        if (!m || !yyjson_mut_obj_add_strcpy(d, m, "id", v->id) ||
            !yyjson_mut_obj_add_int(d, m, "index", v->index) ||
            !qdc_json_add_string(d, m, "number", v->number.data, v->number.len) ||
            !yyjson_mut_obj_add_str(d, m, "direction", v->incoming ? "in" : "out") ||
            !yyjson_mut_obj_add_str(d, m, "state", states[v->stat]))
            goto fail;
        if (v->request_id[0] && !yyjson_mut_obj_add_strcpy(d, m, "request_id", v->request_id))
            goto fail;
    }
    if (!yyjson_mut_obj_add_bool(d, r, "pending", c->pending) ||
        !yyjson_mut_obj_add_strcpy(d, r, "updated_at", c->updated) ||
        (c->error[0] && !yyjson_mut_obj_add_strcpy(d, r, "error", c->error)))
        goto fail;
    yyjson_doc *ret = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    return ret;
fail:
    yyjson_mut_doc_free(d);
    return NULL;
}
yyjson_doc *qdc_calls_snapshot(qdc_app *a) {
    qdc_domain *d = a ? a->domain : NULL;
    if (!d || !d->calls)
        return NULL;
    qdc_calls *c = d->calls;
    pthread_mutex_lock(&c->mu);
    yyjson_doc *s = snapshot_locked(c);
    pthread_mutex_unlock(&c->mu);
    return s;
}
bool qdc_calls_busy(qdc_app *a) {
    qdc_domain *d = a ? a->domain : NULL;
    if (!d || !d->calls)
        return false;
    qdc_calls *c = d->calls;
    pthread_mutex_lock(&c->mu);
    bool b = c->pending || c->count;
    pthread_mutex_unlock(&c->mu);
    return b;
}
static int csv(qdc_slice line, qdc_buf f[8], size_t *count) {
    size_t at = 0;
    *count = 0;
    while (at <= line.len) {
        size_t col = (*count)++;
        if (col >= 8)
            return -1;
        while (at < line.len && (line.ptr[at] == ' ' || line.ptr[at] == '\t'))
            at++;
        if (at < line.len && line.ptr[at] == '"') {
            at++;
            bool closed = false;
            while (at < line.len) {
                char ch = line.ptr[at++];
                if (ch == '"') {
                    if (at < line.len && line.ptr[at] == '"') {
                        at++;
                        if (qdc_buf_append(&f[col], "\"", 1))
                            return -1;
                    } else {
                        closed = true;
                        break;
                    }
                } else if (qdc_buf_append(&f[col], &ch, 1))
                    return -1;
            }
            if (!closed || (at < line.len && line.ptr[at] != ','))
                return -1;
        } else {
            while (at < line.len && line.ptr[at] != ',') {
                char ch = line.ptr[at++];
                if (ch == '"' || qdc_buf_append(&f[col], &ch, 1))
                    return -1;
            }
        }
        if (at == line.len)
            break;
        at++;
    }
    return 0;
}
static int integer(qdc_buf *b, int *out) {
    qdc_slice s = qdc_trim((qdc_slice){(char *)b->data, b->len});
    if (!s.len || s.len > 16 || memchr(s.ptr, 0, s.len))
        return -1;
    char x[20];
    memcpy(x, s.ptr, s.len);
    x[s.len] = 0;
    char *end;
    errno = 0;
    long v = strtol(x, &end, 10);
    if (errno || *end || v < INT_MIN || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}
static int parse(qdc_buf *lines, voice **items, size_t *count, qdc_error *e) {
    *items = NULL;
    *count = 0;
    for (size_t at = 0; at < lines->len;) {
        size_t end = at;
        while (end < lines->len && lines->data[end] != '\n')
            end++;
        qdc_slice s = {(char *)lines->data + at, end - at};
        at = end + 1;
        if (s.len < 6 || memcmp(s.ptr, "+CLCC:", 6))
            continue;
        s = qdc_trim((qdc_slice){s.ptr + 6, s.len - 6});
        qdc_buf f[8] = {{0}};
        size_t nf = 0;
        int n[5], rc = csv(s, f, &nf);
        if (nf < 5)
            rc = -1;
        for (size_t i = 0; !rc && i < 5; i++)
            rc = integer(&f[i], &n[i]);
        if (!rc && (n[0] < 1 || n[1] < 0 || n[1] > 1 || n[2] < 0 || n[2] > 5))
            rc = -1;
        if (!rc && n[3] == 0) {
            voice *p = realloc(*items, (*count + 1) * sizeof **items);
            if (!p)
                rc = -1;
            else {
                *items = p;
                voice *v = &p[(*count)++];
                memset(v, 0, sizeof *v);
                v->index = n[0];
                v->incoming = n[1] == 1;
                v->stat = n[2];
                if (nf > 5) {
                    v->number = f[5];
                    memset(&f[5], 0, sizeof f[5]);
                }
            }
        }
        for (size_t i = 0; i < 8; i++)
            qdc_buf_free(&f[i]);
        if (rc) {
            voices_free(*items, *count);
            *items = NULL;
            *count = 0;
            qdc_error_set(e, "无法解析模块呼叫列表");
            return -1;
        }
    }
    return 0;
}
static int refresh_locked(qdc_app *a, qdc_error *e) {
    qdc_calls *c = ((qdc_domain *)a->domain)->calls;
    qdc_buf lines = {0};
    voice *new_items = NULL;
    size_t count = 0;
    int rc = qdc_command(a, "AT+CLCC", 3000, &lines, e);
    if (!rc)
        rc = parse(&lines, &new_items, &count, e);
    qdc_buf_free(&lines);
    if (rc)
        goto fail;
    for (size_t i = 0; i < count; i++) {
        voice *v = &new_items[i];
        for (size_t j = 0; j < c->count; j++) {
            voice *old = &c->items[j];
            if (old->index == v->index && old->incoming == v->incoming &&
                old->number.len == v->number.len &&
                (!v->number.len || !memcmp(old->number.data, v->number.data, v->number.len))) {
                memcpy(v->id, old->id, sizeof v->id);
                memcpy(v->request_id, old->request_id, sizeof v->request_id);
                break;
            }
        }
        if (!v->id[0]) {
            if (qdc_random_hex(v->id, 12, e)) {
                voices_free(new_items, count);
                goto fail;
            }
            if (!v->incoming && c->pending_at)
                snprintf(v->request_id, sizeof v->request_id, "%s", c->pending_request);
        }
    }
    voices_free(c->items, c->count);
    c->items = new_items;
    c->count = count;
    c->error[0] = 0;
    if (count || (c->pending_at && qdc_mono_ms() - c->pending_at > 15000)) {
        c->pending = false;
        c->pending_at = 0;
        c->pending_request[0] = 0;
    }
    qdc_rfc3339(qdc_now_ms(a), c->updated);
    return 0;
fail:
    snprintf(c->error, sizeof c->error, "无法确认电话状态，请检查模块连接");
    return -1;
}
int qdc_calls_refresh(qdc_app *a, qdc_error *e) {
    qdc_calls *c = ((qdc_domain *)a->domain)->calls;
    pthread_mutex_lock(&c->mu);
    int rc = refresh_locked(a, e);
    pthread_mutex_unlock(&c->mu);
    return rc;
}
int qdc_calls_init(qdc_app *a, qdc_error *e) {
    qdc_calls *c = calloc(1, sizeof *c);
    if (!c)
        return -1;
    pthread_mutex_init(&c->mu, NULL);
    c->attempts = qdc_json_array();
    ((qdc_domain *)a->domain)->calls = c;
    if (!c->attempts || qdc_path(c->path, sizeof c->path, a->config.data_dir, "call-attempts.json"))
        return -1;
    qdc_buf b = {0};
    int rc = qdc_read_file(c->path, 1u << 20, &b, e);
    if (rc == 1)
        return 0;
    if (rc < 0) {
        c->load_error = true;
        qdc_buf_free(&b);
        return 0;
    }
    yyjson_doc *d = qdc_json_read(b.data, b.len, e);
    qdc_buf_free(&b);
    if (!d) {
        c->load_error = true;
        return 0;
    }
    yyjson_val *r = yyjson_doc_get_root(d);
    if (!yyjson_is_arr(r) && !yyjson_is_null(r))
        c->load_error = true;
    else if (yyjson_is_arr(r)) {
        size_t i, max;
        yyjson_val *v;
        yyjson_arr_foreach(r, i, max, v) {
            yyjson_val *id = yyjson_obj_get(v, "request_id"), *num = yyjson_obj_get(v, "number");
            if (!yyjson_is_obj(v) || (id && !yyjson_is_str(id) && !yyjson_is_null(id)) ||
                (num && !yyjson_is_str(num) && !yyjson_is_null(num))) {
                c->load_error = true;
                break;
            }
            yyjson_mut_val *entry = yyjson_val_mut_copy(c->attempts, v);
            if (!entry || !yyjson_mut_arr_append(yyjson_mut_doc_get_root(c->attempts), entry)) {
                c->load_error = true;
                break;
            }
        }
    }
    yyjson_doc_free(d);
    return 0;
}
void qdc_calls_free(qdc_app *a) {
    qdc_domain *d = a->domain;
    qdc_calls *c = d->calls;
    if (!c)
        return;
    voices_free(c->items, c->count);
    yyjson_mut_doc_free(c->attempts);
    pthread_mutex_destroy(&c->mu);
    free(c);
    d->calls = NULL;
}
static bool number_valid(qdc_slice n) {
    size_t i = n.len && n.ptr[0] == '+' ? 1 : 0;
    if (n.len - i < 3 || n.len - i > 20)
        return false;
    for (; i < n.len; i++)
        if (n.ptr[i] < '0' || n.ptr[i] > '9')
            return false;
    return true;
}
int qdc_calls_dial(qdc_app *a, qdc_slice number, qdc_slice id, yyjson_doc **snapshot,
                   qdc_error *e) {
    *snapshot = NULL;
    qdc_calls *c = ((qdc_domain *)a->domain)->calls;
    if (!number_valid(number) || !qdc_request_id(id)) {
        qdc_error_set(e, "请输入有效电话号码和请求标识");
        *snapshot = qdc_calls_snapshot(a);
        return -1;
    }
    pthread_mutex_lock(&c->mu);
    size_t i, max;
    yyjson_mut_val *v;
    yyjson_mut_arr_foreach(yyjson_mut_doc_get_root(c->attempts), i, max, v) {
        yyjson_mut_val *iv = yyjson_mut_obj_get(v, "request_id");
        if (yyjson_mut_get_len(iv) == id.len && !memcmp(yyjson_mut_get_str(iv), id.ptr, id.len)) {
            yyjson_mut_val *nv = yyjson_mut_obj_get(v, "number");
            bool same = yyjson_mut_get_len(nv) == number.len &&
                        !memcmp(yyjson_mut_get_str(nv), number.ptr, number.len);
            qdc_error_set(e, same ? "该拨号请求已处理，请刷新通话状态；不会重复拨号"
                                  : "请求标识已经用于另一个号码");
            goto fail;
        }
    }
    if (refresh_locked(a, e)) {
        qdc_error_set(e, "电话状态无法确认，未拨号");
        goto fail;
    }
    if (c->count || c->pending) {
        qdc_error_set(e, "已有电话或拨号正在进行");
        goto fail;
    }
    if (c->load_error) {
        qdc_error_set(e, "拨号保护记录无法读取，未拨号");
        goto fail;
    }
    yyjson_mut_doc *attempts = qdc_json_array();
    if (!attempts)
        goto fail;
    yyjson_mut_val *arr = yyjson_mut_doc_get_root(attempts);
    size_t total = yyjson_mut_arr_size(yyjson_mut_doc_get_root(c->attempts));
    yyjson_mut_arr_foreach(yyjson_mut_doc_get_root(c->attempts), i, max,
                           v) if (total < 64 || i >= total - 63) {
        yyjson_mut_val *copy = yyjson_mut_val_mut_copy(attempts, v);
        if (!copy || !yyjson_mut_arr_append(arr, copy)) {
            yyjson_mut_doc_free(attempts);
            qdc_error_set(e, "拨号保护记录内存不足，未拨号");
            goto fail;
        }
    }
    yyjson_mut_val *attempt = yyjson_mut_arr_add_obj(attempts, arr);
    if (!attempt || !qdc_json_add_string(attempts, attempt, "request_id", id.ptr, id.len) ||
        !qdc_json_add_string(attempts, attempt, "number", number.ptr, number.len)) {
        yyjson_mut_doc_free(attempts);
        qdc_error_set(e, "拨号保护记录内存不足，未拨号");
        goto fail;
    }
    if (qdc_save_json(c->path, attempts, e)) {
        bool uncertain = e && e->uncertain;
        if (uncertain)
            c->load_error = true;
        yyjson_mut_doc_free(attempts);
        qdc_error_set(e, "拨号保护记录未保存，未拨号");
        if (e)
            e->uncertain = uncertain;
        goto fail;
    }
    yyjson_mut_doc_free(c->attempts);
    c->attempts = attempts;
    c->pending = true;
    c->pending_at = qdc_mono_ms();
    memcpy(c->pending_request, id.ptr, id.len);
    c->pending_request[id.len] = 0;
    char command[28];
    memcpy(command, "ATD", 3);
    memcpy(command + 3, number.ptr, number.len);
    command[number.len + 3] = ';';
    command[number.len + 4] = 0;
    qdc_buf lines = {0};
    int rc = qdc_command(a, command, 10000, &lines, e);
    qdc_buf_free(&lines);
    if (rc) {
        snprintf(c->error, sizeof c->error, "拨号结果尚未确认，请刷新状态，勿重复拨号");
        qdc_error_set(e, "%s", c->error);
        goto fail;
    }
    *snapshot = snapshot_locked(c);
    pthread_mutex_unlock(&c->mu);
    return 0;
fail:
    *snapshot = snapshot_locked(c);
    pthread_mutex_unlock(&c->mu);
    return -1;
}
int qdc_calls_action(qdc_app *a, const char *id, const char *action, qdc_slice digit,
                     qdc_error *e) {
    if (!strcmp(action, "dtmf") &&
        (digit.len != 1 || !digit.ptr[0] || !strchr("0123456789*#ABCD", digit.ptr[0]))) {
        qdc_error_set(e, "无效的按键");
        return -1;
    }
    qdc_calls *c = ((qdc_domain *)a->domain)->calls;
    pthread_mutex_lock(&c->mu);
    if (refresh_locked(a, e)) {
        qdc_error_set(e, "无法确认当前电话，未执行操作");
        goto fail;
    }
    for (size_t i = 0; i < c->count; i++)
        if (!strcmp(c->items[i].id, id)) {
            voice *v = &c->items[i];
            bool allowed = !strcmp(action, "hangup") ||
                           (!strcmp(action, "answer") && (v->stat == 4 || v->stat == 5)) ||
                           (!strcmp(action, "dtmf") && v->stat == 0);
            if (!allowed) {
                qdc_error_set(e, "当前电话状态不支持此操作");
                goto fail;
            }
            if (c->count > 1) {
                qdc_error_set(e, "检测到多路电话，请在模块的原电话终端处理");
                goto fail;
            }
            char command[32];
            if (!strcmp(action, "hangup"))
                strcpy(command, "AT+CHUP");
            else if (!strcmp(action, "answer"))
                strcpy(command, "ATA");
            else
                snprintf(command, sizeof command, "AT+VTS=\"%c\"", digit.ptr[0]);
            qdc_buf b = {0};
            int rc = qdc_command(a, command, 5000, &b, e);
            qdc_buf_free(&b);
            if (rc) {
                qdc_error_set(e, "模块未确认操作结果，请刷新电话状态");
                goto fail;
            }
            pthread_mutex_unlock(&c->mu);
            return 0;
        }
    qdc_error_set(e, "这通电话已结束或状态已变化，请刷新");
fail:
    pthread_mutex_unlock(&c->mu);
    return -1;
}
int qdc_calls_hangup(qdc_app *a, const char *id, qdc_error *e) {
    return qdc_calls_action(a, id, "hangup", (qdc_slice){"", 0}, e);
}
