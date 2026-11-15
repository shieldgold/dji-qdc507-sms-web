#include "qdc_push.h"
#include <limits.h>
#include <mbedtls/asn1.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/x509_crt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PUSH_SEEN_MAX 8192
#define PUSH_CALL_MAX 128
#define PUSH_QUEUE_MAX 128
typedef unsigned char push_hash[32];
typedef char push_call[25];
typedef struct {
    char id[37], kind[8], call_id[25], call_uuid[37];
    int count;
    unsigned attempts;
    int64_t created, next;
} push_event;
typedef struct {
    qdc_push_subscription subscription;
    push_hash *seen;
    size_t seen_count;
    push_call *calls;
    size_t call_count;
    push_event *queue;
    size_t queue_count;
} push_state;
typedef struct {
    pthread_mutex_t mu;
    mbedtls_pk_context key;
    char key_id[11], team_id[11], jwt[512], ca_file[QDC_PATH_MAX];
    int64_t token_at;
    uint64_t clock_refresh;
    bool configured;
} apns_sender;
typedef struct {
    qdc_app *app;
    pthread_mutex_t mu;
    pthread_t worker;
    atomic_bool stop;
    bool started, load_error, in_flight;
    char path[QDC_PATH_MAX], last_error[512], last_accepted[40];
    unsigned accepted;
    push_state state;
    apns_sender sender;
#ifdef QDC_TESTING
    qdc_push_test_deliver_fn deliver;
    void *deliver_context;
#endif
} push_service;

static bool hex_string(const char *s, size_t min, size_t max) {
    if (!s)
        return false;
    size_t n = strlen(s);
    if (n < min || n > max)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}
static void hex_encode(const unsigned char *p, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[p[i] >> 4];
        out[i * 2 + 1] = digits[p[i] & 15];
    }
    out[n * 2] = 0;
}
static int hex_decode(const char *s, size_t n, unsigned char *out) {
    for (size_t i = 0; i < n; i++) {
        int a = s[i * 2] <= '9' ? s[i * 2] - '0' : s[i * 2] - 'a' + 10;
        int b = s[i * 2 + 1] <= '9' ? s[i * 2 + 1] - '0' : s[i * 2 + 1] - 'a' + 10;
        if (a < 0 || a > 15 || b < 0 || b > 15)
            return -1;
        out[i] = (unsigned char)((a << 4) | b);
    }
    return 0;
}
void qdc_call_push_uuid(const char *id, char out[37]) {
    qdc_buf input = {0};
    unsigned char digest[32];
    char text[33];
    if (!id || qdc_buf_puts(&input, "module-call:") || qdc_buf_puts(&input, id) ||
        qdc_sha256(input.data, input.len, digest)) {
        if (out)
            out[0] = 0;
        qdc_buf_free(&input);
        return;
    }
    qdc_buf_free(&input);
    digest[6] = (digest[6] & 15) | 0x40;
    digest[8] = (digest[8] & 63) | 0x80;
    hex_encode(digest, 16, text);
    snprintf(out, 37, "%.8s-%.4s-%.4s-%.4s-%.12s", text, text + 8, text + 12, text + 16, text + 20);
}
static bool uuid_valid(const char *s) {
    if (!s || strlen(s) != 36)
        return false;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return false;
        } else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    }
    return true;
}
static int event_id(char out[37], qdc_error *error) {
    char random[65];
    if (qdc_random_hex(random, 32, error))
        return -1;
    qdc_call_push_uuid(random, out);
    mbedtls_platform_zeroize(random, sizeof(random));
    return out[0] ? 0 : -1;
}
int qdc_apns_payload(const qdc_push_delivery *delivery, qdc_buf *out, qdc_error *error) {
    if (!delivery || !out || !hex_string(delivery->token, 64, 512) || strlen(delivery->token) % 2 ||
        !uuid_valid(delivery->event_id))
        goto invalid;
    yyjson_mut_doc *doc = qdc_json_object();
    if (!doc)
        return -1;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc), *aps = yyjson_mut_obj(doc);
    if (!aps || !yyjson_mut_obj_add_val(doc, root, "aps", aps) ||
        !yyjson_mut_obj_add_str(doc, root, "event_id", delivery->event_id))
        goto oom;
    if (!strcmp(delivery->kind, "call")) {
        char uuid[37];
        qdc_call_push_uuid(delivery->call_id, uuid);
        if (!hex_string(delivery->call_id, 24, 24) || strcmp(uuid, delivery->call_uuid)) {
            yyjson_mut_doc_free(doc);
            goto invalid;
        }
        if (!yyjson_mut_obj_add_str(doc, root, "call_id", delivery->call_id) ||
            !yyjson_mut_obj_add_str(doc, root, "call_uuid", delivery->call_uuid))
            goto oom;
    } else {
        char title[96];
        if (!strcmp(delivery->kind, "test"))
            snprintf(title, sizeof(title), "小电波 · 后台推送测试");
        else if (!strcmp(delivery->kind, "sms") && delivery->count >= 1 && delivery->count <= 300) {
            if (delivery->count == 1)
                snprintf(title, sizeof(title), "模块收到新短信");
            else
                snprintf(title, sizeof(title), "模块收到 %d 条新短信", delivery->count);
        } else {
            yyjson_mut_doc_free(doc);
            goto invalid;
        }
        yyjson_mut_val *alert = yyjson_mut_obj(doc);
        if (!alert || !yyjson_mut_obj_add_val(doc, aps, "alert", alert) ||
            !yyjson_mut_obj_add_strcpy(doc, alert, "title", title) ||
            !yyjson_mut_obj_add_str(doc, alert, "body", "点按查看模块。") ||
            !yyjson_mut_obj_add_str(doc, aps, "sound", "default") ||
            !yyjson_mut_obj_add_str(doc, aps, "thread-id", "module-sms") ||
            !yyjson_mut_obj_add_str(doc, root, "destination", "inbox"))
            goto oom;
    }
    size_t len;
    char *encoded = yyjson_mut_write(doc, 0, &len);
    if (!encoded)
        goto oom;
    int rc = qdc_buf_append(out, encoded, len);
    free(encoded);
    yyjson_mut_doc_free(doc);
    return rc;
oom:
    yyjson_mut_doc_free(doc);
    qdc_error_set(error, "推送内容创建失败");
    return -1;
invalid:
    qdc_error_set(error, "推送事件格式无效");
    return -1;
}

