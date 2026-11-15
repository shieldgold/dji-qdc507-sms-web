#include "qdc_auth.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTH_FILE_LIMIT (4U * 1024U * 1024U)
#define SESSION_SECONDS 43200LL
#define REMEMBER_SECONDS 31536000LL
#define PASSWORD_ITER 20000U
#define TOKEN_COOKIE "smsweb_token"

typedef struct auth_failure {
    char key[128];
    unsigned count;
    uint64_t until;
    struct auth_failure *next;
} auth_failure;
typedef struct {
    char ticket[65], parent[257], identity[128];
    uint64_t expires;
} auth_ticket;
typedef struct {
    pthread_mutex_t mu;
    char path[QDC_PATH_MAX];
    yyjson_mut_doc *state;
    bool load_error;
    auth_failure *failures;
    auth_ticket tickets[8];
    size_t ticket_count;
} auth_store;

static yyjson_mut_val *field(yyjson_mut_doc *d, const char *name) {
    return yyjson_mut_obj_get(yyjson_mut_doc_get_root(d), name);
}
static bool put(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, yyjson_mut_val *v) {
    yyjson_mut_val *k = yyjson_mut_strcpy(d, key);
    return k && v && yyjson_mut_obj_put(o, k, v);
}
static bool put_string(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, const char *v) {
    return put(d, o, key, yyjson_mut_strcpy(d, v ? v : ""));
}
static qdc_slice mutable_slice(yyjson_mut_val *v) {
    if (!yyjson_mut_is_str(v))
        return (qdc_slice){"", 0};
    return (qdc_slice){yyjson_mut_get_str(v), yyjson_mut_get_len(v)};
}
static bool slice_equal(qdc_slice a, qdc_slice b) {
    return a.len == b.len && (!a.len || !memcmp(a.ptr, b.ptr, a.len));
}
static int memory_error(qdc_error *e) {
    qdc_error_set(e, "内存不足，请稍后重试");
    return -1;
}
static yyjson_mut_doc *empty_auth(void) {
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return NULL;
    yyjson_mut_val *r = yyjson_mut_doc_get_root(d);
    if (!put(d, r, "accounts", yyjson_mut_obj(d)) || !put(d, r, "tokens", yyjson_mut_obj(d))) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    return d;
}
static bool valid_cstr(yyjson_val *v, size_t cap) {
    return yyjson_is_str(v) && yyjson_get_len(v) < cap &&
           !memchr(yyjson_get_str(v), 0, yyjson_get_len(v));
}
static bool json_integer(yyjson_val *v, int64_t *n) {
    if (yyjson_is_sint(v)) {
        *n = yyjson_get_sint(v);
        return true;
    }
    if (yyjson_is_uint(v) && yyjson_get_uint(v) <= INT64_MAX) {
        *n = (int64_t)yyjson_get_uint(v);
        return true;
    }
    return false;
}
static bool validate_state(yyjson_val *r) {
    if (!yyjson_is_obj(r) && !yyjson_is_null(r))
        return false;
    yyjson_val *accounts = yyjson_obj_get(r, "accounts"), *tokens = yyjson_obj_get(r, "tokens");
    yyjson_val *active = yyjson_obj_get(r, "active_identity");
    if (active && !yyjson_is_null(active) && !valid_cstr(active, 128))
        return false;
    if (accounts && !yyjson_is_obj(accounts) && !yyjson_is_null(accounts))
        return false;
    if (tokens && !yyjson_is_obj(tokens) && !yyjson_is_null(tokens))
        return false;
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(accounts, idx, max, k, v) {
        int64_t iter;
        if (!valid_cstr(k, 128) || !yyjson_is_obj(v) || !yyjson_is_str(yyjson_obj_get(v, "salt")) ||
            !yyjson_is_str(yyjson_obj_get(v, "hash")) ||
            !json_integer(yyjson_obj_get(v, "iter"), &iter) || iter < 1 || iter > 1000000)
            return false;
        yyjson_val *salt = yyjson_obj_get(v, "salt"), *hash = yyjson_obj_get(v, "hash");
        if (yyjson_get_len(salt) > 65536 || yyjson_get_len(hash) != 64)
            return false;
        const char *hs = yyjson_get_str(hash);
        for (size_t j = 0; j < 64; j++)
            if (!hs[j] || !strchr("0123456789abcdef", hs[j]))
                return false;
        const char *optional[] = {"identity", "imsi", "number", "createdAt"};
        for (size_t j = 0; j < sizeof(optional) / sizeof(optional[0]); j++) {
            yyjson_val *s = yyjson_obj_get(v, optional[j]);
            if (s && !yyjson_is_str(s) && !yyjson_is_null(s))
                return false;
        }
    }
    yyjson_obj_foreach(tokens, idx, max, k, v) {
        int64_t expires;
        if (!valid_cstr(k, 257) || !yyjson_is_obj(v) ||
            !valid_cstr(yyjson_obj_get(v, "identity"), 128) ||
            !json_integer(yyjson_obj_get(v, "expires"), &expires))
            return false;
        yyjson_val *remember = yyjson_obj_get(v, "remember");
        if (remember && !yyjson_is_bool(remember) && !yyjson_is_null(remember))
            return false;
    }
    return true;
}
static int64_t mut_integer(yyjson_mut_val *v) {
    return yyjson_mut_is_uint(v) ? (int64_t)yyjson_mut_get_uint(v) : yyjson_mut_get_sint(v);
}
static void prune_locked(auth_store *a) {
    int64_t now = qdc_system_ms() / 1000;
    yyjson_mut_obj_iter it = yyjson_mut_obj_iter_with(field(a->state, "tokens"));
    yyjson_mut_val *k;
    while ((k = yyjson_mut_obj_iter_next(&it))) {
        yyjson_mut_val *v = yyjson_mut_obj_iter_get_val(k);
        if (mut_integer(yyjson_mut_obj_get(v, "expires")) < now)
            yyjson_mut_obj_iter_remove(&it);
    }
}
int qdc_auth_init(qdc_app *app, qdc_error *e) {
    auth_store *a = calloc(1, sizeof(*a));
    if (!a)
        return memory_error(e);
    if (pthread_mutex_init(&a->mu, NULL)) {
        free(a);
        return memory_error(e);
    }
    if (qdc_path(a->path, sizeof(a->path), app->config.data_dir, "auth.json")) {
        pthread_mutex_destroy(&a->mu);
        free(a);
        qdc_error_set(e, "数据目录路径过长");
        return -1;
    }
    a->state = empty_auth();
    if (!a->state) {
        pthread_mutex_destroy(&a->mu);
        free(a);
        return memory_error(e);
    }
    qdc_buf b = {0};
    qdc_error read_error = {0};
    int rc = qdc_read_file(a->path, AUTH_FILE_LIMIT, &b, &read_error);
    if (rc == 0) {
        yyjson_doc *d = qdc_json_read(b.data, b.len, &read_error);
        if (!d || !validate_state(yyjson_doc_get_root(d)))
            a->load_error = true;
        else if (!yyjson_is_null(yyjson_doc_get_root(d))) {
            yyjson_mut_doc *m = yyjson_doc_mut_copy(d, NULL);
            if (!m) {
                yyjson_doc_free(d);
                qdc_buf_free(&b);
                qdc_auth_free(&(qdc_app){.auth = a});
                return memory_error(e);
            }
            yyjson_mut_doc_free(a->state);
            a->state = m;
            const char *names[] = {"accounts", "tokens"};
            for (size_t i = 0; i < 2; i++)
                if (!yyjson_mut_is_obj(field(m, names[i])))
                    put(m, yyjson_mut_doc_get_root(m), names[i], yyjson_mut_obj(m));
        }
        if (d)
            yyjson_doc_free(d);
    } else if (rc < 0)
        a->load_error = true;
    qdc_buf_free(&b);
    prune_locked(a);
    app->auth = a;
    return 0;
}
void qdc_auth_free(qdc_app *app) {
    auth_store *a = app ? app->auth : NULL;
    if (!a)
        return;
    auth_failure *f = a->failures;
    while (f) {
        auth_failure *next = f->next;
        free(f);
        f = next;
    }
    if (a->state)
        yyjson_mut_doc_free(a->state);
    pthread_mutex_destroy(&a->mu);
    memset(a, 0, sizeof(*a));
    free(a);
    app->auth = NULL;
}
static yyjson_mut_doc *candidate_locked(auth_store *a, qdc_error *e) {
    if (a->load_error) {
        qdc_error_set(e, "访问密码文件无法读取，已保留原文件，请从备份恢复");
        return NULL;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_mut_copy(a->state, NULL);
    if (!d)
        memory_error(e);
    return d;
}
static int commit_locked(auth_store *a, yyjson_mut_doc *d, qdc_error *e) {
    qdc_error failure = {0};
    if (qdc_save_json(a->path, d, &failure)) {
        yyjson_mut_doc_free(d);
        if (failure.uncertain)
            a->load_error = true;
        qdc_error_set(e, "访问设置未能保存，请检查存储空间");
        if (e)
            e->uncertain = failure.uncertain;
        return -1;
    }
    yyjson_mut_doc_free(a->state);
    a->state = d;
    return 0;
}
int qdc_auth_bind(qdc_app *app, const char *key, qdc_error *e) {
    auth_store *a = app->auth;
    if (!a) {
        qdc_error_set(e, "访问服务尚未就绪");
        return -1;
    }
    pthread_mutex_lock(&a->mu);
    qdc_slice old = mutable_slice(field(a->state, "active_identity"));
    if (qdc_slice_eq(old, key)) {
        pthread_mutex_unlock(&a->mu);
        return 0;
    }
    bool changed = old.len != 0;
    yyjson_mut_doc *d = candidate_locked(a, e);
    if (!d) {
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    yyjson_mut_val *r = yyjson_mut_doc_get_root(d);
    if (!put_string(d, r, "active_identity", key) ||
        (changed && !put(d, r, "tokens", yyjson_mut_obj(d)))) {
        yyjson_mut_doc_free(d);
        pthread_mutex_unlock(&a->mu);
        return memory_error(e);
    }
    int rc = commit_locked(a, d, e);
    if (!rc && changed) {
        memset(a->tickets, 0, sizeof(a->tickets));
        a->ticket_count = 0;
    }
    pthread_mutex_unlock(&a->mu);
    return rc;
}
bool qdc_auth_registered(qdc_app *app, const char *key) {
    auth_store *a = app->auth;
    if (!a)
        return false;
    pthread_mutex_lock(&a->mu);
    bool ok = yyjson_mut_obj_get(field(a->state, "accounts"), key) != NULL;
    pthread_mutex_unlock(&a->mu);
    return ok;
}
static bool token_valid_locked(auth_store *a, const char *token, const char *key) {
    if (a->load_error || !token || !*token)
        return false;
    yyjson_mut_val *t = yyjson_mut_obj_get(field(a->state, "tokens"), token);
    if (!t)
        return false;
    if (mut_integer(yyjson_mut_obj_get(t, "expires")) < qdc_system_ms() / 1000) {
        yyjson_mut_obj_remove_key(field(a->state, "tokens"), token);
        return false;
    }
    return qdc_slice_eq(mutable_slice(yyjson_mut_obj_get(t, "identity")), key);
}
bool qdc_auth_token_valid(qdc_app *app, const char *token, const char *key) {
    auth_store *a = app->auth;
    if (!a)
        return false;
    pthread_mutex_lock(&a->mu);
    bool ok = token_valid_locked(a, token, key);
    pthread_mutex_unlock(&a->mu);
    return ok;
}
bool qdc_auth_check(qdc_app *app, const char *cookie, const char *key) {
    char token[257];
    if (qdc_cookie(cookie, TOKEN_COOKIE, token, sizeof(token)) <= 0)
        return false;
    return qdc_auth_token_valid(app, token, key);
}
static size_t codepoints(qdc_slice s) {
    size_t n = 0;
    for (size_t i = 0; i < s.len; i++)
        if (((unsigned char)s.ptr[i] & 0xc0) != 0x80)
            n++;
    return n;
}
static int hash_password(qdc_slice pw, qdc_slice salt, unsigned iter, char out[65], qdc_error *e) {
    unsigned char dk[32];
    if (qdc_pbkdf2(pw, salt, iter, dk)) {
        qdc_error_set(e, "访问密码校验暂不可用");
        return -1;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = hex[dk[i] >> 4];
        out[i * 2 + 1] = hex[dk[i] & 15];
    }
    out[64] = 0;
    volatile unsigned char *wipe = dk;
    for (size_t i = 0; i < 32; i++)
        wipe[i] = 0;
    return 0;
}
static int new_token(yyjson_mut_doc *d, const char *key, bool remember, char out[65],
                     qdc_error *e) {
    if (qdc_random_hex(out, 32, e))
        return -1;
    yyjson_mut_val *t = yyjson_mut_obj(d);
    if (!t || !put_string(d, t, "identity", key) ||
        !put(d, t, "expires",
             yyjson_mut_sint(d, qdc_system_ms() / 1000 +
                                    (remember ? REMEMBER_SECONDS : SESSION_SECONDS))) ||
        !put(d, t, "remember", yyjson_mut_bool(d, remember)) ||
        !put(d, field(d, "tokens"), out, t)) {
        out[0] = 0;
        return memory_error(e);
    }
    return 0;
}
int qdc_auth_setup(qdc_app *app, const qdc_sim_identity *si, qdc_slice pw, bool remember,
                   char token[65], qdc_error *e) {
    token[0] = 0;
    if (codepoints(pw) < 4) {
        qdc_error_set(e, "密码至少 4 位");
        return -1;
    }
    if (pw.len > 64) {
        qdc_error_set(e, "密码太长");
        return -1;
    }
    auth_store *a = app->auth;
    if (!a)
        return memory_error(e);
    pthread_mutex_lock(&a->mu);
    if (yyjson_mut_obj_get(field(a->state, "accounts"), si->key)) {
        pthread_mutex_unlock(&a->mu);
        qdc_error_set(e, "该号码已经注册过密码");
        return -1;
    }
    yyjson_mut_doc *d = candidate_locked(a, e);
    if (!d) {
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    char salt[33], hash[65], created[32];
    time_t now = (time_t)(qdc_system_ms() / 1000);
    struct tm tm;
    if (qdc_random_hex(salt, 16, e) ||
        hash_password(pw, (qdc_slice){salt, 32}, PASSWORD_ITER, hash, e)) {
        yyjson_mut_doc_free(d);
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    if (!localtime_r(&now, &tm) || !strftime(created, sizeof(created), "%Y-%m-%d %H:%M:%S", &tm))
        created[0] = 0;
    yyjson_mut_val *acc = yyjson_mut_obj(d);
    bool ok = acc && put_string(d, acc, "identity", si->key) && put_string(d, acc, "salt", salt) &&
              put_string(d, acc, "hash", hash) &&
              put(d, acc, "iter", yyjson_mut_uint(d, PASSWORD_ITER)) &&
              put_string(d, acc, "createdAt", created);
    if (si->imsi[0])
        ok = ok && put_string(d, acc, "imsi", si->imsi);
    if (si->number[0])
        ok = ok && put_string(d, acc, "number", si->number);
    if (!ok || !put(d, field(d, "accounts"), si->key, acc)) {
        yyjson_mut_doc_free(d);
        pthread_mutex_unlock(&a->mu);
        return memory_error(e);
    }
    if (new_token(d, si->key, remember, token, e)) {
        yyjson_mut_doc_free(d);
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    int rc = commit_locked(a, d, e);
    if (rc)
        token[0] = 0;
    pthread_mutex_unlock(&a->mu);
    return rc;
}
static auth_failure *failure_locked(auth_store *a, const char *key, bool create) {
    auth_failure *f = a->failures;
    while (f && strcmp(f->key, key))
        f = f->next;
    if (!f && create) {
        f = calloc(1, sizeof(*f));
        if (f) {
            snprintf(f->key, sizeof(f->key), "%s", key);
            f->next = a->failures;
            a->failures = f;
        }
    }
    return f;
}
int qdc_auth_login(qdc_app *app, const char *key, qdc_slice pw, bool remember, char token[65],
                   qdc_error *e) {
    token[0] = 0;
    auth_store *a = app->auth;
    if (!a)
        return memory_error(e);
    pthread_mutex_lock(&a->mu);
    auth_failure *f = failure_locked(a, key, false);
    uint64_t now = qdc_mono_ms();
    if (f && f->until > now) {
        qdc_error_set(e, "密码错误次数过多，请 %llu 秒后再试",
                      (unsigned long long)((f->until - now) / 1000 + 1));
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    if (f && f->until) {
        f->count = 0;
        f->until = 0;
    }
    yyjson_mut_val *acc = yyjson_mut_obj_get(field(a->state, "accounts"), key);
    if (!acc) {
        pthread_mutex_unlock(&a->mu);
        qdc_error_set(e, "该号码尚未注册密码");
        return -1;
    }
    char hash[65];
    qdc_slice salt = mutable_slice(yyjson_mut_obj_get(acc, "salt"));
    unsigned iter = (unsigned)mut_integer(yyjson_mut_obj_get(acc, "iter"));
    if (hash_password(pw, salt, iter, hash, e)) {
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    qdc_slice expected = mutable_slice(yyjson_mut_obj_get(acc, "hash"));
    if (expected.len != 64 || !qdc_constant_equal(hash, expected.ptr, 64)) {
        f = failure_locked(a, key, true);
        if (!f) {
            pthread_mutex_unlock(&a->mu);
            return memory_error(e);
        }
        f->count++;
        if (f->count >= 5) {
            f->count = 0;
            f->until = qdc_mono_ms() + 60000;
            qdc_error_set(e, "密码错误次数过多，已锁定 60 秒");
        } else
            qdc_error_set(e, "密码错误（还可尝试 %u 次）", 5 - f->count);
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    if (f) {
        f->count = 0;
        f->until = 0;
    }
    yyjson_mut_doc *d = candidate_locked(a, e);
    if (!d) {
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    if (new_token(d, key, remember, token, e)) {
        yyjson_mut_doc_free(d);
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    int rc = commit_locked(a, d, e);
    if (rc)
        token[0] = 0;
    pthread_mutex_unlock(&a->mu);
    return rc;
}
int qdc_auth_logout(qdc_app *app, const char *token, qdc_error *e) {
    if (!token || !*token)
        return 0;
    auth_store *a = app->auth;
    if (!a)
        return memory_error(e);
    pthread_mutex_lock(&a->mu);
    yyjson_mut_doc *d = candidate_locked(a, e);
    if (!d) {
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    yyjson_mut_obj_remove_key(field(d, "tokens"), token);
    int rc = commit_locked(a, d, e);
    pthread_mutex_unlock(&a->mu);
    return rc;
}
int qdc_auth_issue(qdc_app *app, const char *key, bool remember, char token[65], qdc_error *e) {
    token[0] = 0;
    auth_store *a = app->auth;
    if (!a)
        return memory_error(e);
    pthread_mutex_lock(&a->mu);
    yyjson_mut_doc *d = candidate_locked(a, e);
    if (!d) {
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    if (new_token(d, key, remember, token, e)) {
        yyjson_mut_doc_free(d);
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    int rc = commit_locked(a, d, e);
    if (rc)
        token[0] = 0;
    pthread_mutex_unlock(&a->mu);
    return rc;
}
int qdc_auth_change(qdc_app *app, const char *key, qdc_slice old_pw, qdc_slice new_pw,
                    qdc_error *e) {
    if (new_pw.len > 64) {
        qdc_error_set(e, "密码太长");
        return -1;
    }
    if (codepoints(new_pw) < 4) {
        qdc_error_set(e, "新密码至少 4 位");
        return -1;
    }
    auth_store *a = app->auth;
    if (!a)
        return memory_error(e);
    pthread_mutex_lock(&a->mu);
    yyjson_mut_val *acc = yyjson_mut_obj_get(field(a->state, "accounts"), key);
    if (!acc) {
        pthread_mutex_unlock(&a->mu);
        qdc_error_set(e, "该号码尚未注册密码");
        return -1;
    }
    qdc_slice salt_slice = mutable_slice(yyjson_mut_obj_get(acc, "salt"));
    qdc_slice hash_slice = mutable_slice(yyjson_mut_obj_get(acc, "hash"));
    char *salt = malloc(salt_slice.len ? salt_slice.len : 1), old_hash[65];
    unsigned iter = (unsigned)mut_integer(yyjson_mut_obj_get(acc, "iter"));
    if (!salt || hash_slice.len != 64) {
        free(salt);
        pthread_mutex_unlock(&a->mu);
        return memory_error(e);
    }
    memcpy(salt, salt_slice.ptr, salt_slice.len);
    memcpy(old_hash, hash_slice.ptr, 64);
    old_hash[64] = 0;
    size_t salt_len = salt_slice.len;
    pthread_mutex_unlock(&a->mu);
    char h[65], new_salt[33], new_hash[65];
    int rc = hash_password(old_pw, (qdc_slice){salt, salt_len}, iter, h, e);
    free(salt);
    if (rc)
        return -1;
    if (!qdc_constant_equal(h, old_hash, 64)) {
        qdc_error_set(e, "原密码不正确");
        return -1;
    }
    if (qdc_random_hex(new_salt, 16, e) ||
        hash_password(new_pw, (qdc_slice){new_salt, 32}, PASSWORD_ITER, new_hash, e))
        return -1;
    pthread_mutex_lock(&a->mu);
    acc = yyjson_mut_obj_get(field(a->state, "accounts"), key);
    if (!acc) {
        pthread_mutex_unlock(&a->mu);
        qdc_error_set(e, "账号已不存在");
        return -1;
    }
    if (!qdc_slice_eq(mutable_slice(yyjson_mut_obj_get(acc, "hash")), old_hash)) {
        pthread_mutex_unlock(&a->mu);
        qdc_error_set(e, "密码已变更，请重新登录");
        return -1;
    }
    yyjson_mut_doc *d = candidate_locked(a, e);
    if (!d) {
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    acc = yyjson_mut_obj_get(field(d, "accounts"), key);
    if (!put_string(d, acc, "salt", new_salt) || !put_string(d, acc, "hash", new_hash) ||
        !put(d, acc, "iter", yyjson_mut_uint(d, PASSWORD_ITER)) ||
        !put(d, yyjson_mut_doc_get_root(d), "tokens", yyjson_mut_obj(d))) {
        yyjson_mut_doc_free(d);
        pthread_mutex_unlock(&a->mu);
        return memory_error(e);
    }
    rc = commit_locked(a, d, e);
    pthread_mutex_unlock(&a->mu);
    return rc;
}
static void remove_ticket(auth_store *a, size_t i) {
    if (i + 1 < a->ticket_count)
        memmove(&a->tickets[i], &a->tickets[i + 1],
                (a->ticket_count - i - 1) * sizeof(a->tickets[0]));
    a->ticket_count--;
    memset(&a->tickets[a->ticket_count], 0, sizeof(a->tickets[0]));
}
int qdc_auth_ticket(qdc_app *app, const char *parent, const char *key, char out[65], qdc_error *e) {
    out[0] = 0;
    auth_store *a = app->auth;
    if (!a)
        return memory_error(e);
    pthread_mutex_lock(&a->mu);
    if (!token_valid_locked(a, parent, key)) {
        pthread_mutex_unlock(&a->mu);
        qdc_error_set(e, "请先在 App 登录当前模块 SIM 卡");
        return -1;
    }
    uint64_t now = qdc_mono_ms();
    for (size_t i = 0; i < a->ticket_count;) {
        if (now > a->tickets[i].expires || !strcmp(a->tickets[i].parent, parent))
            remove_ticket(a, i);
        else
            i++;
    }
    if (a->ticket_count >= 8) {
        pthread_mutex_unlock(&a->mu);
        qdc_error_set(e, "免密请求过多，请稍后重试");
        return -1;
    }
    if (qdc_random_hex(out, 32, e)) {
        pthread_mutex_unlock(&a->mu);
        qdc_error_set(e, "暂时无法生成免密链接");
        return -1;
    }
    auth_ticket *t = &a->tickets[a->ticket_count++];
    memcpy(t->ticket, out, 65);
    snprintf(t->parent, sizeof(t->parent), "%s", parent);
    snprintf(t->identity, sizeof(t->identity), "%s", key);
    t->expires = qdc_mono_ms() + 30000;
    pthread_mutex_unlock(&a->mu);
    return 0;
}
int qdc_auth_redeem(qdc_app *app, const char *ticket, const char *key, char out[65], qdc_error *e) {
    out[0] = 0;
    auth_store *a = app->auth;
    if (!a)
        return memory_error(e);
    pthread_mutex_lock(&a->mu);
    auth_ticket found = {0};
    bool ok = false;
    for (size_t i = 0; i < a->ticket_count; i++)
        if (!strcmp(a->tickets[i].ticket, ticket)) {
            found = a->tickets[i];
            remove_ticket(a, i);
            ok = true;
            break;
        }
    if (!ok || qdc_mono_ms() > found.expires || strcmp(found.identity, key) ||
        !token_valid_locked(a, found.parent, key)) {
        pthread_mutex_unlock(&a->mu);
        qdc_error_set(e, "免密链接已过期或 SIM 已变化，请返回 App 重新打开");
        return -1;
    }
    yyjson_mut_doc *d = candidate_locked(a, e);
    if (!d) {
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    if (new_token(d, key, true, out, e)) {
        yyjson_mut_doc_free(d);
        pthread_mutex_unlock(&a->mu);
        return -1;
    }
    int rc = commit_locked(a, d, e);
    if (rc)
        out[0] = 0;
    pthread_mutex_unlock(&a->mu);
    return rc;
}
int qdc_auth_require(qdc_app *app, qdc_request *r, qdc_response *s) {
    qdc_sim_identity id = {0};
    qdc_error e = {0};
    if (!qdc_domain_identity(app, false, &id, &e) && id.key[0] && strcmp(id.key, "unknown") &&
        qdc_auth_check(app, r->cookie, id.key))
        return 0;
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return -1;
    yyjson_mut_val *o = yyjson_mut_doc_get_root(d);
    if (!put_string(d, o, "error", "未登录") || !put_string(d, o, "code", "unauthorized")) {
        yyjson_mut_doc_free(d);
        return -1;
    }
    return qdc_response_json(s, 401, d) ? -1 : 1;
}
static int result(int rc) {
    return rc ? -1 : 1;
}
static int error_response(qdc_response *s, int status, const char *message) {
    return result(qdc_response_error(s, status, message));
}
static yyjson_val *request_object(qdc_request *r, qdc_error *e) {
    yyjson_val *v = qdc_request_json(r, e);
    if (!v || (!yyjson_is_obj(v) && !yyjson_is_null(v))) {
        qdc_error_set(e, "请求格式错误");
        return NULL;
    }
    return v;
}
static bool request_string(yyjson_val *o, const char *key, qdc_slice *out) {
    yyjson_val *v = yyjson_obj_get(o, key);
    if (!v || yyjson_is_null(v)) {
        *out = (qdc_slice){"", 0};
        return true;
    }
    if (!yyjson_is_str(v))
        return false;
    *out = qdc_json_slice(v);
    return true;
}
static bool request_bool(yyjson_val *o, const char *key, bool *out) {
    yyjson_val *v = yyjson_obj_get(o, key);
    *out = false;
    if (!v || yyjson_is_null(v))
        return true;
    if (!yyjson_is_bool(v))
        return false;
    *out = yyjson_get_bool(v);
    return true;
}
static int set_cookie(qdc_response *s, const char *token, bool remember) {
    char c[160];
    int n = snprintf(c, sizeof(c), TOKEN_COOKIE "=%s; Path=/; HttpOnly; SameSite=Lax%s", token,
                     remember ? "; Max-Age=31536000" : "");
    return n < 0 || (size_t)n >= sizeof(c) ? -1 : qdc_response_header(s, "Set-Cookie", c);
}
static int ok_remember(qdc_response *s, bool remember) {
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return -1;
    yyjson_mut_val *o = yyjson_mut_doc_get_root(d);
    if (!put(d, o, "ok", yyjson_mut_bool(d, true)) ||
        !put(d, o, "remember", yyjson_mut_bool(d, remember))) {
        yyjson_mut_doc_free(d);
        return -1;
    }
    return result(qdc_response_json(s, 200, d));
}
static int auth_status(qdc_app *app, qdc_request *r, qdc_response *s) {
    qdc_error e = {0};
    qdc_sim_identity id = {0};
    pthread_mutex_lock(&app->op_mu);
    if (qdc_domain_identity(app, true, &id, &e)) {
        pthread_mutex_unlock(&app->op_mu);
        return error_response(s, 503, e.message);
    }
    yyjson_doc *pin = qdc_pin_snapshot(app);
    yyjson_mut_doc *d = qdc_json_object();
    bool ok = d && pin;
    if (ok) {
        yyjson_mut_val *o = yyjson_mut_doc_get_root(d);
        ok = put_string(d, o, "iccid", id.iccid) && put_string(d, o, "imsi", id.imsi) &&
             put_string(d, o, "number", id.number) &&
             put(d, o, "registered", yyjson_mut_bool(d, qdc_auth_registered(app, id.key))) &&
             put(d, o, "authenticated",
                 yyjson_mut_bool(d, qdc_auth_check(app, r->cookie, id.key))) &&
             put(d, o, "minPassword", yyjson_mut_uint(d, 4)) &&
             put(d, o, "sim", yyjson_val_mut_copy(d, yyjson_doc_get_root(pin)));
    }
    if (pin)
        yyjson_doc_free(pin);
    pthread_mutex_unlock(&app->op_mu);
    if (!ok) {
        if (d)
            yyjson_mut_doc_free(d);
        return -1;
    }
    return result(qdc_response_json(s, 200, d));
}
static int auth_setup(qdc_app *app, qdc_request *r, qdc_response *s) {
    qdc_error e = {0};
    qdc_slice pw, confirm;
    bool remember;
    yyjson_val *v = request_object(r, &e);
    if (!v || !request_string(v, "password", &pw) || !request_string(v, "confirm", &confirm) ||
        !request_bool(v, "remember", &remember))
        return error_response(s, 400, "请求格式错误");
    if (!slice_equal(pw, confirm))
        return error_response(s, 400, "两次输入的密码不一致");
    qdc_sim_identity id = {0};
    if (qdc_domain_identity(app, false, &id, &e) || !id.key[0] || !strcmp(id.key, "unknown"))
        return error_response(s, 400, "尚未识别 SIM 卡，请稍后重试");
    char token[65];
    if (qdc_auth_setup(app, &id, pw, remember, token, &e))
        return error_response(s, 400, e.message);
    if (set_cookie(s, token, remember))
        return -1;
    return ok_remember(s, remember);
}
static int auth_login(qdc_app *app, qdc_request *r, qdc_response *s) {
    qdc_error e = {0};
    qdc_slice pw, expected;
    bool remember;
    yyjson_val *v = request_object(r, &e);
    if (!v || !request_string(v, "password", &pw) ||
        !request_string(v, "expected_iccid", &expected) || !request_bool(v, "remember", &remember))
        return error_response(s, 400, "请求格式错误");
    qdc_sim_identity id = {0};
    if (expected.len)
        pthread_mutex_lock(&app->op_mu);
    int rc = qdc_domain_identity(app, expected.len != 0, &id, &e);
    if (expected.len && (rc || !qdc_slice_eq(expected, id.iccid))) {
        pthread_mutex_unlock(&app->op_mu);
        return error_response(s, 409, "SIM 卡已变化或无法确认，请重新输入该卡的访问密码");
    }
    char token[65];
    rc = qdc_auth_login(app, id.key, pw, remember, token, &e);
    if (expected.len)
        pthread_mutex_unlock(&app->op_mu);
    if (rc)
        return error_response(s, 401, e.message);
    if (set_cookie(s, token, remember))
        return -1;
    return ok_remember(s, remember);
}
static int auth_logout(qdc_app *app, qdc_request *r, qdc_response *s) {
    char token[257] = {0};
    qdc_cookie(r->cookie, TOKEN_COOKIE, token, sizeof(token));
    qdc_error e = {0};
    if (qdc_auth_logout(app, token, &e))
        return error_response(s, 500, e.message);
    if (qdc_response_header(s, "Set-Cookie",
                            TOKEN_COOKIE "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0"))
        return -1;
    return result(qdc_response_ok(s, 200));
}
static int auth_change(qdc_app *app, qdc_request *r, qdc_response *s) {
    qdc_error e = {0};
    qdc_slice old, pw, confirm;
    bool remember;
    yyjson_val *v = request_object(r, &e);
    if (!v || !request_string(v, "old", &old) || !request_string(v, "password", &pw) ||
        !request_string(v, "confirm", &confirm) || !request_bool(v, "remember", &remember))
        return error_response(s, 400, "请求格式错误");
    if (!slice_equal(pw, confirm))
        return error_response(s, 400, "两次输入的新密码不一致");
    qdc_sim_identity id = {0};
    if (qdc_domain_identity(app, false, &id, &e))
        return error_response(s, 400, "该号码尚未注册密码");
    if (qdc_auth_change(app, id.key, old, pw, &e))
        return error_response(s, 400, e.message);
    char token[65];
    if (qdc_auth_issue(app, id.key, remember, token, &e))
        return error_response(s, 500, "密码已修改，请重新登录");
    if (set_cookie(s, token, remember))
        return -1;
    return result(qdc_response_ok(s, 200));
}
static int browser_route(qdc_app *app, qdc_request *r, qdc_response *s, bool redeem) {
    qdc_error e = {0};
    char input[257] = {0};
    qdc_slice ts = {"", 0};
    if (redeem) {
        yyjson_val *v = request_object(r, &e);
        if (!v || !request_string(v, "ticket", &ts) || ts.len != 64)
            return error_response(s, 400, "无效的免密链接，请返回 App 重新打开");
        memcpy(input, ts.ptr, 64);
    } else
        qdc_cookie(r->cookie, TOKEN_COOKIE, input, sizeof(input));
    qdc_sim_identity id = {0};
    pthread_mutex_lock(&app->op_mu);
    if (qdc_domain_identity(app, true, &id, &e)) {
        pthread_mutex_unlock(&app->op_mu);
        return error_response(s, 409, e.message);
    }
    char output[65];
    int rc = redeem ? qdc_auth_redeem(app, input, id.key, output, &e)
                    : qdc_auth_ticket(app, input, id.key, output, &e);
    pthread_mutex_unlock(&app->op_mu);
    if (rc)
        return error_response(s, 401, e.message);
    if (redeem) {
        if (set_cookie(s, output, true))
            return -1;
        return ok_remember(s, true);
    }
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return -1;
    yyjson_mut_val *o = yyjson_mut_doc_get_root(d);
    if (!put_string(d, o, "ticket", output) || !put(d, o, "expires_in", yyjson_mut_uint(d, 30))) {
        yyjson_mut_doc_free(d);
        return -1;
    }
    return result(qdc_response_json(s, 200, d));
}
int qdc_auth_route(qdc_app *app, qdc_request *r, qdc_response *s) {
    if (!strcmp(r->method, "GET") && !strcmp(r->path, "/api/auth/status"))
        return auth_status(app, r, s);
    if (strcmp(r->method, "POST"))
        return 0;
    if (!strcmp(r->path, "/api/auth/setup"))
        return auth_setup(app, r, s);
    if (!strcmp(r->path, "/api/auth/login"))
        return auth_login(app, r, s);
    if (!strcmp(r->path, "/api/auth/logout"))
        return auth_logout(app, r, s);
    if (!strcmp(r->path, "/api/auth/change"))
        return auth_change(app, r, s);
    if (!strcmp(r->path, "/api/auth/browser/ticket"))
        return browser_route(app, r, s, false);
    if (!strcmp(r->path, "/api/auth/browser/redeem"))
        return browser_route(app, r, s, true);
    if (!strcmp(r->path, "/api/time"))
        return qdc_clock_route(app, r, s);
    return 0;
}
