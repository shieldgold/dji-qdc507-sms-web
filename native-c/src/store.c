#include "qdc_domain.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct qdc_store {
    pthread_mutex_t mu;
    yyjson_mut_doc *doc;
    char path[QDC_PATH_MAX];
    bool load_error;
};
static qdc_store *store(qdc_app *a) {
    return ((qdc_domain *)a->domain)->store;
}
static yyjson_mut_val *root(qdc_store *s) {
    return yyjson_mut_doc_get_root(s->doc);
}
#ifdef QDC_TESTING
size_t qdc_store_test_pool(qdc_app *a) {
    qdc_store *s = store(a);
    size_t n = 0;
    pthread_mutex_lock(&s->mu);
    for (yyjson_str_chunk *p = s->doc->str_pool.chunks; p; p = p->next)
        n += p->chunk_size;
    for (yyjson_val_chunk *p = s->doc->val_pool.chunks; p; p = p->next)
        n += p->chunk_size;
    pthread_mutex_unlock(&s->mu);
    return n;
}
#endif
static int compact(qdc_store *s) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return -1;
    yyjson_mut_val *r = yyjson_mut_val_mut_copy(d, root(s));
    if (!r) {
        yyjson_mut_doc_free(d);
        return -1;
    }
    yyjson_mut_doc_set_root(d, r);
    yyjson_mut_doc_free(s->doc);
    s->doc = d;
    return 0;
}
static int save(qdc_store *s, qdc_error *e) {
    if (s->load_error) {
        qdc_error_set(e, "短信归档无法读取，已保留原文件");
        (void)compact(s);
        return -1;
    }
    int rc = qdc_save_json(s->path, s->doc, e);
    if (rc && e && e->uncertain)
        s->load_error = true;
    (void)compact(s);
    return rc;
}
static yyjson_mut_val *normalize(yyjson_mut_doc *doc, yyjson_val *v, qdc_error *e) {
    if (!yyjson_is_obj(v)) {
        qdc_error_set(e, "短信记录必须为对象");
        return NULL;
    }
    yyjson_mut_val *m = yyjson_mut_obj(doc);
    if (!m)
        return NULL;
    static const char *names[] = {"key",        "sender",    "body",       "time",
                                  "status",     "direction", "request_id", "send_state",
                                  "send_error", "code",      "concat"};
    for (size_t i = 0; i < 11; i++) {
        yyjson_val *s = yyjson_obj_get(v, names[i]);
        if (s && !yyjson_is_null(s) && !yyjson_is_str(s)) {
            qdc_error_set(e, "短信字段类型错误");
            return NULL;
        }
        qdc_slice b = qdc_json_slice(s);
        if ((i < 6 || b.len) && !qdc_json_add_string(doc, m, names[i], b.ptr, b.len))
            return NULL;
    }
    static const char *nums[] = {"index", "parts", "sent_parts"};
    for (size_t i = 0; i < 3; i++) {
        yyjson_val *n = yyjson_obj_get(v, nums[i]);
        if (n && !yyjson_is_null(n) && !yyjson_is_int(n)) {
            qdc_error_set(e, "短信数值字段类型错误");
            return NULL;
        }
        int64_t x = yyjson_get_sint(n);
        if (x < INT_MIN || x > INT_MAX || (yyjson_is_uint(n) && yyjson_get_uint(n) > INT_MAX)) {
            qdc_error_set(e, "短信数值字段越界");
            return NULL;
        }
        if ((!i || x) && !yyjson_mut_obj_add_sint(doc, m, nums[i], x))
            return NULL;
    }
    return m;
}
static yyjson_mut_val *find(qdc_store *s, qdc_slice key) {
    return yyjson_mut_obj_getn(root(s), key.ptr, key.len);
}
static int put(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *name, yyjson_mut_val *v) {
    yyjson_mut_val *k = yyjson_mut_strcpy(doc, name);
    return k && v && yyjson_mut_obj_put(obj, k, v) ? 0 : -1;
}
int qdc_store_init(qdc_app *a, qdc_error *e) {
    qdc_store *s = calloc(1, sizeof *s);
    if (!s)
        return -1;
    pthread_mutex_init(&s->mu, NULL);
    s->doc = qdc_json_object();
    ((qdc_domain *)a->domain)->store = s;
    if (!s->doc || qdc_path(s->path, sizeof s->path, a->config.data_dir, "messages.json"))
        return -1;
    qdc_buf b = {0};
    int rc = qdc_read_file(s->path, 16u << 20, &b, e);
    if (rc == 1)
        return 0;
    if (rc < 0) {
        s->load_error = true;
        qdc_buf_free(&b);
        return 0;
    }
    yyjson_doc *d = qdc_json_read(b.data, b.len, e);
    qdc_buf_free(&b);
    if (!d) {
        s->load_error = true;
        return 0;
    }
    yyjson_val *r = yyjson_doc_get_root(d);
    if (!yyjson_is_obj(r) && !yyjson_is_null(r))
        s->load_error = true;
    if (yyjson_is_obj(r)) {
        size_t i, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(r, i, max, k, v) {
            yyjson_mut_val *m = normalize(s->doc, v, e);
            if (!m || !yyjson_mut_obj_add(root(s), yyjson_val_mut_copy(s->doc, k), m)) {
                s->load_error = true;
                break;
            }
        }
    }
    yyjson_doc_free(d);
    return 0;
}
void qdc_store_free(qdc_app *a) {
    qdc_store *s = store(a);
    if (!s)
        return;
    yyjson_mut_doc_free(s->doc);
    pthread_mutex_destroy(&s->mu);
    free(s);
    ((qdc_domain *)a->domain)->store = NULL;
}
yyjson_doc *qdc_store_get(qdc_app *a, qdc_slice key) {
    qdc_store *s = store(a);
    pthread_mutex_lock(&s->mu);
    yyjson_mut_val *m = find(s, key);
    yyjson_doc *d = m ? yyjson_mut_val_imut_copy(m, NULL) : NULL;
    pthread_mutex_unlock(&s->mu);
    return d;
}
static int time_cmp(const void *pa, const void *pb) {
    yyjson_mut_val *const *a = pa, *const *b = pb;
    yyjson_mut_val *av = yyjson_mut_obj_get(*a, "time"), *bv = yyjson_mut_obj_get(*b, "time");
    const char *as = yyjson_mut_get_str(av), *bs = yyjson_mut_get_str(bv);
    size_t an = yyjson_mut_get_len(av), bn = yyjson_mut_get_len(bv);
    int c = memcmp(as ? as : "", bs ? bs : "", an < bn ? an : bn);
    return c ? -c : (bn > an) - (bn < an);
}
static yyjson_mut_val **sorted(qdc_store *s, size_t *n) {
    *n = yyjson_mut_obj_size(root(s));
    yyjson_mut_val **p = calloc(*n ? *n : 1, sizeof *p);
    if (!p)
        return NULL;
    size_t i, max;
    yyjson_mut_val *k, *v;
    yyjson_mut_obj_foreach(root(s), i, max, k, v) {
        (void)k;
        p[i] = v;
    }
    qsort(p, *n, sizeof *p, time_cmp);
    return p;
}
static yyjson_doc *snapshot(qdc_app *a, bool push) {
    qdc_store *s = store(a);
    pthread_mutex_lock(&s->mu);
    size_t n;
    yyjson_mut_val **p = sorted(s, &n);
    yyjson_mut_doc *d = qdc_json_array();
    if (!p || !d) {
        free(p);
        if (d)
            yyjson_mut_doc_free(d);
        pthread_mutex_unlock(&s->mu);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *m;
        if (push) {
            m = yyjson_mut_obj(d);
            const char *fields[] = {"key", "direction"};
            for (size_t j = 0; j < 2; j++) {
                yyjson_mut_val *v = yyjson_mut_obj_get(p[i], fields[j]);
                yyjson_mut_obj_add_val(d, m, fields[j], yyjson_mut_val_mut_copy(d, v));
            }
        } else
            m = yyjson_mut_val_mut_copy(d, p[i]);
        if (!m || !yyjson_mut_arr_append(yyjson_mut_doc_get_root(d), m)) {
            yyjson_mut_doc_free(d);
            free(p);
            pthread_mutex_unlock(&s->mu);
            return NULL;
        }
    }
    yyjson_doc *ret = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    free(p);
    pthread_mutex_unlock(&s->mu);
    return ret;
}
yyjson_doc *qdc_messages(qdc_app *a) {
    return snapshot(a, false);
}
yyjson_doc *qdc_push_messages(qdc_app *a) {
    return snapshot(a, true);
}
int qdc_store_insert(qdc_app *a, yyjson_val *m, qdc_error *e) {
    qdc_store *s = store(a);
    qdc_slice k = qdc_json_slice(yyjson_obj_get(m, "key"));
    pthread_mutex_lock(&s->mu);
    if (find(s, k)) {
        pthread_mutex_unlock(&s->mu);
        qdc_error_set(e, "短信标识已存在");
        return -1;
    }
    yyjson_mut_val *v = normalize(s->doc, m, e), *key = yyjson_mut_strncpy(s->doc, k.ptr, k.len);
    int rc = !v || !key || !yyjson_mut_obj_add(root(s), key, v) ? -1 : save(s, e);
    if (rc)
        yyjson_mut_obj_remove_keyn(root(s), k.ptr, k.len);
    pthread_mutex_unlock(&s->mu);
    return rc;
}
int qdc_store_remove(qdc_app *a, qdc_slice k, qdc_error *e) {
    qdc_store *s = store(a);
    pthread_mutex_lock(&s->mu);
    yyjson_mut_obj_remove_keyn(root(s), k.ptr, k.len);
    int rc = save(s, e);
    pthread_mutex_unlock(&s->mu);
    return rc;
}
int qdc_store_update_send(qdc_app *a, qdc_slice key, const char *state, const char *status,
                          int sent, const char *error, qdc_error *e) {
    qdc_store *s = store(a);
    pthread_mutex_lock(&s->mu);
    yyjson_mut_val *m = find(s, key);
    if (m) {
        if (state)
            put(s->doc, m, "send_state", yyjson_mut_strcpy(s->doc, state));
        if (status)
            put(s->doc, m, "status", yyjson_mut_strcpy(s->doc, status));
        if (sent >= 0) {
            if (sent)
                put(s->doc, m, "sent_parts", yyjson_mut_int(s->doc, sent));
            else
                yyjson_mut_obj_remove_key(m, "sent_parts");
        }
        if (error && *error)
            put(s->doc, m, "send_error", yyjson_mut_strcpy(s->doc, error));
    }
    int rc = save(s, e);
    pthread_mutex_unlock(&s->mu);
    return rc;
}
int qdc_store_recover(qdc_app *a, qdc_error *e) {
    qdc_store *s = store(a);
    pthread_mutex_lock(&s->mu);
    bool changed = false;
    size_t i, max;
    yyjson_mut_val *k, *v;
    yyjson_mut_obj_foreach(root(s), i, max, k, v) {
        (void)k;
        const char *state = yyjson_mut_get_str(yyjson_mut_obj_get(v, "send_state"));
        if (state && (!strcmp(state, "queued") || !strcmp(state, "sending"))) {
            put(s->doc, v, "send_state", yyjson_mut_str(s->doc, "uncertain"));
            put(s->doc, v, "status", yyjson_mut_str(s->doc, "结果待确认"));
            put(s->doc, v, "send_error",
                yyjson_mut_str(s->doc, "服务曾中断，请核对接收端后再决定是否重新发送"));
            changed = true;
        }
    }
    int rc = changed ? save(s, e) : 0;
    pthread_mutex_unlock(&s->mu);
    return rc;
}
int qdc_store_merge(qdc_app *a, yyjson_val *arr, int *added, qdc_error *e) {
    qdc_store *s = store(a);
    *added = 0;
    bool allocated = false;
    pthread_mutex_lock(&s->mu);
    size_t i, max;
    yyjson_val *m;
    yyjson_arr_foreach(arr, i, max, m) {
        qdc_slice k = qdc_json_slice(yyjson_obj_get(m, "key"));
        yyjson_mut_val *old = find(s, k);
        if (old) {
            /* Scalar index changes can reuse the existing value. Most polls are byte
             * identical; do not leave replaced nodes/strings in the document arena. */
            int64_t index = yyjson_get_sint(yyjson_obj_get(m, "index"));
            yyjson_mut_val *iv = yyjson_mut_obj_get(old, "index");
            if (!iv || !yyjson_mut_set_sint(iv, index)) {
                if (put(s->doc, old, "index", yyjson_mut_sint(s->doc, index))) {
                    pthread_mutex_unlock(&s->mu);
                    return -1;
                }
                allocated = true;
            }
            qdc_slice status = qdc_json_slice(yyjson_obj_get(m, "status"));
            yyjson_mut_val *sv = yyjson_mut_obj_get(old, "status");
            if (!yyjson_mut_is_str(sv) || yyjson_mut_get_len(sv) != status.len ||
                (status.len && memcmp(yyjson_mut_get_str(sv), status.ptr, status.len))) {
                if (put(s->doc, old, "status", qdc_json_string(s->doc, status.ptr, status.len))) {
                    pthread_mutex_unlock(&s->mu);
                    return -1;
                }
                allocated = true;
            }
        } else {
            yyjson_mut_val *v = normalize(s->doc, m, e);
            if (!v || !yyjson_mut_obj_add(root(s), yyjson_mut_strncpy(s->doc, k.ptr, k.len), v)) {
                pthread_mutex_unlock(&s->mu);
                return -1;
            }
            (*added)++;
            allocated = true;
        }
    }
    if (yyjson_mut_obj_size(root(s)) > 300) {
        size_t n;
        yyjson_mut_val **p = sorted(s, &n);
        if (p) {
            for (i = 300; i < n; i++) {
                const char *st = yyjson_mut_get_str(yyjson_mut_obj_get(p[i], "send_state"));
                if (st && (!strcmp(st, "queued") || !strcmp(st, "sending")))
                    continue;
                yyjson_mut_val *k = yyjson_mut_obj_get(p[i], "key");
                yyjson_mut_obj_remove_keyn(root(s), yyjson_mut_get_str(k), yyjson_mut_get_len(k));
            }
            free(p);
        }
    }
    int rc = *added ? save(s, e) : allocated ? compact(s) : 0;
    if (rc && (!e || !e->message[0]))
        qdc_error_set(e, "短信归档内存整理失败");
    pthread_mutex_unlock(&s->mu);
    return rc;
}
int qdc_store_restore(qdc_app *a, yyjson_val *arr, int *added, qdc_error *e) {
    *added = 0;
    if (!yyjson_is_arr(arr) || yyjson_arr_size(arr) > 300) {
        qdc_error_set(e, "一次最多恢复 300 条短信");
        return -1;
    }
    yyjson_mut_doc *validated = qdc_json_array();
    if (!validated)
        return -1;
    size_t i, max;
    yyjson_val *v;
    yyjson_arr_foreach(arr, i, max, v) {
        yyjson_mut_val *m = normalize(validated, v, e);
        if (!m) {
            qdc_error_set(e, "不支持的短信备份文件");
            goto invalid;
        }
        qdc_slice key = qdc_json_slice(yyjson_obj_get(v, "key")),
                  body = qdc_json_slice(yyjson_obj_get(v, "body")),
                  sender = qdc_json_slice(yyjson_obj_get(v, "sender")),
                  dir = qdc_json_slice(yyjson_obj_get(v, "direction"));
        if (!key.len || key.len > 24000 || body.len > 10000 || sender.len > 80 ||
            (!qdc_slice_eq(dir, "in") && !qdc_slice_eq(dir, "out"))) {
            qdc_error_set(e, "短信备份内容不完整或格式错误");
            goto invalid;
        }
        if (!yyjson_mut_arr_append(yyjson_mut_doc_get_root(validated), m))
            goto invalid;
    }
    yyjson_doc *before = qdc_messages(a);
    if (!before)
        goto invalid;
    yyjson_mut_doc *before_mut = yyjson_doc_mut_copy(before, NULL);
    yyjson_doc_free(before);
    char path[QDC_PATH_MAX];
    if (!before_mut ||
        qdc_path(path, sizeof path, a->config.data_dir, "messages.before-restore.json") ||
        qdc_save_json(path, before_mut, e)) {
        if (before_mut)
            yyjson_mut_doc_free(before_mut);
        qdc_error_set(e, "无法保存恢复前备份，未修改短信");
        goto invalid;
    }
    yyjson_mut_doc_free(before_mut);
    qdc_store *s = store(a);
    pthread_mutex_lock(&s->mu);
    yyjson_mut_doc *original = s->doc, *copy = yyjson_mut_doc_new(NULL);
    if (!copy) {
        pthread_mutex_unlock(&s->mu);
        goto invalid;
    }
    yyjson_mut_val *r = yyjson_mut_val_mut_copy(copy, root(s));
    if (!r) {
        yyjson_mut_doc_free(copy);
        pthread_mutex_unlock(&s->mu);
        goto invalid;
    }
    yyjson_mut_doc_set_root(copy, r);
    s->doc = copy;
    yyjson_mut_val *m;
    yyjson_mut_arr_foreach(yyjson_mut_doc_get_root(validated), i, max, m) {
        yyjson_mut_val *k = yyjson_mut_obj_get(m, "key");
        qdc_slice key = {yyjson_mut_get_str(k), yyjson_mut_get_len(k)};
        if (find(s, key))
            continue;
        if (yyjson_mut_obj_size(root(s)) >= 300)
            break;
        yyjson_mut_val *n = yyjson_mut_val_mut_copy(s->doc, m);
        if (!n || put(s->doc, n, "index", yyjson_mut_int(s->doc, 0)))
            goto rollback_memory;
        const char *st = yyjson_mut_get_str(yyjson_mut_obj_get(n, "send_state"));
        if (st && (!strcmp(st, "queued") || !strcmp(st, "sending"))) {
            if (put(s->doc, n, "send_state", yyjson_mut_str(s->doc, "uncertain")) ||
                put(s->doc, n, "status", yyjson_mut_str(s->doc, "导入记录，发送结果待确认")))
                goto rollback_memory;
        }
        yyjson_mut_val *new_key = yyjson_mut_strncpy(s->doc, key.ptr, key.len);
        if (!new_key || !yyjson_mut_obj_add(root(s), new_key, n))
            goto rollback_memory;
        (*added)++;
    }
    if (save(s, e)) {
        bool uncertain = e && e->uncertain;
        yyjson_mut_doc_free(s->doc);
        s->doc = original;
        *added = 0;
        pthread_mutex_unlock(&s->mu);
        qdc_error_set(e, uncertain ? "恢复落盘状态未确认，已停止继续写入，请刷新归档"
                                   : "恢复未保存，已撤销本次导入");
        if (e)
            e->uncertain = uncertain;
        goto invalid;
    }
    yyjson_mut_doc_free(original);
    pthread_mutex_unlock(&s->mu);
    yyjson_mut_doc_free(validated);
    return 0;
rollback_memory:
    yyjson_mut_doc_free(s->doc);
    s->doc = original;
    *added = 0;
    pthread_mutex_unlock(&s->mu);
    qdc_error_set(e, "恢复内存不足，未修改短信");
    goto invalid;
invalid:
    yyjson_mut_doc_free(validated);
    return -1;
}