static void state_free(push_state *s) {
    free(s->seen);
    free(s->calls);
    free(s->queue);
    memset(s, 0, sizeof(*s));
}
static int state_copy(push_state *dest, const push_state *source) {
    *dest = *source;
    dest->seen = NULL;
    dest->calls = NULL;
    dest->queue = NULL;
    if (source->seen_count && !(dest->seen = malloc(source->seen_count * sizeof(push_hash))))
        goto fail;
    if (source->call_count && !(dest->calls = malloc(source->call_count * sizeof(push_call))))
        goto fail;
    if (source->queue_count && !(dest->queue = malloc(source->queue_count * sizeof(push_event))))
        goto fail;
    if (source->seen_count)
        memcpy(dest->seen, source->seen, source->seen_count * sizeof(push_hash));
    if (source->call_count)
        memcpy(dest->calls, source->calls, source->call_count * sizeof(push_call));
    if (source->queue_count)
        memcpy(dest->queue, source->queue, source->queue_count * sizeof(push_event));
    return 0;
fail:
    state_free(dest);
    return -1;
}
static int hash_compare(const void *a, const void *b) {
    return memcmp(a, b, 32);
}
static int call_compare(const void *a, const void *b) {
    return strcmp(a, b);
}
static bool hash_has(const push_state *s, const unsigned char hash[32]) {
    return s->seen_count &&
           bsearch(hash, s->seen, s->seen_count, sizeof(push_hash), hash_compare) != NULL;
}
static bool call_has(const push_state *s, const char *id) {
    return s->call_count &&
           bsearch(id, s->calls, s->call_count, sizeof(push_call), call_compare) != NULL;
}
static int append_call(push_state *s, const char *id) {
    for (size_t i = 0; i < s->call_count; i++)
        if (!strcmp(s->calls[i], id))
            return 0;
    if (s->call_count >= PUSH_CALL_MAX)
        return -1;
    push_call *p = realloc(s->calls, (s->call_count + 1) * sizeof(*p));
    if (!p)
        return -1;
    s->calls = p;
    snprintf(s->calls[s->call_count++], 25, "%s", id);
    return 0;
}
static int append_event(push_state *s, const push_event *event) {
    if (s->queue_count >= PUSH_QUEUE_MAX)
        return -1;
    push_event *p = realloc(s->queue, (s->queue_count + 1) * sizeof(*p));
    if (!p)
        return -1;
    s->queue = p;
    s->queue[s->queue_count++] = *event;
    return 0;
}
static void remove_event(push_state *s, size_t index) {
    memmove(s->queue + index, s->queue + index + 1,
            (s->queue_count - index - 1) * sizeof(push_event));
    s->queue_count--;
}
static int message_hashes(yyjson_val *messages, push_hash **out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (!messages)
        return 0;
    if (!yyjson_is_arr(messages) || yyjson_arr_size(messages) > PUSH_SEEN_MAX)
        return -1;
    size_t size = yyjson_arr_size(messages);
    push_hash *hashes = size ? malloc(size * sizeof(push_hash)) : NULL;
    if (size && !hashes)
        return -1;
    size_t i, max;
    yyjson_val *m;
    yyjson_arr_foreach(messages, i, max, m) {
        yyjson_val *key = yyjson_obj_get(m, "key");
        qdc_slice text = qdc_json_slice(key);
        if (!yyjson_is_str(key) || qdc_sha256(text.ptr, text.len, hashes[(*count)++])) {
            free(hashes);
            *count = 0;
            return -1;
        }
    }
    if (*count)
        qsort(hashes, *count, sizeof(push_hash), hash_compare);
    size_t unique = 0;
    for (size_t j = 0; j < *count; j++)
        if (!unique || memcmp(hashes[j], hashes[unique - 1], 32)) {
            if (unique != j)
                memcpy(hashes[unique], hashes[j], 32);
            unique++;
        }
    *count = unique;
    *out = hashes;
    return 0;
}
static bool subscription_valid(const qdc_push_subscription *s) {
    return s && s->iccid[0] && s->device_id[0] &&
           (!s->enabled || (hex_string(s->token, 64, 512) && strlen(s->token) % 2 == 0)) &&
           (!s->calls_enabled ||
            (hex_string(s->voip_token, 64, 512) && strlen(s->voip_token) % 2 == 0));
}
static bool subscription_equal(const qdc_push_subscription *a, const qdc_push_subscription *b) {
    return !strcmp(a->iccid, b->iccid) && !strcmp(a->device_id, b->device_id) &&
           !strcmp(a->token, b->token) && !strcmp(a->voip_token, b->voip_token) &&
           a->enabled == b->enabled && a->calls_enabled == b->calls_enabled;
}
static int read_string(yyjson_val *object, const char *key, char *out, size_t cap) {
    yyjson_val *v = yyjson_obj_get(object, key);
    out[0] = 0;
    return v && !yyjson_is_null(v) ? qdc_json_cstr(v, out, cap) : 0;
}
static int read_bool(yyjson_val *object, const char *key, bool *out) {
    yyjson_val *v = yyjson_obj_get(object, key);
    *out = false;
    if (v && !yyjson_is_null(v) && !yyjson_is_bool(v))
        return -1;
    if (v)
        *out = yyjson_get_bool(v);
    return 0;
}
static int read_subscription(yyjson_val *object, qdc_push_subscription *s) {
    if (!object || yyjson_is_null(object))
        return 0;
    if (!yyjson_is_obj(object) || read_string(object, "iccid", s->iccid, sizeof(s->iccid)) ||
        read_string(object, "device_id", s->device_id, sizeof(s->device_id)) ||
        read_string(object, "token", s->token, sizeof(s->token)) ||
        read_string(object, "voip_token", s->voip_token, sizeof(s->voip_token)) ||
        read_bool(object, "enabled", &s->enabled) ||
        read_bool(object, "calls_enabled", &s->calls_enabled))
        return -1;
    return 0;
}
static int state_read(push_state *s, yyjson_val *root) {
    if (!yyjson_is_obj(root) ||
        read_subscription(yyjson_obj_get(root, "subscription"), &s->subscription))
        return -1;
    yyjson_val *seen = yyjson_obj_get(root, "seen"), *calls = yyjson_obj_get(root, "calls"),
               *queue = yyjson_obj_get(root, "queue");
    size_t i, max;
    yyjson_val *key, *value;
    if (seen && !yyjson_is_null(seen)) {
        if (!yyjson_is_obj(seen) || yyjson_obj_size(seen) > PUSH_SEEN_MAX)
            return -1;
        if (yyjson_obj_size(seen) && !(s->seen = malloc(yyjson_obj_size(seen) * sizeof(push_hash))))
            return -1;
        yyjson_obj_foreach(seen, i, max, key, value) {
            const char *text = yyjson_get_str(key);
            if (yyjson_get_len(key) != 64 || !hex_string(text, 64, 64) || !yyjson_is_bool(value))
                return -1;
            if (yyjson_get_bool(value) && hex_decode(text, 32, s->seen[s->seen_count++]))
                return -1;
        }
        if (s->seen_count)
            qsort(s->seen, s->seen_count, sizeof(push_hash), hash_compare);
    }
    if (calls && !yyjson_is_null(calls)) {
        if (!yyjson_is_obj(calls) || yyjson_obj_size(calls) > PUSH_CALL_MAX)
            return -1;
        yyjson_obj_foreach(calls, i, max, key, value) {
            const char *id = yyjson_get_str(key);
            if (yyjson_get_len(key) != 24 || !hex_string(id, 24, 24) || !yyjson_is_bool(value))
                return -1;
            if (yyjson_get_bool(value) && append_call(s, id))
                return -1;
        }
        if (s->call_count)
            qsort(s->calls, s->call_count, sizeof(push_call), call_compare);
    }
    if (queue && !yyjson_is_null(queue)) {
        if (!yyjson_is_arr(queue) || yyjson_arr_size(queue) > PUSH_QUEUE_MAX)
            return -1;
        yyjson_arr_foreach(queue, i, max, value) {
            push_event e = {0};
            yyjson_val *v;
            if (!yyjson_is_obj(value) || read_string(value, "id", e.id, sizeof(e.id)) ||
                !uuid_valid(e.id) || read_string(value, "kind", e.kind, sizeof(e.kind)) ||
                (strcmp(e.kind, "sms") && strcmp(e.kind, "test") && strcmp(e.kind, "call")) ||
                read_string(value, "call_id", e.call_id, sizeof(e.call_id)) ||
                read_string(value, "call_uuid", e.call_uuid, sizeof(e.call_uuid)))
                return -1;
            v = yyjson_obj_get(value, "count");
            if (v && (!yyjson_is_int(v) || yyjson_get_sint(v) < 0 || yyjson_get_uint(v) > INT_MAX))
                return -1;
            e.count = (int)yyjson_get_sint(v);
            v = yyjson_obj_get(value, "attempts");
            if (v && (!yyjson_is_uint(v) || yyjson_get_uint(v) > UINT_MAX))
                return -1;
            e.attempts = (unsigned)yyjson_get_uint(v);
            v = yyjson_obj_get(value, "created");
            if (v && qdc_parse_rfc3339(qdc_json_slice(v), &e.created))
                return -1;
            v = yyjson_obj_get(value, "next");
            if (v && qdc_parse_rfc3339(qdc_json_slice(v), &e.next))
                return -1;
            if (append_event(s, &e))
                return -1;
        }
    }
    return 0;
}
static yyjson_mut_doc *state_json(const push_state *s) {
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d), *sub = yyjson_mut_obj(d),
                   *seen = yyjson_mut_obj(d);
    yyjson_mut_val *calls = yyjson_mut_obj(d), *queue = yyjson_mut_arr(d);
