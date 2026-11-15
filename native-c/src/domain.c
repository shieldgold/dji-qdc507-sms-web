#include "qdc_domain.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool prefix(qdc_slice s, const char *p) {
    size_t n = strlen(p);
    return s.len >= n && !memcmp(s.ptr, p, n);
}
static int respond(qdc_response *s, int status, yyjson_doc *d) {
    if (!d)
        return -1;
    yyjson_mut_doc *m = yyjson_doc_mut_copy(d, NULL);
    yyjson_doc_free(d);
    if (!m)
        return -1;
    return qdc_response_json(s, status, m) ? -1 : 1;
}
static int error_response(qdc_response *s, int status, const char *text) {
    return qdc_response_error(s, status, text) ? -1 : 1;
}
static int combined(qdc_response *s, int status, const char *error, const char *field,
                    yyjson_doc *value) {
    yyjson_mut_doc *d = qdc_json_object();
    if (!d) {
        if (value)
            yyjson_doc_free(value);
        return -1;
    }
    yyjson_mut_val *r = yyjson_mut_doc_get_root(d);
    yyjson_mut_obj_add_strcpy(d, r, "error", error);
    if (value) {
        yyjson_mut_obj_add_val(d, r, field, yyjson_val_mut_copy(d, yyjson_doc_get_root(value)));
        yyjson_doc_free(value);
    }
    return qdc_response_json(s, status, d) ? -1 : 1;
}
static yyjson_val *object(qdc_request *r, qdc_error *e) {
    yyjson_val *v = qdc_request_json(r, e);
    return v && (yyjson_is_obj(v) || yyjson_is_null(v)) ? v : NULL;
}
static bool string(yyjson_val *r, const char *k, qdc_slice *out) {
    yyjson_val *v = yyjson_obj_get(r, k);
    *out = (qdc_slice){"", 0};
    if (!v || yyjson_is_null(v))
        return true;
    if (!yyjson_is_str(v))
        return false;
    *out = qdc_json_slice(v);
    return true;
}
static bool boolean(yyjson_val *r, const char *k, bool *out) {
    yyjson_val *v = yyjson_obj_get(r, k);
    *out = false;
    if (!v || yyjson_is_null(v))
        return true;
    if (!yyjson_is_bool(v))
        return false;
    *out = yyjson_get_bool(v);
    return true;
}
void qdc_sms_notify(qdc_app *a) {
    qdc_domain *d = a ? a->domain : NULL;
    if (!d)
        return;
    pthread_mutex_lock(&d->wake_mu);
    d->poll_wake = true;
    pthread_cond_signal(&d->wake);
    pthread_mutex_unlock(&d->wake_mu);
}
void qdc_domain_last_error(qdc_app *a, char *out, size_t cap) {
    qdc_domain *d = a->domain;
    pthread_mutex_lock(&d->state_mu);
    snprintf(out, cap, "%s", d->last_error);
    pthread_mutex_unlock(&d->state_mu);
}
static bool digits(qdc_slice s) {
    if (!s.len)
        return false;
    for (size_t i = 0; i < s.len; i++)
        if (s.ptr[i] < '0' || s.ptr[i] > '9')
            return false;
    return true;
}
static int iccid(qdc_app *a, char out[32], qdc_error *e) {
    static const char *cmds[] = {"AT+QCCID", "AT+CCID", "AT+ICCID"};
    out[0] = 0;
    for (size_t j = 0; j < 3; j++) {
        qdc_buf b = {0};
        qdc_error hidden = {0};
        int rc = qdc_command(a, cmds[j], 3000, &b, &hidden);
        if (!rc)
            for (size_t at = 0; at < b.len;) {
                size_t end = at;
                while (end < b.len && b.data[end] != '\n')
                    end++;
                qdc_slice s = {(char *)b.data + at, end - at};
                at = end + 1;
                if (!prefix(s, "+QCCID:") && !prefix(s, "+CCID:") && !prefix(s, "+ICCID:"))
                    continue;
                const char *colon = memchr(s.ptr, ':', s.len);
                s = qdc_trim((qdc_slice){colon + 1, s.len - (size_t)(colon + 1 - s.ptr)});
                while (s.len && s.ptr[0] == '"') {
                    s.ptr++;
                    s.len--;
                }
                while (s.len && s.ptr[s.len - 1] == '"')
                    s.len--;
                if (s.len >= 15 && s.len <= 22 && digits(s)) {
                    memcpy(out, s.ptr, s.len);
                    out[s.len] = 0;
                    break;
                }
            }
        qdc_buf_free(&b);
        if (out[0])
            return 0;
    }
    qdc_error_set(e, "未能实时识别 SIM 卡，未使用保存的凭据");
    return -1;
}
static void extras(qdc_app *a, qdc_sim_identity *id) {
    qdc_buf b = {0};
    qdc_error e = {0};
    if (!qdc_command(a, "AT+CIMI", 5000, &b, &e))
        for (size_t at = 0; at < b.len;) {
            size_t end = at;
            while (end < b.len && b.data[end] != '\n')
                end++;
            qdc_slice s = {(char *)b.data + at, end - at};
            at = end + 1;
            if (s.len >= 14 && s.len < sizeof id->imsi && digits(s)) {
                memcpy(id->imsi, s.ptr, s.len);
                id->imsi[s.len] = 0;
            }
        }
    qdc_buf_free(&b);
    if (!qdc_command(a, "AT+CNUM", 5000, &b, &e))
        for (size_t at = 0; at < b.len;) {
            size_t end = at;
            while (end < b.len && b.data[end] != '\n')
                end++;
            qdc_slice s = {(char *)b.data + at, end - at};
            at = end + 1;
            if (!prefix(s, "+CNUM:"))
                continue;
            for (size_t i = 0; i < s.len; i++)
                if (s.ptr[i] == '"') {
                    size_t start = ++i;
                    while (i < s.len && s.ptr[i] != '"')
                        i++;
                    if (i > start && s.ptr[start] == '+') {
                        char number[96] = "+";
                        size_t used = 1;
                        for (size_t j = start + 1; j < i && used < sizeof number - 1; j++)
                            if (s.ptr[j] >= '0' && s.ptr[j] <= '9')
                                number[used++] = s.ptr[j];
                        if (used >= 12) {
                            number[used] = 0;
                            memcpy(id->number, number, used + 1);
                            break;
                        }
                    }
                }
        }
    qdc_buf_free(&b);
}
int qdc_domain_identity(qdc_app *a, bool fresh, qdc_sim_identity *out, qdc_error *e) {
    qdc_domain *d = a->domain;
    if (!fresh) {
        pthread_mutex_lock(&d->identity_mu);
        bool cached = d->identity.key[0] && strcmp(d->identity.key, "unknown") &&
                      (qdc_mono_ms() - d->identity_at <= 60000 || qdc_outbox_busy(a));
        if (cached) {
            *out = d->identity;
            pthread_mutex_unlock(&d->identity_mu);
            return 0;
        }
        pthread_mutex_unlock(&d->identity_mu);
        pthread_mutex_lock(&a->op_mu);
    }
    qdc_sim_identity id = {0};
    int rc = iccid(a, id.iccid, e);
    if (!fresh)
        extras(a, &id);
    if (id.iccid[0])
        snprintf(id.key, sizeof id.key, "iccid:%s", id.iccid);
    else if (!fresh && id.imsi[0])
        snprintf(id.key, sizeof id.key, "imsi:%s", id.imsi);
    else
        strcpy(id.key, "unknown");
    if (strcmp(id.key, "unknown")) {
        if (a->auth && qdc_auth_bind(a, id.key, e))
            rc = -1;
        else {
            if (a->push && id.iccid[0])
                qdc_push_bind(a, id.iccid);
            rc = 0;
        }
    }
    pthread_mutex_lock(&d->identity_mu);
    if (!rc) {
        if (fresh && strcmp(d->identity.key, id.key) == 0) {
            snprintf(id.imsi, sizeof id.imsi, "%s", d->identity.imsi);
            snprintf(id.number, sizeof id.number, "%s", d->identity.number);
        }
        d->identity = id;
        d->identity_at = qdc_mono_ms();
    } else {
        memset(&d->identity, 0, sizeof d->identity);
        strcpy(d->identity.key, "unknown");
        d->identity_at = 0;
    }
    *out = d->identity;
    pthread_mutex_unlock(&d->identity_mu);
    if (!fresh)
        pthread_mutex_unlock(&a->op_mu);
    return rc;
}
static bool hex_line(qdc_slice s) {
    if (s.len < 8 || s.len % 2)
        return false;
    for (size_t i = 0; i < s.len; i++)
        if (!((s.ptr[i] >= '0' && s.ptr[i] <= '9') || (s.ptr[i] >= 'a' && s.ptr[i] <= 'f') ||
              (s.ptr[i] >= 'A' && s.ptr[i] <= 'F')))
            return false;
    return true;
}
static int list_messages(qdc_app *a, yyjson_doc **messages, qdc_error *e) {
    *messages = NULL;
    qdc_buf b = {0};
    int rc = qdc_command(a, "AT+CMGL=4", 20000, &b, e);
    if (rc && !b.len) {
        qdc_buf_free(&b);
        return -1;
    }
    qdc_part *p = NULL;
    size_t n = 0;
    bool current = false;
    int index = 0;
    char status[32] = "";
    for (size_t at = 0; at < b.len;) {
        size_t end = at;
        while (end < b.len && b.data[end] != '\n')
            end++;
        qdc_slice s = qdc_trim((qdc_slice){(char *)b.data + at, end - at});
        at = end + 1;
        if (prefix(s, "+CMGL:")) {
            current = true;
            index = 0;
            status[0] = 0;
            char *text = malloc(s.len + 1);
            if (!text)
                goto fail;
            memcpy(text, s.ptr, s.len);
            text[s.len] = 0;
            (void)sscanf(text, "+CMGL: %d", &index);
            char *comma = strchr(text, ',');
            if (comma) {
                char *last = strchr(comma + 1, ',');
                qdc_slice st = qdc_trim(
                    (qdc_slice){comma + 1, last ? (size_t)(last - comma - 1) : strlen(comma + 1)});
                if (st.len < sizeof status) {
                    memcpy(status, st.ptr, st.len);
                    status[st.len] = 0;
                }
            }
            free(text);
            continue;
        }
        if (current && hex_line(s)) {
            qdc_part item = {0};
            qdc_error ignored = {0};
            if (!qdc_pdu_deliver(s, &item, &ignored)) {
                qdc_part *next = realloc(p, (n + 1) * sizeof *p);
                if (!next) {
                    qdc_part_free(&item);
                    goto fail;
                }
                p = next;
                item.index = index;
                snprintf(item.status, sizeof item.status, "%s", status);
                p[n++] = item;
            }
            current = false;
        }
    }
    *messages = qdc_merge_parts(p, n);
    for (size_t i = 0; i < n; i++)
        qdc_part_free(&p[i]);
    free(p);
    qdc_buf_free(&b);
    if (!*messages) {
        qdc_error_set(e, "内存不足");
        return -1;
    }
    return 0;
fail:
    for (size_t i = 0; i < n; i++)
        qdc_part_free(&p[i]);
    free(p);
    qdc_buf_free(&b);
    qdc_error_set(e, "内存不足");
    return -1;
}
static void poll_once(qdc_app *a) {
    if (qdc_calls_busy(a))
        return;
    qdc_domain *d = a->domain;
    pthread_mutex_lock(&a->op_mu);
    qdc_error e = {0};
    yyjson_doc *msgs = NULL;
    int rc = qdc_ensure_sms(a, &e);
    if (!rc)
        rc = list_messages(a, &msgs, &e);
    if (!rc) {
        int added;
        (void)qdc_store_merge(a, yyjson_doc_get_root(msgs), &added, &e);
    }
    if (msgs)
        yyjson_doc_free(msgs);
    pthread_mutex_lock(&d->state_mu);
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(d->last_poll, sizeof d->last_poll, "%H:%M:%S", &t);
    snprintf(d->last_error, sizeof d->last_error, "%s", rc ? e.message : "");
    pthread_mutex_unlock(&d->state_mu);
    pthread_mutex_unlock(&a->op_mu);
}
static void *poll_loop(void *arg) {
    qdc_app *a = arg;
    qdc_domain *d = a->domain;
    while (!atomic_load(&a->stopping)) {
        poll_once(a);
        pthread_mutex_lock(&d->wake_mu);
        if (!d->poll_wake && !atomic_load(&a->stopping)) {
            unsigned ms = a->config.poll_ms ? a->config.poll_ms : 3000;
            qdc_cond_until(&d->wake, &d->wake_mu, qdc_mono_ms() + ms);
        }
        d->poll_wake = false;
        pthread_mutex_unlock(&d->wake_mu);
    }
    return NULL;
}
static void *calls_loop(void *arg) {
    qdc_app *a = arg;
    while (!atomic_load(&a->stopping)) {
        qdc_error e = {0};
        qdc_calls_refresh(a, &e);
        qdc_sleep(a, 1000);
    }
    return NULL;
}
int qdc_domain_init(qdc_app *a, qdc_error *e) {
    qdc_domain *d = calloc(1, sizeof *d);
    if (!d) {
        qdc_error_set(e, "内存不足");
        return -1;
    }
    a->domain = d;
    d->app = a;
    pthread_mutex_init(&d->state_mu, NULL);
    pthread_mutex_init(&d->identity_mu, NULL);
    pthread_mutex_init(&d->wake_mu, NULL);
    if (qdc_cond_init(&d->wake)) {
        pthread_mutex_destroy(&d->wake_mu);
        pthread_mutex_destroy(&d->identity_mu);
        pthread_mutex_destroy(&d->state_mu);
        free(d);
        a->domain = NULL;
        qdc_error_set(e, "无法初始化单调轮询等待器");
        return -1;
    }
    strcpy(d->last_poll, "00:00:00");
    if (qdc_mkdir_private(a->config.data_dir, e) || qdc_store_init(a, e) || qdc_pin_init(a, e) ||
        qdc_calls_init(a, e) || qdc_outbox_init(a, e))
        return -1;
    return 0;
}
int qdc_domain_start(qdc_app *a, qdc_error *e) {
    qdc_domain *d = a->domain;
    int rc = -1;
    for (unsigned i = 0; i < 60 && !atomic_load(&a->stopping); i++) {
        rc = qdc_at_open(a, e);
        if (!rc)
            break;
        qdc_sleep(a, 2000);
    }
    if (rc)
        return -1;
    if (qdc_domain_thread(&d->calls_thread, calls_loop, a)) {
        qdc_error_set(e, "无法启动通话轮询");
        return -1;
    }
    d->calls_started = true;
    if (qdc_domain_thread(&d->poll_thread, poll_loop, a)) {
        qdc_error_set(e, "无法启动短信轮询");
        return -1;
    }
    d->poll_started = true;
    return 0;
}
void qdc_domain_free(qdc_app *a) {
    qdc_domain *d = a ? a->domain : NULL;
    if (!d)
        return;
    atomic_store(&a->stopping, true);
    pthread_mutex_lock(&d->wake_mu);
    pthread_cond_broadcast(&d->wake);
    pthread_mutex_unlock(&d->wake_mu);
    if (d->poll_started)
        pthread_join(d->poll_thread, NULL);
    if (d->calls_started)
        pthread_join(d->calls_thread, NULL);
    qdc_outbox_free(a);
    qdc_at_close(a);
    qdc_calls_free(a);
    qdc_pin_free(a);
    qdc_store_free(a);
    pthread_cond_destroy(&d->wake);
    pthread_mutex_destroy(&d->wake_mu);
    pthread_mutex_destroy(&d->identity_mu);
    pthread_mutex_destroy(&d->state_mu);
    free(d);
    a->domain = NULL;
}
static int state_route(qdc_app *a, qdc_response *s) {
    qdc_domain *d = a->domain;
    yyjson_mut_doc *m = qdc_json_object();
    if (!m)
        return -1;
    yyjson_mut_val *r = yyjson_mut_doc_get_root(m);
    yyjson_doc *msgs = qdc_messages(a);
    if (!msgs) {
        yyjson_mut_doc_free(m);
        return -1;
    }
    yyjson_mut_obj_add_val(m, r, "messages", yyjson_val_mut_copy(m, yyjson_doc_get_root(msgs)));
    yyjson_doc_free(msgs);
    pthread_mutex_lock(&d->state_mu);
    yyjson_mut_obj_add_strcpy(m, r, "lastPoll", d->last_poll);
    if (d->last_error[0])
        yyjson_mut_obj_add_strcpy(m, r, "lastError", d->last_error);
    pthread_mutex_unlock(&d->state_mu);
    if (!qdc_outbox_busy(a)) {
        yyjson_doc *pin = qdc_pin_snapshot(a);
        if (pin) {
            yyjson_mut_obj_add_val(m, r, "sim", yyjson_val_mut_copy(m, yyjson_doc_get_root(pin)));
            yyjson_doc_free(pin);
        }
    }
    return qdc_response_json(s, 200, m) ? -1 : 1;
}
static int binding_route(qdc_app *a, qdc_response *s) {
    qdc_error e = {0};
    qdc_sim_identity id = {0};
    pthread_mutex_lock(&a->op_mu);
    int rc = qdc_domain_identity(a, true, &id, &e);
    if (rc) {
        pthread_mutex_unlock(&a->op_mu);
        return error_response(s, 503, e.message);
    }
    yyjson_doc *pin = qdc_pin_snapshot(a);
    yyjson_mut_doc *m = qdc_json_object();
    if (!m || !pin) {
        if (m)
            yyjson_mut_doc_free(m);
        if (pin)
            yyjson_doc_free(pin);
        pthread_mutex_unlock(&a->op_mu);
        return -1;
    }
    yyjson_mut_val *r = yyjson_mut_doc_get_root(m);
    yyjson_mut_obj_add_strcpy(m, r, "iccid", id.iccid);
    yyjson_mut_obj_add_int(m, r, "binding_version", 1);
    yyjson_mut_obj_add_val(m, r, "sim", yyjson_val_mut_copy(m, yyjson_doc_get_root(pin)));
    yyjson_doc_free(pin);
    pthread_mutex_unlock(&a->op_mu);
    return qdc_response_json(s, 200, m) ? -1 : 1;
}
int qdc_domain_route(qdc_app *a, qdc_request *req, qdc_response *s) {
    qdc_domain *d = a->domain;
    qdc_error e = {0};
    bool get = !strcmp(req->method, "GET"), post = !strcmp(req->method, "POST");
    const char *p = req->path;
    if (get && !strcmp(p, "/api/state"))
        return state_route(a, s);
    if (get && !strcmp(p, "/api/sim"))
        return respond(s, 200, qdc_pin_snapshot(a));
    if (get && !strcmp(p, "/api/sim/binding"))
        return binding_route(a, s);
    if (get && !strcmp(p, "/api/send/status")) {
        qdc_buf id = {0}, key = {0};
        qdc_query(req->query, "request_id", &id);
        if (!qdc_request_id((qdc_slice){(char *)id.data, id.len})) {
            qdc_buf_free(&id);
            return error_response(s, 400, "无效请求标识");
        }
        qdc_buf_puts(&key, "out:");
        qdc_buf_append(&key, id.data, id.len);
        yyjson_doc *m = qdc_store_get(a, (qdc_slice){(char *)key.data, key.len});
        qdc_buf_free(&id);
        qdc_buf_free(&key);
        return m ? respond(s, 200, m)
                 : error_response(s, 404, "尚未找到该发送记录，请先刷新列表，勿直接重发");
    }
    if (get && !strcmp(p, "/api/calls")) {
        yyjson_doc *calls = qdc_calls_snapshot(a), *audio = qdc_audio_snapshot(a);
        yyjson_mut_doc *m = qdc_json_object();
        if (!calls || !audio || !m) {
            if (calls)
                yyjson_doc_free(calls);
            if (audio)
                yyjson_doc_free(audio);
            if (m)
                yyjson_mut_doc_free(m);
            return -1;
        }
        yyjson_mut_val *r = yyjson_mut_doc_get_root(m);
        yyjson_mut_obj_add_val(m, r, "telephone",
                               yyjson_val_mut_copy(m, yyjson_doc_get_root(calls)));
        yyjson_mut_obj_add_val(m, r, "audio", yyjson_val_mut_copy(m, yyjson_doc_get_root(audio)));
        yyjson_mut_obj_add_false(m, r, "background_incoming");
        yyjson_doc_free(calls);
        yyjson_doc_free(audio);
        return qdc_response_json(s, 200, m) ? -1 : 1;
    }
    if (!post)
        return 0;
    if (!strcmp(p, "/api/send")) {
        pthread_mutex_lock(&a->action_mu);
        if (a->maintenance) {
            pthread_mutex_unlock(&a->action_mu);
            return error_response(s, 409, "正在切换版本，请稍后刷新");
        }
        if (qdc_calls_busy(a)) {
            pthread_mutex_unlock(&a->action_mu);
            return error_response(s, 409, "电话正在进行，请结束后再发送短信");
        }
        yyjson_val *r = object(req, &e);
        qdc_slice id, number, text;
        if (!r || !string(r, "request_id", &id) || !string(r, "number", &number) ||
            !string(r, "text", &text)) {
            pthread_mutex_unlock(&a->action_mu);
            return error_response(s, 400, "请求格式错误");
        }
        yyjson_doc *m = NULL;
        int rc = qdc_outbox_enqueue(a, id, number, text, &m, &e);
        pthread_mutex_unlock(&a->action_mu);
        return rc ? error_response(s, 400, e.message) : respond(s, 202, m);
    }
    if (!strcmp(p, "/api/delete")) {
        yyjson_val *r = object(req, &e);
        qdc_slice key;
        if (!r || !string(r, "key", &key))
            return error_response(s, 400, "请求格式错误");
        pthread_mutex_lock(&a->op_mu);
        yyjson_doc *m = qdc_store_get(a, key);
        if (!m) {
            pthread_mutex_unlock(&a->op_mu);
            return error_response(s, 404, "找不到该短信");
        }
        yyjson_val *v = yyjson_doc_get_root(m);
        qdc_slice state = qdc_json_slice(yyjson_obj_get(v, "send_state"));
        if (qdc_slice_eq(state, "queued") || qdc_slice_eq(state, "sending")) {
            yyjson_doc_free(m);
            pthread_mutex_unlock(&a->op_mu);
            return error_response(s, 409, "短信正在提交，暂时不能删除");
        }
        char note[768] = "";
        int64_t index = yyjson_get_sint(yyjson_obj_get(v, "index"));
        if (index > 0 && qdc_slice_eq(qdc_json_slice(yyjson_obj_get(v, "direction")), "in")) {
            char cmd[64];
            snprintf(cmd, sizeof cmd, "AT+CMGD=%lld", (long long)index);
            qdc_buf b = {0};
            if (qdc_command(a, cmd, 10000, &b, &e))
                snprintf(note, sizeof note, "（模块侧删除失败: %s，已从本机列表移除）", e.message);
            qdc_buf_free(&b);
        }
        qdc_store_remove(a, key, &e);
        yyjson_doc_free(m);
        pthread_mutex_unlock(&a->op_mu);
        yyjson_mut_doc *out = qdc_json_object();
        yyjson_mut_val *o = yyjson_mut_doc_get_root(out);
        yyjson_mut_obj_add_true(out, o, "ok");
        yyjson_mut_obj_add_strcpy(out, o, "note", note);
        return qdc_response_json(s, 200, out) ? -1 : 1;
    }
    if (!strcmp(p, "/api/sim/unlock")) {
        yyjson_val *r = object(req, &e);
        qdc_slice pin, id, expected;
        if (!r || !string(r, "pin", &pin) || !string(r, "request_id", &id) ||
            !string(r, "expected_iccid", &expected))
            return error_response(s, 400, "请求格式错误");
        pthread_mutex_lock(&a->op_mu);
        if (expected.len) {
            qdc_sim_identity current = {0};
            if (qdc_domain_identity(a, true, &current, &e) ||
                expected.len != strlen(current.iccid) ||
                memcmp(expected.ptr, current.iccid, expected.len)) {
                pthread_mutex_unlock(&a->op_mu);
                return error_response(s, 409, "SIM 卡已变化或无法确认，未提交 PIN");
            }
            if (!qdc_auth_check(a, req->cookie, current.key)) {
                pthread_mutex_unlock(&a->op_mu);
                return error_response(s, 401, "请先登录当前 SIM 卡");
            }
        }
        yyjson_doc *sim = NULL;
        int rc = qdc_pin_unlock(a, pin, id, &sim, &e);
        if (!rc)
            d->sms_configured = false;
        pthread_mutex_unlock(&a->op_mu);
        return rc ? combined(s, 400, e.message, "sim", sim) : respond(s, 200, sim);
    }
    if (!strcmp(p, "/api/sim/retry")) {
        yyjson_val *r = object(req, &e);
        bool confirmed;
        if (!r || !boolean(r, "confirmed", &confirmed))
            return error_response(s, 400, "请求格式错误");
        pthread_mutex_lock(&a->op_mu);
        int rc = qdc_pin_retry(a, confirmed, &e);
        yyjson_doc *sim = rc ? NULL : qdc_pin_snapshot(a);
        pthread_mutex_unlock(&a->op_mu);
        return rc ? error_response(s, 400, e.message) : respond(s, 200, sim);
    }
    if (!strcmp(p, "/api/calls/dial")) {
        pthread_mutex_lock(&a->action_mu);
        if (a->maintenance || qdc_outbox_busy(a)) {
            pthread_mutex_unlock(&a->action_mu);
            return error_response(s, 409, "正在处理短信或维护，请稍后拨号");
        }
        if (!qdc_audio_available(a)) {
            pthread_mutex_unlock(&a->action_mu);
            return error_response(s, 409, "模块音频尚未准备好，未拨号");
        }
        yyjson_val *r = object(req, &e);
        qdc_slice number, id;
        if (!r || !string(r, "number", &number) || !string(r, "request_id", &id)) {
            pthread_mutex_unlock(&a->action_mu);
            return error_response(s, 400, "请求格式错误");
        }
        yyjson_doc *calls = NULL;
        int rc = qdc_calls_dial(a, number, id, &calls, &e);
        pthread_mutex_unlock(&a->action_mu);
        return rc ? combined(s, 409, e.message, "telephone", calls) : respond(s, 202, calls);
    }
    if (!strcmp(p, "/api/calls/answer") || !strcmp(p, "/api/calls/hangup") ||
        !strcmp(p, "/api/calls/dtmf")) {
        yyjson_val *r = object(req, &e);
        qdc_slice id, digit;
        char call_id[128];
        if (!r || !string(r, "id", &id) || !string(r, "digit", &digit) || !id.len ||
            id.len >= sizeof call_id || memchr(id.ptr, 0, id.len))
            return error_response(s, 400, "缺少当前通话标识");
        memcpy(call_id, id.ptr, id.len);
        call_id[id.len] = 0;
        const char *action = strrchr(p, '/') + 1;
        pthread_mutex_lock(&a->action_mu);
        if (!strcmp(action, "answer") && !qdc_audio_available(a)) {
            pthread_mutex_unlock(&a->action_mu);
            return error_response(s, 409, "模块音频尚未准备好，未接听");
        }
        int rc = qdc_calls_action(a, call_id, action, digit, &e);
        pthread_mutex_unlock(&a->action_mu);
        if (rc)
            return error_response(s, 409, e.message);
        yyjson_mut_doc *m = qdc_json_object();
        yyjson_mut_obj_add_true(m, yyjson_mut_doc_get_root(m), "accepted");
        return qdc_response_json(s, 202, m) ? -1 : 1;
    }
    return 0;
}
