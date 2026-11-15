#include "qdc_domain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct qdc_outbox {
    pthread_mutex_t mu;
    pthread_t worker;
    bool active, started;
};
typedef struct {
    qdc_app *app;
    qdc_pdus pdus;
    qdc_buf key;
} outgoing;
bool qdc_outbox_busy(qdc_app *a) {
    qdc_domain *d = a ? a->domain : NULL;
    if (!d || !d->outbox)
        return false;
    pthread_mutex_lock(&d->outbox->mu);
    bool b = d->outbox->active;
    pthread_mutex_unlock(&d->outbox->mu);
    return b;
}
int qdc_outbox_init(qdc_app *a, qdc_error *e) {
    qdc_outbox *q = calloc(1, sizeof *q);
    if (!q) {
        qdc_error_set(e, "内存不足");
        return -1;
    }
    pthread_mutex_init(&q->mu, NULL);
    ((qdc_domain *)a->domain)->outbox = q;
    (void)qdc_store_recover(a, e);
    return 0;
}
static void *send_worker(void *arg) {
    outgoing *j = arg;
    qdc_app *a = j->app;
    qdc_domain *d = a->domain;
    qdc_error e = {0}, ignored = {0};
    qdc_slice key = {(char *)j->key.data, j->key.len};
    size_t sent = 0;
    (void)qdc_store_update_send(a, key, "sending", "正在提交", -1, NULL, &ignored);
    pthread_mutex_lock(&a->op_mu);
    int rc = qdc_ensure_sms(a, &e);
    for (size_t i = 0; !rc && i < j->pdus.len; i++) {
        qdc_buf response = {0};
        rc = qdc_at_submit(a, (qdc_slice){(char *)j->pdus.items[i].data, j->pdus.items[i].len},
                           false, &response, &e);
        qdc_buf_free(&response);
        if (!rc) {
            sent = i + 1;
            (void)qdc_store_update_send(a, key, NULL, NULL, (int)sent, NULL, &ignored);
        }
    }
    pthread_mutex_unlock(&a->op_mu);
    if (!rc)
        (void)qdc_store_update_send(a, key, "submitted", "已提交，送达未确认", (int)j->pdus.len,
                                    NULL, &ignored);
    else {
        const char *state = sent          ? "partial"
                            : e.uncertain ? "uncertain"
                                          : "failed",
                   *status = sent          ? "部分已提交，结果待确认"
                             : e.uncertain ? "结果待确认"
                                           : "提交失败";
        char error[768];
        snprintf(error, sizeof error, "%s（已确认提交 %zu/%zu 条分片）", e.message, sent,
                 j->pdus.len);
        (void)qdc_store_update_send(a, key, state, status, (int)sent, error, &ignored);
    }
    qdc_pdus_free(&j->pdus);
    qdc_buf_free(&j->key);
    free(j);
    pthread_mutex_lock(&d->outbox->mu);
    d->outbox->active = false;
    pthread_mutex_unlock(&d->outbox->mu);
    return NULL;
}
int qdc_outbox_enqueue(qdc_app *a, qdc_slice id, qdc_slice number, qdc_slice text,
                       yyjson_doc **result, qdc_error *e) {
    *result = NULL;
    if (!qdc_request_id(id)) {
        qdc_error_set(e, "缺少有效的发送请求标识，请刷新后重试");
        return -1;
    }
    number = qdc_trim(number);
    outgoing *j = calloc(1, sizeof *j);
    if (!j) {
        qdc_error_set(e, "内存不足");
        return -1;
    }
    j->app = a;
    if (qdc_pdu_submit(number, text, (unsigned)(qdc_mono_ms() % 255) + 1, &j->pdus, e) ||
        qdc_buf_puts(&j->key, "out:") || qdc_buf_append(&j->key, id.ptr, id.len))
        goto fail;
    qdc_domain *d = a->domain;
    qdc_outbox *q = d->outbox;
    pthread_mutex_lock(&q->mu);
    yyjson_doc *old = qdc_store_get(a, (qdc_slice){(char *)j->key.data, j->key.len});
    if (old) {
        yyjson_val *r = yyjson_doc_get_root(old);
        qdc_slice n = qdc_json_slice(yyjson_obj_get(r, "sender")),
                  t = qdc_json_slice(yyjson_obj_get(r, "body"));
        if (n.len != number.len || t.len != text.len || memcmp(n.ptr, number.ptr, n.len) ||
            memcmp(t.ptr, text.ptr, t.len)) {
            yyjson_doc_free(old);
            qdc_error_set(e, "请求标识已用于另一条短信，请重新创建短信");
            pthread_mutex_unlock(&q->mu);
            goto fail;
        }
        *result = old;
        pthread_mutex_unlock(&q->mu);
        qdc_pdus_free(&j->pdus);
        qdc_buf_free(&j->key);
        free(j);
        return 0;
    }
    if (q->active) {
        qdc_error_set(e, "上一条短信仍在提交，请等待结果后再发送");
        pthread_mutex_unlock(&q->mu);
        goto fail;
    }
    if (q->started) {
        pthread_join(q->worker, NULL);
        q->started = false;
    }
    yyjson_mut_doc *m = qdc_json_object();
    if (!m) {
        pthread_mutex_unlock(&q->mu);
        goto fail;
    }
    yyjson_mut_val *r = yyjson_mut_doc_get_root(m);
    char now[40];
    qdc_rfc3339(qdc_now_ms(a), now);
    bool ok = qdc_json_add_string(m, r, "key", j->key.data, j->key.len) &&
              qdc_json_add_string(m, r, "request_id", id.ptr, id.len) &&
              qdc_json_add_string(m, r, "sender", number.ptr, number.len) &&
              qdc_json_add_string(m, r, "body", text.ptr, text.len) &&
              yyjson_mut_obj_add_int(m, r, "index", 0) &&
              yyjson_mut_obj_add_strcpy(m, r, "time", now) &&
              yyjson_mut_obj_add_str(m, r, "direction", "out") &&
              yyjson_mut_obj_add_str(m, r, "status", "等待发送") &&
              yyjson_mut_obj_add_str(m, r, "send_state", "queued") &&
              yyjson_mut_obj_add_uint(m, r, "parts", j->pdus.len);
    yyjson_doc *immutable = ok ? yyjson_mut_doc_imut_copy(m, NULL) : NULL;
    yyjson_mut_doc_free(m);
    if (!immutable || qdc_store_insert(a, yyjson_doc_get_root(immutable), e)) {
        if (immutable)
            yyjson_doc_free(immutable);
        qdc_error_set(e, "无法保存发送记录，短信尚未提交");
        pthread_mutex_unlock(&q->mu);
        goto fail;
    }
    yyjson_doc_free(immutable);
    *result = qdc_store_get(a, (qdc_slice){(char *)j->key.data, j->key.len});
    q->active = true;
    if (!*result || qdc_domain_thread(&q->worker, send_worker, j)) {
        q->active = false;
        if (*result) {
            yyjson_doc_free(*result);
            *result = NULL;
        }
        qdc_error ignored = {0};
        qdc_store_update_send(a, (qdc_slice){(char *)j->key.data, j->key.len}, "failed", "提交失败",
                              0, "无法启动发送线程，短信尚未提交", &ignored);
        qdc_error_set(e, "无法启动发送线程，短信尚未提交");
        pthread_mutex_unlock(&q->mu);
        goto fail;
    }
    q->started = true;
    pthread_mutex_unlock(&q->mu);
    return 0;
fail:
    qdc_pdus_free(&j->pdus);
    qdc_buf_free(&j->key);
    free(j);
    return -1;
}
void qdc_outbox_free(qdc_app *a) {
    qdc_domain *d = a->domain;
    qdc_outbox *q = d->outbox;
    if (!q)
        return;
    if (q->started)
        pthread_join(q->worker, NULL);
    pthread_mutex_destroy(&q->mu);
    free(q);
    d->outbox = NULL;
}