#define ADD(value)                                                                                 \
    do {                                                                                           \
        if (!(value))                                                                              \
            goto oom;                                                                              \
    } while (0)
    ADD(sub && seen && calls && queue);
    ADD(yyjson_mut_obj_add_val(d, root, "subscription", sub));
    ADD(yyjson_mut_obj_add_val(d, root, "seen", seen));
    ADD(yyjson_mut_obj_add_val(d, root, "calls", calls));
    ADD(yyjson_mut_obj_add_val(d, root, "queue", queue));
    ADD(yyjson_mut_obj_add_str(d, sub, "iccid", s->subscription.iccid));
    ADD(yyjson_mut_obj_add_str(d, sub, "device_id", s->subscription.device_id));
    ADD(yyjson_mut_obj_add_str(d, sub, "token", s->subscription.token));
    ADD(yyjson_mut_obj_add_str(d, sub, "voip_token", s->subscription.voip_token));
    ADD(yyjson_mut_obj_add_bool(d, sub, "enabled", s->subscription.enabled));
    ADD(yyjson_mut_obj_add_bool(d, sub, "calls_enabled", s->subscription.calls_enabled));
    for (size_t i = 0; i < s->seen_count; i++) {
        char text[65];
        hex_encode(s->seen[i], 32, text);
        ADD(yyjson_mut_obj_add(seen, yyjson_mut_strcpy(d, text), yyjson_mut_bool(d, true)));
    }
    for (size_t i = 0; i < s->call_count; i++)
        ADD(yyjson_mut_obj_add(calls, yyjson_mut_strcpy(d, s->calls[i]), yyjson_mut_bool(d, true)));
    for (size_t i = 0; i < s->queue_count; i++) {
        const push_event *e = &s->queue[i];
        yyjson_mut_val *v = yyjson_mut_obj(d);
        char created[40], next[40];
        qdc_rfc3339(e->created, created);
        qdc_rfc3339(e->next, next);
        ADD(v && yyjson_mut_arr_append(queue, v));
        ADD(yyjson_mut_obj_add_str(d, v, "id", e->id));
        ADD(yyjson_mut_obj_add_str(d, v, "kind", e->kind));
        if (e->count)
            ADD(yyjson_mut_obj_add_int(d, v, "count", e->count));
        if (e->call_id[0])
            ADD(yyjson_mut_obj_add_str(d, v, "call_id", e->call_id));
        if (e->call_uuid[0])
            ADD(yyjson_mut_obj_add_str(d, v, "call_uuid", e->call_uuid));
        ADD(yyjson_mut_obj_add_strcpy(d, v, "created", created));
        ADD(yyjson_mut_obj_add_strcpy(d, v, "next", next));
        ADD(yyjson_mut_obj_add_uint(d, v, "attempts", e->attempts));
    }
#undef ADD
    return d;
oom:
    yyjson_mut_doc_free(d);
    return NULL;
}
/* Caller owns mu. A rename with uncertain directory sync freezes the service:
 * the disk may already contain next, so no send/second mutation can race it. */
static int commit_state(push_service *p, push_state *next, qdc_error *error) {
    if (p->load_error) {
        qdc_error_set(error, "通知设置无法读取，原文件已保留");
        return -1;
    }
    yyjson_mut_doc *d = state_json(next);
    int rc = d ? qdc_save_json(p->path, d, error) : -1;
    if (d)
        yyjson_mut_doc_free(d);
    if (rc) {
        if (error && error->uncertain)
            p->load_error = true;
        snprintf(p->last_error, sizeof(p->last_error), "%s",
                 error && error->uncertain ? "通知保存结果未确认，请重启后核对"
                                           : "通知设置保存失败");
        if (!error || !error->uncertain)
            qdc_error_set(error, "%s", p->last_error);
        return -1;
    }
    state_free(&p->state);
    p->state = *next;
    memset(next, 0, sizeof(*next));
    return 0;
}

static int sender_random(void *context, unsigned char *p, size_t n) {
    (void)context;
    return qdc_random(p, n, NULL);
}
static bool apple_id(const char *s) {
    if (strlen(s) != 10)
        return false;
    for (size_t i = 0; i < 10; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9')))
            return false;
    return true;
}
static void sender_load(apns_sender *sender, const char *dir) {
    qdc_buf bytes = {0};
    qdc_error error = {0};
    char path[QDC_PATH_MAX], topic[80], environment[32];
    yyjson_doc *doc = NULL;
    if (qdc_path(path, sizeof(path), dir, "push-apns.json") ||
        qdc_private_file(path, 8192, &bytes, &error))
        goto done;
    doc = qdc_json_read(bytes.data, bytes.len, &error);
    qdc_buf_free(&bytes);
    if (!doc)
        goto done;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (read_string(root, "key_id", sender->key_id, sizeof(sender->key_id)) ||
        !apple_id(sender->key_id) ||
        read_string(root, "team_id", sender->team_id, sizeof(sender->team_id)) ||
        !apple_id(sender->team_id) || read_string(root, "topic", topic, sizeof(topic)) ||
        strcmp(topic, QDC_APNS_TOPIC) ||
        read_string(root, "environment", environment, sizeof(environment)) ||
        strcmp(environment, "development"))
        goto done;
    if (qdc_path(path, sizeof(path), dir, "push-apns.p8") ||
        qdc_private_file(path, 16384, &bytes, &error) || !bytes.len ||
        memchr(bytes.data, 0, bytes.len) ||
        !strstr((char *)bytes.data, "-----BEGIN PRIVATE KEY-----") ||
        !strstr((char *)bytes.data, "-----END PRIVATE KEY-----") ||
        mbedtls_pk_parse_key(&sender->key, bytes.data, bytes.len + 1, NULL, 0, sender_random,
                             NULL) ||
        !mbedtls_pk_can_do(&sender->key, MBEDTLS_PK_ECDSA) ||
        mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(sender->key)) != MBEDTLS_ECP_DP_SECP256R1 ||
        qdc_path(sender->ca_file, sizeof(sender->ca_file), dir, "push-ca.pem"))
        goto done;
    mbedtls_platform_zeroize(bytes.data, bytes.len);
    qdc_buf_free(&bytes);
    int ca_status = qdc_read_file(sender->ca_file, 2 * 1024 * 1024, &bytes, &error);
    if (ca_status < 0)
        goto done;
    if (ca_status == 0) {
        mbedtls_x509_crt ca;
        mbedtls_x509_crt_init(&ca);
        int parsed = bytes.len ? mbedtls_x509_crt_parse(&ca, bytes.data, bytes.len + 1) : -1;
        mbedtls_x509_crt_free(&ca);
        if (parsed != 0)
            goto done;
    }
    sender->configured = true;
done:
    if (bytes.data)
        mbedtls_platform_zeroize(bytes.data, bytes.len);
    qdc_buf_free(&bytes);
    if (doc)
        yyjson_doc_free(doc);
}
static int base64url(const void *data, size_t size, qdc_buf *out) {
    unsigned char encoded[2048];
    size_t n;
    if (size > 1024 || mbedtls_base64_encode(encoded, sizeof(encoded), &n, data, size))
        return -1;
    while (n && encoded[n - 1] == '=')
        n--;
    for (size_t i = 0; i < n; i++) {
        if (encoded[i] == '+')
            encoded[i] = '-';
        else if (encoded[i] == '/')
            encoded[i] = '_';
    }
    return qdc_buf_append(out, encoded, n);
}
static int provider_token(apns_sender *s, int64_t now, qdc_error *error) {
    if (s->jwt[0] && now >= s->token_at && now - s->token_at < 50 * 60 * 1000)
        return 0;
    qdc_buf token = {0};
    char header[96], claims[128];
    unsigned char digest[32], signature[MBEDTLS_PK_SIGNATURE_MAX_SIZE], raw[64];
    size_t length = 0, sequence;
    mbedtls_mpi r, v;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&v);
    int rc = -1;
    int hn = snprintf(header, sizeof(header), "{\"alg\":\"ES256\",\"kid\":\"%s\"}", s->key_id);
    int cn = snprintf(claims, sizeof(claims), "{\"iss\":\"%s\",\"iat\":%lld}", s->team_id,
                      (long long)(now / 1000));
    if (hn < 0 || cn < 0 || base64url(header, (size_t)hn, &token) || qdc_buf_puts(&token, ".") ||
        base64url(claims, (size_t)cn, &token) || qdc_sha256(token.data, token.len, digest) ||
        mbedtls_pk_sign(&s->key, MBEDTLS_MD_SHA256, digest, sizeof(digest), signature,
                        sizeof(signature), &length, sender_random, NULL))
        goto done;
    unsigned char *p = signature;
    const unsigned char *end = signature + length;
    if (mbedtls_asn1_get_tag(&p, end, &sequence,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) ||
        sequence != (size_t)(end - p) || mbedtls_asn1_get_mpi(&p, end, &r) ||
        mbedtls_asn1_get_mpi(&p, end, &v) || p != end || mbedtls_mpi_write_binary(&r, raw, 32) ||
        mbedtls_mpi_write_binary(&v, raw + 32, 32) || qdc_buf_puts(&token, ".") ||
        base64url(raw, sizeof(raw), &token) || token.len >= sizeof(s->jwt))
        goto done;
    memcpy(s->jwt, token.data, token.len + 1);
    s->token_at = now - now % 1000;
    rc = 0;
done:
    if (rc) {
        s->jwt[0] = 0;
        qdc_error_set(error, "苹果推送签名失败");
    }
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&v);
    qdc_buf_free(&token);
    mbedtls_platform_zeroize(raw, sizeof(raw));
    mbedtls_platform_zeroize(signature, sizeof(signature));
    return rc;
}
static void sync_date(push_service *p, const char *date) {
    int64_t now;
    qdc_error error = {0};
    if (date[0] && !qdc_http_date((qdc_slice){date, strlen(date)}, &now) &&
        !qdc_clock_sync(p->app, now, &error))
        p->sender.clock_refresh = qdc_mono_ms();
}
static void refresh_clock(push_service *p, uint64_t deadline) {
    if (p->sender.clock_refresh && qdc_mono_ms() - p->sender.clock_refresh < 600000)
        return;
    uint64_t now = qdc_mono_ms();
    if (now >= deadline)
        return;
    qdc_tls_request request = {.app = p->app,
                               .host = QDC_CLOCK_HOST,
                               .method = "GET",
                               .path = QDC_CLOCK_PATH,
                               .ca_file = p->sender.ca_file,
                               .timeout_ms =
                                   (unsigned)((deadline - now) < 3000 ? deadline - now : 3000)};
    qdc_tls_response response = {0};
    qdc_error error = {0};
    if (!qdc_tls_fetch(&request, &response, &error) && response.status == 200)
        sync_date(p, response.date);
    qdc_tls_response_free(&response);
}
static qdc_push_result sender_send(push_service *p, const qdc_push_delivery *delivery,
                                   qdc_error *error) {
    qdc_buf payload = {0};
    if (qdc_apns_payload(delivery, &payload, error))
        return QDC_PUSH_PERMANENT;
    apns_sender *s = &p->sender;
    qdc_push_result result = QDC_PUSH_RETRY;
    pthread_mutex_lock(&s->mu);
    if (!s->configured) {
        qdc_error_set(error, "苹果直连推送尚未配置");
        goto done;
    }
    if (qdc_now_ms(p->app) < 1704067200000LL) {
        qdc_error_set(error, "请打开 App 校准通知时间");
        goto done;
    }
    uint64_t deadline = qdc_mono_ms() + 12000;
    refresh_clock(p, deadline);
    int64_t now = qdc_now_ms(p->app);
    if (provider_token(s, now, error))
        goto done;
    char path[560], bearer[544], expiration[32];
    snprintf(path, sizeof(path), "/3/device/%s", delivery->token);
    snprintf(bearer, sizeof(bearer), "bearer %s", s->jwt);
    snprintf(expiration, sizeof(expiration), "%lld",
             (long long)(!strcmp(delivery->kind, "call") ? 0 : now / 1000 + 300));
    bool call = !strcmp(delivery->kind, "call");
    qdc_tls_header headers[] = {{"authorization", bearer},
                                {"content-type", "application/json"},
                                {"apns-topic", call ? QDC_APNS_TOPIC ".voip" : QDC_APNS_TOPIC},
                                {"apns-push-type", call ? "voip" : "alert"},
                                {"apns-priority", "10"},
                                {"apns-expiration", expiration},
                                {"apns-id", delivery->event_id},
                                {"apns-collapse-id", delivery->event_id}};
    uint64_t remaining = qdc_mono_ms();
    if (remaining >= deadline) {
        qdc_error_set(error, "苹果推送连接超时");
        goto done;
    }
    qdc_tls_request request = {.app = p->app,
                               .host = QDC_APNS_HOST,
                               .method = "POST",
                               .path = path,
                               .ca_file = s->ca_file,
                               .headers = headers,
                               .header_count = sizeof(headers) / sizeof(headers[0]),
                               .body = {(char *)payload.data, payload.len},
                               .timeout_ms = (unsigned)(deadline - remaining),
                               .require_h2 = true};
    qdc_tls_response response = {0};
    if (qdc_tls_fetch(&request, &response, error))
        goto done;
    sync_date(p, response.date);
    if (response.status == 200 && response.http2)
        result = QDC_PUSH_OK;
    else {
        char reason[128] = "";
        yyjson_doc *doc = yyjson_read((char *)response.body.data, response.body.len, 0);
        if (doc) {
            (void)read_string(yyjson_doc_get_root(doc), "reason", reason, sizeof(reason));
            yyjson_doc_free(doc);
        }
        if (!strcmp(reason, "BadDeviceToken") || !strcmp(reason, "DeviceTokenNotForTopic") ||
            !strcmp(reason, "Unregistered")) {
            result = QDC_PUSH_PERMANENT;
            qdc_error_set(error, "苹果推送令牌已失效，请重新打开 App 登记");
        } else if (!strcmp(reason, "ExpiredProviderToken")) {
            s->jwt[0] = 0;
            s->clock_refresh = 0;
            qdc_error_set(error, "苹果推送签名已过期，正在重新校准");
        } else if (!strcmp(reason, "InvalidProviderToken") ||
                   !strcmp(reason, "MissingProviderToken") || !strcmp(reason, "TopicDisallowed")) {
            result = QDC_PUSH_PERMANENT;
            qdc_error_set(error, "苹果拒绝推送签名，请检查专用密钥与 App 配置");
        } else if (response.status >= 400 && response.status < 500 && response.status != 429) {
            result = QDC_PUSH_PERMANENT;
            qdc_error_set(error, "苹果拒绝推送，请检查推送配置");
        } else
            qdc_error_set(error, "苹果推送暂未成功，将稍后重试");
    }
    qdc_tls_response_free(&response);
done:
    pthread_mutex_unlock(&s->mu);
    qdc_buf_free(&payload);
    return result;
}

int qdc_push_register(qdc_app *app, const qdc_push_subscription *subscription, yyjson_val *messages,
                      qdc_error *error) {
    push_service *p = app->push;
    if (!p || !subscription_valid(subscription)) {
        qdc_error_set(error, "缺少设备、SIM 或苹果推送令牌");
        return -1;
    }
    pthread_mutex_lock(&p->mu);
    int rc = -1;
    push_state next = {0};
    if (p->load_error) {
        qdc_error_set(error, "通知设置无法读取，原文件已保留");
        goto done;
    }
    if (subscription_equal(subscription, &p->state.subscription)) {
        rc = 0;
        goto done;
    }
    if (state_copy(&next, &p->state))
        goto done;
    bool changed = strcmp(subscription->iccid, next.subscription.iccid) ||
                   strcmp(subscription->device_id, next.subscription.device_id);
    if (changed) {
        free(next.calls);
        next.calls = NULL;
        next.call_count = 0;
        free(next.queue);
        next.queue = NULL;
        next.queue_count = 0;
    }
    if (changed || (!next.subscription.enabled && subscription->enabled)) {
        free(next.seen);
        next.seen = NULL;
        next.seen_count = 0;
        if (message_hashes(messages, &next.seen, &next.seen_count))
            goto done;
    }
    next.subscription = *subscription;
    for (size_t i = 0; i < next.queue_count;) {
        bool call = !strcmp(next.queue[i].kind, "call");
        if ((call && !subscription->calls_enabled) || (!call && !subscription->enabled))
            remove_event(&next, i);
        else
            i++;
    }
    rc = commit_state(p, &next, error);
    if (!rc)
        p->last_error[0] = 0;
done:
    state_free(&next);
    pthread_mutex_unlock(&p->mu);
    return rc;
}
void qdc_push_bind(qdc_app *app, const char *iccid) {
    push_service *p = app->push;
    if (!p || !iccid)
        return;
    pthread_mutex_lock(&p->mu);
    if (p->state.subscription.iccid[0] && strcmp(p->state.subscription.iccid, iccid)) {
        push_state next = {0};
        qdc_error error = {0};
        (void)commit_state(p, &next, &error);
        state_free(&next);
        snprintf(p->last_error, sizeof(p->last_error), "SIM 已更换，请重新开启通知");
    }
    pthread_mutex_unlock(&p->mu);
}
int qdc_push_observe(qdc_app *app, yyjson_val *messages, yyjson_val *calls, int64_t now,
                     qdc_error *error) {
    push_service *p = app->push;
    if (!p)
        return -1;
    pthread_mutex_lock(&p->mu);
    int rc = -1;
    push_state next = {0};
    const qdc_push_subscription *sub = &p->state.subscription;
    if (!sub->iccid[0] || (!sub->enabled && !sub->calls_enabled)) {
        rc = 0;
        goto done;
    }
    if (p->load_error || (messages && !yyjson_is_arr(messages)) || (calls && !yyjson_is_obj(calls)))
        goto done;
    next.subscription = *sub;
    if (message_hashes(messages, &next.seen, &next.seen_count))
        goto done;
    int count = 0;
    size_t i, max;
    yyjson_val *item;
    yyjson_arr_foreach(messages, i, max, item) {
        if (sub->enabled && qdc_slice_eq(qdc_json_slice(yyjson_obj_get(item, "direction")), "in")) {
            qdc_slice key = qdc_json_slice(yyjson_obj_get(item, "key"));
            unsigned char hash[32];
            if (qdc_sha256(key.ptr, key.len, hash))
                goto done;
            if (!hash_has(&p->state, hash))
                count++;
        }
    }
    yyjson_val *list = yyjson_obj_get(calls, "calls");
    if (list && (!yyjson_is_arr(list) || yyjson_arr_size(list) > PUSH_CALL_MAX))
        goto done;
    yyjson_arr_foreach(list, i, max, item) {
        char id[25], direction[8], state[24];
        if (read_string(item, "id", id, sizeof(id)) ||
            read_string(item, "direction", direction, sizeof(direction)) ||
            read_string(item, "state", state, sizeof(state)))
            goto done;
        if (!strcmp(direction, "in") && (!strcmp(state, "incoming") || !strcmp(state, "waiting"))) {
            bool duplicate = false;
            for (size_t j = 0; j < next.call_count; j++)
                if (!strcmp(next.calls[j], id))
                    duplicate = true;
            if (!hex_string(id, 24, 24) || append_call(&next, id))
                goto done;
            if (sub->calls_enabled && !call_has(&p->state, id) && !duplicate) {
                push_event event = {.created = now, .next = now};
                strcpy(event.kind, "call");
                strcpy(event.call_id, id);
                qdc_call_push_uuid(id, event.call_uuid);
                if (event_id(event.id, error) || append_event(&next, &event))
                    goto done;
            }
        }
    }
    bool uncertain = qdc_json_slice(yyjson_obj_get(calls, "error")).len != 0;
    if (uncertain) {
        if (!next.queue_count) {
            free(next.calls);
            next.calls = NULL;
            next.call_count = 0;
        }
        for (size_t j = 0; j < p->state.call_count; j++)
            if (append_call(&next, p->state.calls[j]))
                goto done;
    }
    if (next.call_count)
        qsort(next.calls, next.call_count, sizeof(push_call), call_compare);
    for (size_t j = 0; j < p->state.queue_count; j++) {
        const push_event *event = &p->state.queue[j];
        if (strcmp(event->kind, "call") || call_has(&next, event->call_id))
            if (append_event(&next, event))
                goto done;
    }
    if (count > 0 && next.queue_count < 32) {
        push_event event = {.count = count, .created = now, .next = now};
        strcpy(event.kind, "sms");
        if (event_id(event.id, error) || append_event(&next, &event))
            goto done;
    } else if (count > 0) {
        free(next.seen);
        next.seen = NULL;
        next.seen_count = p->state.seen_count;
        if (next.seen_count) {
            next.seen = malloc(next.seen_count * sizeof(push_hash));
            if (!next.seen)
                goto done;
            memcpy(next.seen, p->state.seen, next.seen_count * sizeof(push_hash));
        }
    }
    if (next.seen_count == p->state.seen_count && next.call_count == p->state.call_count &&
        next.queue_count == p->state.queue_count &&
        (!next.seen_count ||
         !memcmp(next.seen, p->state.seen, next.seen_count * sizeof(push_hash))) &&
        (!next.call_count ||
         !memcmp(next.calls, p->state.calls, next.call_count * sizeof(push_call))) &&
        (!next.queue_count ||
         !memcmp(next.queue, p->state.queue, next.queue_count * sizeof(push_event)))) {
        rc = 0;
        goto done;
    }
    rc = commit_state(p, &next, error);
done:
    state_free(&next);
    pthread_mutex_unlock(&p->mu);
    return rc;
}
void qdc_push_send_next(qdc_app *app, const char *iccid, int64_t now) {
    push_service *p = app->push;
    if (!p)
        return;
    pthread_mutex_lock(&p->mu);
    if (p->load_error || p->in_flight || !p->state.queue_count) {
        pthread_mutex_unlock(&p->mu);
        return;
    }
    if (!iccid || !iccid[0]) {
        snprintf(p->last_error, sizeof(p->last_error), "等待实时确认模块 SIM");
        pthread_mutex_unlock(&p->mu);
        return;
    }
    if (strcmp(iccid, p->state.subscription.iccid)) {
        pthread_mutex_unlock(&p->mu);
        qdc_push_bind(app, iccid);
        return;
    }
    push_event event = p->state.queue[0];
    if (now < event.next) {
        pthread_mutex_unlock(&p->mu);
        return;
    }
    qdc_error error = {0};
    push_state next = {0};
    if (now >= event.created &&
        now - event.created > (!strcmp(event.kind, "call") ? 20000 : 300000)) {
        if (!state_copy(&next, &p->state)) {
            remove_event(&next, 0);
            (void)commit_state(p, &next, &error);
        }
        state_free(&next);
        pthread_mutex_unlock(&p->mu);
        return;
    }
    qdc_push_delivery delivery = {.count = event.count, .sent_at = now / 1000};
    strcpy(delivery.kind, event.kind);
    strcpy(delivery.event_id, event.id);
    strcpy(delivery.call_id, event.call_id);
    strcpy(delivery.call_uuid, event.call_uuid);
    strcpy(delivery.token, !strcmp(event.kind, "call") ? p->state.subscription.voip_token
                                                       : p->state.subscription.token);
    p->in_flight = true;
#ifdef QDC_TESTING
    qdc_push_test_deliver_fn deliver = p->deliver;
    void *deliver_context = p->deliver_context;
#endif
    pthread_mutex_unlock(&p->mu);
    qdc_push_result result;
#ifdef QDC_TESTING
    result =
        deliver ? deliver(&delivery, deliver_context, &error) : sender_send(p, &delivery, &error);
#else
    result = sender_send(p, &delivery, &error);
#endif
    pthread_mutex_lock(&p->mu);
    p->in_flight = false;
    for (size_t i = 0; i < p->state.queue_count; i++)
        if (!strcmp(p->state.queue[i].id, event.id)) {
            if (state_copy(&next, &p->state))
                break;
            if (result == QDC_PUSH_OK || result == QDC_PUSH_PERMANENT || event.attempts >= 4)
                remove_event(&next, i);
            else {
                next.queue[i].attempts++;
                next.queue[i].next = now + (int64_t)(5U << event.attempts) * 1000;
            }
            if (result != QDC_PUSH_OK)
                snprintf(p->last_error, sizeof(p->last_error), "%s",
                         error.message[0] ? error.message : "苹果推送暂未成功，将稍后重试");
            qdc_error save_error = {0};
            if (!commit_state(p, &next, &save_error) && result == QDC_PUSH_OK) {
                p->accepted++;
                qdc_rfc3339(now, p->last_accepted);
                p->last_error[0] = 0;
            }
            state_free(&next);
            break;
        }
    pthread_mutex_unlock(&p->mu);
}
yyjson_mut_doc *qdc_push_status(qdc_app *app) {
    push_service *p = app->push;
    if (!p)
        return NULL;
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    bool ok = true;
    pthread_mutex_lock(&p->mu);
    const qdc_push_subscription *s = &p->state.subscription;
    ok &= yyjson_mut_obj_add_bool(d, root, "configured", p->sender.configured);
    ok &= yyjson_mut_obj_add_bool(d, root, "registered", s->iccid[0] != 0);
    ok &= yyjson_mut_obj_add_strcpy(d, root, "device_id", s->device_id);
    ok &= yyjson_mut_obj_add_bool(d, root, "sms_enabled", s->enabled);
    ok &= yyjson_mut_obj_add_bool(d, root, "calls_enabled", s->calls_enabled);
    ok &= yyjson_mut_obj_add_uint(d, root, "queued", p->state.queue_count);
    ok &= yyjson_mut_obj_add_uint(d, root, "accepted", p->accepted);
    ok &= yyjson_mut_obj_add_strcpy(d, root, "last_accepted", p->last_accepted);
    ok &= yyjson_mut_obj_add_strcpy(d, root, "error", p->last_error);
    pthread_mutex_unlock(&p->mu);
    if (!ok) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    return d;
}
static int enqueue_test(push_service *p, int64_t now, qdc_error *error) {
    for (size_t i = 0; i < p->state.queue_count; i++)
        if (!strcmp(p->state.queue[i].kind, "test"))
            return 0;
    push_state next = {0};
    int rc = -1;
    push_event event = {.created = now, .next = now + 8000};
    strcpy(event.kind, "test");
    if (!state_copy(&next, &p->state) && !event_id(event.id, error) && !append_event(&next, &event))
        rc = commit_state(p, &next, error);
    state_free(&next);
    return rc;
}
int qdc_push_route(qdc_app *app, qdc_request *request, qdc_response *response) {
    push_service *p = app->push;
    if (!p)
        return 0;
    if (!strcmp(request->path, "/api/push/status") && !strcmp(request->method, "GET")) {
        qdc_response_json(response, 200, qdc_push_status(app));
        return 1;
    }
    if (!strcmp(request->path, "/api/push/register") && !strcmp(request->method, "POST")) {
        qdc_error error = {0};
        yyjson_val *root = qdc_request_json(request, &error);
        qdc_push_subscription sub = {0};
        char expected[32], environment[32];
        if (!root || read_subscription(root, &sub) ||
            read_string(root, "expected_iccid", expected, sizeof(expected)) ||
            read_string(root, "environment", environment, sizeof(environment))) {
            qdc_response_error(response, 400, "通知设置格式错误");
            return 1;
        }
        if (strcmp(environment, "development")) {
            qdc_response_error(response, 400, "推送环境不匹配");
            return 1;
        }
        if ((sub.enabled || sub.calls_enabled) && !p->sender.configured) {
            qdc_response_error(response, 409, "后台推送服务尚未配置，前台提醒仍可使用");
            return 1;
        }
        pthread_mutex_lock(&app->op_mu);
        qdc_sim_identity sim;
        if (qdc_domain_identity(app, true, &sim, &error) || strcmp(sim.iccid, expected) ||
            !qdc_auth_check(app, request->cookie, sim.key)) {
            pthread_mutex_unlock(&app->op_mu);
            qdc_response_error(response, 409, "SIM 已变化，请重新连接后开启通知");
            return 1;
        }
        snprintf(sub.iccid, sizeof(sub.iccid), "%s", sim.iccid);
        yyjson_doc *messages = qdc_push_messages(app);
        int rc =
            messages ? qdc_push_register(app, &sub, yyjson_doc_get_root(messages), &error) : -1;
        if (messages)
            yyjson_doc_free(messages);
        pthread_mutex_unlock(&app->op_mu);
        if (rc)
            qdc_response_error(response, 400,
                               error.message[0] ? error.message : "通知设置保存失败");
        else
            qdc_response_json(response, 200, qdc_push_status(app));
        return 1;
    }
    if (!strcmp(request->path, "/api/push/test") && !strcmp(request->method, "POST")) {
        pthread_mutex_lock(&p->mu);
        if (!p->sender.configured || !p->state.subscription.enabled) {
            pthread_mutex_unlock(&p->mu);
            qdc_response_error(response, 409, "请先开启后台短信通知");
            return 1;
        }
        qdc_error error = {0};
        int rc = enqueue_test(p, qdc_now_ms(app), &error);
        pthread_mutex_unlock(&p->mu);
        if (rc)
            qdc_response_error(response, 500,
                               error.message[0] ? error.message : "通知设置保存失败");
        else {
            yyjson_mut_doc *d = qdc_json_object();
            if (d)
                yyjson_mut_obj_add_bool(d, yyjson_mut_doc_get_root(d), "queued", true);
            qdc_response_json(response, 200, d);
        }
        return 1;
    }
    return 0;
}
static void *push_worker(void *context) {
    push_service *p = context;
    qdc_app *app = p->app;
    while (!atomic_load(&p->stop) && !atomic_load(&app->stopping)) {
        pthread_mutex_lock(&p->mu);
        bool observing = p->state.subscription.iccid[0] &&
                         (p->state.subscription.enabled || p->state.subscription.calls_enabled);
        pthread_mutex_unlock(&p->mu);
        if (observing) {
            yyjson_doc *messages = qdc_push_messages(app), *calls = qdc_calls_snapshot(app);
            qdc_error error = {0};
            if (messages && calls)
                (void)qdc_push_observe(app, yyjson_doc_get_root(messages),
                                       yyjson_doc_get_root(calls), qdc_now_ms(app), &error);
            if (messages)
                yyjson_doc_free(messages);
            if (calls)
                yyjson_doc_free(calls);
        }
        pthread_mutex_lock(&p->mu);
        bool pending = p->state.queue_count > 0 && !p->load_error;
        pthread_mutex_unlock(&p->mu);
        if (pending) {
            qdc_sim_identity sim = {0};
            qdc_error error = {0};
            pthread_mutex_lock(&app->op_mu);
            if (qdc_domain_identity(app, true, &sim, &error))
                sim.iccid[0] = 0;
            pthread_mutex_unlock(&app->op_mu);
            qdc_push_send_next(app, sim.iccid, qdc_now_ms(app));
        }
        qdc_sleep(app, 1000);
    }
    return NULL;
}
int qdc_push_init(qdc_app *app, qdc_error *error) {
    push_service *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->app = app;
    atomic_init(&p->stop, false);
    mbedtls_pk_init(&p->sender.key);
    if (pthread_mutex_init(&p->mu, NULL)) {
        free(p);
        return -1;
    }
    if (pthread_mutex_init(&p->sender.mu, NULL)) {
        pthread_mutex_destroy(&p->mu);
        free(p);
        return -1;
    }
    if (qdc_path(p->path, sizeof(p->path), app->config.data_dir, "push-state.json")) {
        pthread_mutex_destroy(&p->sender.mu);
        pthread_mutex_destroy(&p->mu);
        free(p);
        return -1;
    }
    qdc_buf bytes = {0};
    qdc_error load_error = {0};
    int rc = qdc_read_file(p->path, 2 * 1024 * 1024, &bytes, &load_error);
    if (rc == 0) {
        yyjson_doc *doc = qdc_json_read(bytes.data, bytes.len, &load_error);
        if (!doc || state_read(&p->state, doc ? yyjson_doc_get_root(doc) : NULL)) {
            p->load_error = true;
            state_free(&p->state);
        }
        if (doc)
            yyjson_doc_free(doc);
    } else if (rc != 1)
        p->load_error = true;
    qdc_buf_free(&bytes);
    if (p->load_error)
        snprintf(p->last_error, sizeof(p->last_error), "通知设置无法读取，原文件已保留");
    sender_load(&p->sender, app->config.data_dir);
    app->push = p;
    (void)error;
    return 0;
}
int qdc_push_start(qdc_app *app, qdc_error *error) {
    push_service *p = app->push;
    if (!p || p->started)
        return p ? 0 : -1;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr))
        return -1;
    int rc = pthread_attr_setstacksize(&attr, 128 * 1024);
    if (!rc)
        rc = pthread_create(&p->worker, &attr, push_worker, p);
    pthread_attr_destroy(&attr);
    if (rc) {
        qdc_error_set(error, "通知服务启动失败");
        return -1;
    }
    p->started = true;
    return 0;
}
void qdc_push_free(qdc_app *app) {
    push_service *p = app->push;
    if (!p)
        return;
    atomic_store(&p->stop, true);
    if (p->started)
        pthread_join(p->worker, NULL);
    state_free(&p->state);
    mbedtls_pk_free(&p->sender.key);
    mbedtls_platform_zeroize(p->sender.jwt, sizeof(p->sender.jwt));
    pthread_mutex_destroy(&p->sender.mu);
    pthread_mutex_destroy(&p->mu);
    free(p);
    app->push = NULL;
}
#ifdef QDC_TESTING
void qdc_push_test_deliver(qdc_app *app, qdc_push_test_deliver_fn deliver, void *context) {
    push_service *p = app->push;
    pthread_mutex_lock(&p->mu);
    p->deliver = deliver;
    p->deliver_context = context;
    pthread_mutex_unlock(&p->mu);
}
int qdc_push_test_event(qdc_app *app, const char *kind, int64_t now, qdc_error *error) {
    push_service *p = app->push;
    push_state next = {0};
    push_event event = {.created = now, .next = now, .count = 1};
    int rc = -1;
    if (strcmp(kind, "sms") && strcmp(kind, "test"))
        return -1;
    snprintf(event.kind, sizeof(event.kind), "%s", kind);
    if (!strcmp(kind, "test"))
        event.next += 8000;
    pthread_mutex_lock(&p->mu);
    if (!state_copy(&next, &p->state) && !event_id(event.id, error) && !append_event(&next, &event))
        rc = commit_state(p, &next, error);
    state_free(&next);
    pthread_mutex_unlock(&p->mu);
    return rc;
}
int qdc_push_test_jwt(qdc_app *app, int64_t now, qdc_buf *out, qdc_error *error) {
    push_service *p = app->push;
    apns_sender *s = &p->sender;
    int rc = -1;
    pthread_mutex_lock(&s->mu);
    if (s->configured && !provider_token(s, now, error))
        rc = qdc_buf_puts(out, s->jwt);
    pthread_mutex_unlock(&s->mu);
    return rc;
}
#endif
