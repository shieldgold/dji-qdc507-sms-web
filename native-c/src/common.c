#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "qdc.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
void qdc_error_set(qdc_error *e, const char *fmt, ...) {
    if (!e)
        return;
    e->uncertain = false;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
}
void qdc_buf_free(qdc_buf *b) {
    if (b) {
        free(b->data);
        memset(b, 0, sizeof(*b));
    }
}
int qdc_buf_reserve(qdc_buf *b, size_t need) {
    if (!b || need == SIZE_MAX)
        return -1;
    if (need + 1 <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 128;
    while (cap < need + 1) {
        if (cap > SIZE_MAX / 2) {
            cap = need + 1;
            break;
        }
        cap *= 2;
    }
    unsigned char *p = realloc(b->data, cap);
    if (!p)
        return -1;
    b->data = p;
    b->cap = cap;
    b->data[b->len] = 0;
    return 0;
}
int qdc_buf_append(qdc_buf *b, const void *p, size_t n) {
    if (!b || (n && !p) || n > SIZE_MAX - b->len - 1 || qdc_buf_reserve(b, b->len + n))
        return -1;
    if (n)
        memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}
int qdc_buf_puts(qdc_buf *b, const char *s) {
    return s ? qdc_buf_append(b, s, strlen(s)) : -1;
}
int qdc_buf_printf(qdc_buf *b, const char *fmt, ...) {
    va_list ap, cp;
    va_start(ap, fmt);
    va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0 || (size_t)n > SIZE_MAX - b->len - 1 || qdc_buf_reserve(b, b->len + (size_t)n)) {
        va_end(ap);
        return -1;
    }
    vsnprintf((char *)b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}
qdc_slice qdc_json_slice(yyjson_val *v) {
    return yyjson_is_str(v) ? (qdc_slice){yyjson_get_str(v), yyjson_get_len(v)}
                            : (qdc_slice){"", 0};
}
bool qdc_slice_eq(qdc_slice s, const char *c) {
    return c && s.len == strlen(c) && (!s.len || !memcmp(s.ptr, c, s.len));
}
int qdc_json_cstr(yyjson_val *v, char *out, size_t cap) {
    if (!yyjson_is_str(v) || !out || !cap)
        return -1;
    qdc_slice s = qdc_json_slice(v);
    if (s.len >= cap || memchr(s.ptr, 0, s.len))
        return -1;
    memcpy(out, s.ptr, s.len);
    out[s.len] = 0;
    return 0;
}
static bool depth_ok(yyjson_val *v, unsigned depth) {
    if (depth > 128)
        return false;
    if (yyjson_is_arr(v)) {
        size_t i, max;
        yyjson_val *it;
        yyjson_arr_foreach(v, i, max, it) if (!depth_ok(it, depth + 1)) return false;
    }
    if (yyjson_is_obj(v)) {
        size_t i, max;
        yyjson_val *k, *it;
        yyjson_obj_foreach(v, i, max, k, it) if (!depth_ok(it, depth + 1)) return false;
    }
    return true;
}
static int folded_key(qdc_buf *out, qdc_slice key) {
    for (size_t i = 0; i < key.len; i++) {
        unsigned char c = (unsigned char)key.ptr[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + 32);
        else if (i + 1 < key.len && c == 0xc5 && (unsigned char)key.ptr[i + 1] == 0xbf) {
            c = 's';
            i++;
        } else if (i + 2 < key.len && c == 0xe2 && (unsigned char)key.ptr[i + 1] == 0x84 &&
                   (unsigned char)key.ptr[i + 2] == 0xaa) {
            c = 'k';
            i += 2;
        }
        if (qdc_buf_append(out, &c, 1))
            return -1;
    }
    return 0;
}
static bool needs_canonical(yyjson_val *v, bool fold) {
    size_t i, max;
    yyjson_val *key, *item;
    if (yyjson_is_obj(v)) {
        yyjson_obj_foreach(v, i, max, key, item) {
            qdc_slice k = qdc_json_slice(key);
            if (fold)
                for (size_t j = 0; j < k.len; j++) {
                    unsigned char c = (unsigned char)k.ptr[j];
                    if ((c >= 'A' && c <= 'Z') || c >= 128)
                        return true;
                }
            size_t j, other_max;
            yyjson_val *other, *ignored;
            yyjson_obj_foreach(v, j, other_max, other, ignored) {
                if (j >= i)
                    break;
                qdc_slice old = qdc_json_slice(other);
                if (k.len == old.len && (!k.len || !memcmp(k.ptr, old.ptr, k.len)))
                    return true;
            }
            if (needs_canonical(item, fold))
                return true;
        }
    } else if (yyjson_is_arr(v))
        yyjson_arr_foreach(v, i, max, item) if (needs_canonical(item, fold)) return true;
    return false;
}
static yyjson_mut_val *canonical(yyjson_mut_doc *d, yyjson_val *v, bool fold) {
    size_t i, max;
    yyjson_val *key, *item;
    if (yyjson_is_obj(v)) {
        yyjson_mut_val *obj = yyjson_mut_obj(d);
        if (!obj)
            return NULL;
        yyjson_obj_foreach(v, i, max, key, item) {
            qdc_slice k = qdc_json_slice(key);
            qdc_buf lower = {0};
            if (fold && folded_key(&lower, k)) {
                qdc_buf_free(&lower);
                return NULL;
            }
            const char *name = fold ? (lower.data ? (char *)lower.data : "") : k.ptr;
            size_t length = fold ? lower.len : k.len;
            yyjson_mut_val *previous = yyjson_mut_obj_getn(obj, name, length);
            /* encoding/json leaves an existing scalar unchanged for null. */
            if (fold && yyjson_is_null(item) && previous && !yyjson_mut_is_ctn(previous) &&
                !yyjson_mut_is_null(previous)) {
                qdc_buf_free(&lower);
                continue;
            }
            yyjson_mut_val *mk = yyjson_mut_strncpy(d, name, length),
                           *mv = canonical(d, item, fold);
            qdc_buf_free(&lower);
            if (!mk || !mv || !yyjson_mut_obj_put(obj, mk, mv))
                return NULL;
        }
        return obj;
    }
    if (yyjson_is_arr(v)) {
        yyjson_mut_val *arr = yyjson_mut_arr(d);
        if (!arr)
            return NULL;
        yyjson_arr_foreach(v, i, max, item) {
            yyjson_mut_val *value = canonical(d, item, fold);
            if (!value || !yyjson_mut_arr_add_val(arr, value))
                return NULL;
        }
        return arr;
    }
    return yyjson_val_mut_copy(d, v);
}
static yyjson_doc *canonical_doc(yyjson_doc *old, bool fold) {
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    if (!m)
        return NULL;
    yyjson_mut_val *root = canonical(m, yyjson_doc_get_root(old), fold);
    yyjson_doc *out = NULL;
    if (root) {
        yyjson_mut_doc_set_root(m, root);
        out = yyjson_mut_doc_imut_copy(m, NULL);
    }
    yyjson_mut_doc_free(m);
    return out;
}
yyjson_doc *qdc_json_read(const void *data, size_t n, qdc_error *e) {
    yyjson_read_err je;
    yyjson_doc *d = yyjson_read_opts((char *)data, n, 0, NULL, &je);
    if (!d) {
        qdc_error_set(e, "JSON 格式错误");
        return NULL;
    }
    if (!depth_ok(yyjson_doc_get_root(d), 0)) {
        yyjson_doc_free(d);
        qdc_error_set(e, "JSON 嵌套过深");
        return NULL;
    }
    if (needs_canonical(yyjson_doc_get_root(d), false)) {
        yyjson_doc *last = canonical_doc(d, false);
        yyjson_doc_free(d);
        d = last;
        if (!d)
            qdc_error_set(e, "JSON 内存不足");
    }
    return d;
}
static bool listed(qdc_slice key, const char *list) {
    for (const char *p = list; *p;) {
        const char *end = strchr(p, ',');
        if (!end)
            end = p + strlen(p);
        if (key.len == (size_t)(end - p) && (!key.len || !memcmp(key.ptr, p, key.len)))
            return true;
        p = *end ? end + 1 : end;
    }
    return false;
}
static bool request_fields(yyjson_val *obj, const char *strings, const char *booleans,
                           const char *numbers, bool restore, bool message) {
    if (!yyjson_is_obj(obj))
        return true;
    size_t i, max;
    yyjson_val *key, *v;
    yyjson_obj_foreach(obj, i, max, key, v) {
        qdc_buf folded = {0};
        if (folded_key(&folded, qdc_json_slice(key))) {
            qdc_buf_free(&folded);
            return false;
        }
        qdc_slice name = {(char *)folded.data, folded.len};
        bool ok = true;
        if (!yyjson_is_null(v)) {
            if (listed(name, strings))
                ok = yyjson_is_str(v);
            else if (listed(name, booleans))
                ok = yyjson_is_bool(v);
            else if (listed(name, numbers)) {
                ok = yyjson_is_int(v);
                if (ok) {
                    if (qdc_slice_eq(name, "unix_ms"))
                        ok = !yyjson_is_uint(v) || yyjson_get_uint(v) <= INT64_MAX;
                    else
                        ok = (!yyjson_is_uint(v) || yyjson_get_uint(v) <= INT_MAX) &&
                             yyjson_get_sint(v) >= INT_MIN;
                }
            } else if (restore && !message && qdc_slice_eq(name, "messages")) {
                ok = yyjson_is_arr(v);
                if (ok) {
                    size_t j, n;
                    yyjson_val *m;
                    yyjson_arr_foreach(v, j, n, m) {
                        if (!yyjson_is_null(m) &&
                            (!yyjson_is_obj(m) ||
                             !request_fields(m,
                                             "key,sender,body,time,status,direction,request_id,"
                                             "send_state,send_error,code,concat",
                                             "", "index,parts,sent_parts", false, true))) {
                            ok = false;
                            break;
                        }
                    }
                }
            }
        }
        qdc_buf_free(&folded);
        if (!ok)
            return false;
    }
    return true;
}
static bool request_types(qdc_request *r) {
    const char *p = r->path ? r->path : "", *s = "", *b = "", *n = "";
    bool restore = false;
    if (!strcmp(p, "/api/auth/setup")) {
        s = "password,confirm";
        b = "remember";
    } else if (!strcmp(p, "/api/auth/login")) {
        s = "password,expected_iccid";
        b = "remember";
    } else if (!strcmp(p, "/api/auth/change")) {
        s = "old,password,confirm";
        b = "remember";
    } else if (!strcmp(p, "/api/auth/browser/redeem"))
        s = "ticket";
    else if (!strcmp(p, "/api/send"))
        s = "request_id,number,text";
    else if (!strcmp(p, "/api/delete"))
        s = "key";
    else if (!strcmp(p, "/api/calls/dial"))
        s = "number,request_id";
    else if (!strncmp(p, "/api/calls/", 11))
        s = "id,digit";
    else if (!strcmp(p, "/api/sim/unlock"))
        s = "pin,request_id,expected_iccid";
    else if (!strcmp(p, "/api/sim/retry"))
        b = "confirmed";
    else if (!strcmp(p, "/api/push/register")) {
        s = "iccid,device_id,token,voip_token,expected_iccid,environment";
        b = "enabled,calls_enabled";
    } else if (!strcmp(p, "/api/time"))
        n = "unix_ms";
    else if (!strcmp(p, "/api/restore")) {
        s = "exported_at";
        n = "version";
        restore = true;
    }
    return request_fields(yyjson_doc_get_root(r->json), s, b, n, restore, false);
}
yyjson_val *qdc_request_json(qdc_request *r, qdc_error *e) {
    if (!r->json) {
        yyjson_read_err je;
        r->json = yyjson_read_opts((char *)r->body.ptr, r->body.len, 0, NULL, &je);
        if (r->json && !depth_ok(yyjson_doc_get_root(r->json), 0)) {
            yyjson_doc_free(r->json);
            r->json = NULL;
        }
        if (!r->json) {
            qdc_error_set(e, "JSON 格式错误");
            return NULL;
        }
        if (!request_types(r)) {
            yyjson_doc_free(r->json);
            r->json = NULL;
            qdc_error_set(e, "JSON 字段类型错误");
            return NULL;
        }
        /* Requests decode Go structs, whose ASCII field names are matched
         * case-insensitively. Data maps keep their case-sensitive keys. */
        if (r->json && needs_canonical(yyjson_doc_get_root(r->json), true)) {
            yyjson_doc *folded = canonical_doc(r->json, true);
            yyjson_doc_free(r->json);
            r->json = folded;
            if (!folded)
                qdc_error_set(e, "JSON 内存不足");
        }
    }
    return r->json ? yyjson_doc_get_root(r->json) : NULL;
}
yyjson_mut_doc *qdc_json_object(void) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d) {
        yyjson_mut_val *r = yyjson_mut_obj(d);
        if (!r) {
            yyjson_mut_doc_free(d);
            return NULL;
        }
        yyjson_mut_doc_set_root(d, r);
    }
    return d;
}
yyjson_mut_doc *qdc_json_array(void) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d) {
        yyjson_mut_val *r = yyjson_mut_arr(d);
        if (!r) {
            yyjson_mut_doc_free(d);
            return NULL;
        }
        yyjson_mut_doc_set_root(d, r);
    }
    return d;
}
static size_t valid_utf8(const unsigned char *p, size_t n) {
    if (!n)
        return 0;
    unsigned c = p[0];
    if (c < 128)
        return 1;
    if (c >= 0xc2 && c <= 0xdf && n >= 2 && (p[1] & 0xc0) == 0x80)
        return 2;
    if (c >= 0xe0 && c <= 0xef && n >= 3 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 &&
        !(c == 0xe0 && p[1] < 0xa0) && !(c == 0xed && p[1] >= 0xa0))
        return 3;
    if (c >= 0xf0 && c <= 0xf4 && n >= 4 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 &&
        (p[3] & 0xc0) == 0x80 && !(c == 0xf0 && p[1] < 0x90) && !(c == 0xf4 && p[1] >= 0x90))
        return 4;
    return 0;
}
yyjson_mut_val *qdc_json_string(yyjson_mut_doc *d, const void *raw, size_t n) {
    if (!raw && n)
        return NULL;
    const unsigned char *p = raw;
    size_t i = 0;
    bool valid = true;
    while (i < n) {
        size_t z = valid_utf8(p + i, n - i);
        if (!z) {
            valid = false;
            break;
        }
        i += z;
    }
    if (valid)
        return yyjson_mut_strncpy(d, raw ? raw : "", n);
    qdc_buf b = {0};
    i = 0;
    while (i < n) {
        size_t z = valid_utf8(p + i, n - i);
        if (z) {
            if (qdc_buf_append(&b, p + i, z))
                goto fail;
            i += z;
        } else {
            if (qdc_buf_append(&b, "\xef\xbf\xbd", 3))
                goto fail;
            i++;
        }
    }
    yyjson_mut_val *v = yyjson_mut_strncpy(d, (const char *)b.data, b.len);
    qdc_buf_free(&b);
    return v;
fail:
    qdc_buf_free(&b);
    return NULL;
}
bool qdc_json_add_string(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, const void *p,
                         size_t n) {
    yyjson_mut_val *k = yyjson_mut_strcpy(d, key), *v = qdc_json_string(d, p, n);
    return k && v && yyjson_mut_obj_add(obj, k, v);
}
int qdc_response_json(qdc_response *r, int status, yyjson_mut_doc *doc) {
    if (!doc)
        return -1;
    size_t n = 0;
    char *s = yyjson_mut_write(doc, 0, &n);
    yyjson_mut_doc_free(doc);
    if (!s)
        return -1;
    qdc_buf_free(&r->body);
    r->body = (qdc_buf){(unsigned char *)s, n, n + 1};
    r->static_body = NULL;
    r->static_len = 0;
    r->status = status;
    r->content_type = "application/json; charset=utf-8";
    return 0;
}
int qdc_response_error(qdc_response *r, int status, const char *s) {
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return -1;
    if (!qdc_json_add_string(d, yyjson_mut_doc_get_root(d), "error", s, strlen(s))) {
        yyjson_mut_doc_free(d);
        return -1;
    }
    return qdc_response_json(r, status, d);
}
int qdc_response_ok(qdc_response *r, int status) {
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return -1;
    if (!yyjson_mut_obj_add_bool(d, yyjson_mut_doc_get_root(d), "ok", true)) {
        yyjson_mut_doc_free(d);
        return -1;
    }
    return qdc_response_json(r, status, d);
}
int qdc_response_header(qdc_response *r, const char *key, const char *v) {
    if (r->header_count >= 16 || strlen(key) >= sizeof(r->headers[0].name) ||
        strlen(v) >= sizeof(r->headers[0].value) || strpbrk(key, "\r\n:") || strpbrk(v, "\r\n"))
        return -1;
    qdc_header *h = &r->headers[r->header_count++];
    strcpy(h->name, key);
    strcpy(h->value, v);
    return 0;
}
void qdc_response_free(qdc_response *r) {
    qdc_buf_free(&r->body);
    memset(r, 0, sizeof(*r));
}
static int hexval(unsigned c) {
    if (c >= '0' && c <= '9')
        return (int)c - '0';
    if (c >= 'a' && c <= 'f')
        return (int)c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return (int)c - 'A' + 10;
    return -1;
}
int qdc_query(const char *query, const char *key, qdc_buf *out) {
    if (!query)
        return 0;
    size_t kl = strlen(key);
    const char *p = query;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end)
            end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == kl && !memcmp(p, key, kl)) {
            for (p = eq + 1; p < end; p++) {
                unsigned char c = (unsigned char)*p;
                if (c == '+')
                    c = ' ';
                else if (c == '%') {
                    if (end - p < 3)
                        return -1;
                    int a = hexval((unsigned char)p[1]), b = hexval((unsigned char)p[2]);
                    if (a < 0 || b < 0)
                        return -1;
                    c = (unsigned char)(a * 16 + b);
                    p += 2;
                }
                if (qdc_buf_append(out, &c, 1))
                    return -1;
            }
            return 1;
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}
int qdc_cookie(const char *cookies, const char *name, char *out, size_t cap) {
    if (!cookies || !out || !cap)
        return 0;
    size_t nl = strlen(name);
    const char *p = cookies;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';')
            p++;
        const char *end = strchr(p, ';');
        if (!end)
            end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == nl && !memcmp(p, name, nl)) {
            const char *v = eq + 1;
            size_t n = (size_t)(end - v);
            while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
                n--;
            if (n >= cap)
                return -1;
            memcpy(out, v, n);
            out[n] = 0;
            return 1;
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}
uint64_t qdc_mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
int qdc_cond_init(pthread_cond_t *cond) {
#ifdef __APPLE__
    return pthread_cond_init(cond, NULL);
#else
    pthread_condattr_t attr;
    int rc = pthread_condattr_init(&attr);
    if (rc)
        return rc;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rc)
        rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
#endif
}
int qdc_cond_until(pthread_cond_t *cond, pthread_mutex_t *mu, uint64_t deadline) {
    uint64_t now = qdc_mono_ms();
    if (now >= deadline)
        return ETIMEDOUT;
#ifdef __APPLE__
    uint64_t left = deadline - now;
    struct timespec ts = {(time_t)(left / 1000), (long)(left % 1000) * 1000000};
    return pthread_cond_timedwait_relative_np(cond, mu, &ts);
#else
    struct timespec ts = {(time_t)(deadline / 1000), (long)(deadline % 1000) * 1000000};
    return pthread_cond_timedwait(cond, mu, &ts);
#endif
}
int64_t qdc_system_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void qdc_rfc3339(int64_t ms, char out[40]) {
    /* Keep the public clock range even with a 32-bit libc time_t. */
    int64_t seconds = ms / 1000;
    if (ms < 0 && ms % 1000)
        seconds--;
    int64_t days = seconds / 86400, rest = seconds % 86400;
    if (rest < 0) {
        days--;
        rest += 86400;
    }
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097),
             yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t year = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1, month = mp + (mp < 10 ? 3 : (unsigned)-9);
    year += month <= 2;
    if (year < 1 || year > 9999) {
        out[0] = 0;
        return;
    }
    snprintf(out, 40, "%04d-%02u-%02uT%02u:%02u:%02uZ", (int)year, month, day,
             (unsigned)(rest / 3600), (unsigned)(rest / 60 % 60), (unsigned)(rest % 60));
}
static int digits(const char *s, size_t n) {
    int x = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        x = x * 10 + s[i] - '0';
    }
    return x;
}
static int64_t civil_days(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yo = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yo * 365 + yo / 4 - yo / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}
int qdc_parse_rfc3339(qdc_slice s, int64_t *out) {
    if (!s.ptr || s.len < 20 || s.len > 40)
        return -1;
    const char *p = s.ptr;
    int y = digits(p, 4), mo = digits(p + 5, 2), d = digits(p + 8, 2), h = digits(p + 11, 2),
        mi = digits(p + 14, 2), se = digits(p + 17, 2);
    if (p[4] != '-' || p[7] != '-' || p[10] != 'T' || p[13] != ':' || p[16] != ':' || y < 1 ||
        mo < 1 || mo > 12 || d < 1 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59)
        return -1;
    static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max = md[mo - 1] + (mo == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    if (d > max)
        return -1;
    size_t i = 19;
    int frac = 0;
    if (i < s.len && (p[i] == '.' || p[i] == ',')) {
        i++;
        size_t start = i;
        for (; i < s.len && p[i] >= '0' && p[i] <= '9'; i++)
            if (i - start < 3)
                frac = frac * 10 + p[i] - '0';
        if (i == start)
            return -1;
        for (size_t n = i - start; n < 3; n++)
            frac *= 10;
    }
    int off = 0;
    if (i < s.len && p[i] == 'Z') {
        if (++i != s.len)
            return -1;
    } else {
        if (s.len - i != 6 || (p[i] != '+' && p[i] != '-') || p[i + 3] != ':')
            return -1;
        int oh = digits(p + i + 1, 2), om = digits(p + i + 4, 2);
        if (oh < 0 || oh > 23 || om < 0 || om > 59)
            return -1;
        off = (oh * 60 + om) * 60 * (p[i] == '+' ? 1 : -1);
        i = s.len;
    }
    if (i != s.len)
        return -1;
    *out =
        (civil_days(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + se - off) * 1000 +
        frac;
    return 0;
}
int qdc_path(char *out, size_t cap, const char *dir, const char *name) {
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
int qdc_mkdir_private(const char *path, qdc_error *e) {
    if (mkdir(path, 0700) && errno != EEXIST) {
        qdc_error_set(e, "无法创建私有数据目录");
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) || !S_ISDIR(st.st_mode) || chmod(path, 0700)) {
        qdc_error_set(e, "数据目录不可用");
        return -1;
    }
    return 0;
}
static int read_fd(int fd, size_t max, qdc_buf *b, qdc_error *e) {
    unsigned char scratch[4096];
    for (;;) {
        size_t want = max - b->len;
        if (want >= sizeof(scratch))
            want = sizeof(scratch);
        else
            want++;
        ssize_t n = read(fd, scratch, want);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            qdc_error_set(e, "文件读取失败");
            return -1;
        }
        if (!n)
            return 0;
        if ((size_t)n > max - b->len) {
            qdc_error_set(e, "文件超过大小限制");
            return -1;
        }
        if (qdc_buf_append(b, scratch, (size_t)n)) {
            qdc_error_set(e, "内存不足");
            return -1;
        }
    }
}
int qdc_read_file(const char *path, size_t max, qdc_buf *b, qdc_error *e) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return 1;
        qdc_error_set(e, "文件无法读取");
        return -1;
    }
    struct stat st;
    int rc = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        qdc_error_set(e, "文件类型无效");
        rc = -1;
    } else
        rc = read_fd(fd, max, b, e);
    close(fd);
    return rc;
}
int qdc_private_file(const char *path, size_t max, qdc_buf *b, qdc_error *e) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) {
        qdc_error_set(e, "私有文件不可用");
        return -1;
    }
    int rc;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 0077)) {
        qdc_error_set(e, "私有文件权限必须为 0600");
        rc = -1;
    } else
        rc = read_fd(fd, max, b, e);
    close(fd);
    return rc;
}
int qdc_atomic_write(const char *path, const void *p, size_t n, qdc_error *e) {
    if (e)
        memset(e, 0, sizeof(*e));
    char tmp[QDC_PATH_MAX], parent[QDC_PATH_MAX];
    if (strlen(path) >= sizeof(parent) ||
        snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        qdc_error_set(e, "文件路径过长");
        return -1;
    }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        if (slash == parent)
            slash[1] = 0;
        else
            *slash = 0;
    } else
        strcpy(parent, ".");
    /* Old Go builds used a fixed .tmp. Preserve a crash-left regular file for
     * recovery, without letting it permanently block future saves. Reject a
     * directory/link in that reserved location, and own only our unique file. */
    struct stat stale;
    if (!lstat(tmp, &stale)) {
        if (!S_ISREG(stale.st_mode) || stale.st_nlink != 1) {
            qdc_error_set(e, "临时文件类型无效");
            return -1;
        }
    } else if (errno != ENOENT) {
        qdc_error_set(e, "无法检查临时文件");
        return -1;
    }
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int)sizeof(tmp)) {
        qdc_error_set(e, "文件路径过长");
        return -1;
    }
    int fd = mkstemp(tmp);
    if (fd < 0) {
        qdc_error_set(e, "无法创建临时文件");
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC)) {
        close(fd);
        unlink(tmp);
        qdc_error_set(e, "临时文件设置失败");
        return -1;
    }
    if (fchmod(fd, 0600)) {
        close(fd);
        unlink(tmp);
        qdc_error_set(e, "临时文件权限设置失败");
        return -1;
    }
    const unsigned char *s = p;
    size_t off = 0;
    bool failed = false;
    while (off < n) {
        ssize_t z = write(fd, s + off, n - off);
        if (z < 0 && errno == EINTR)
            continue;
        if (z <= 0) {
            failed = true;
            break;
        }
        off += (size_t)z;
    }
    if (!failed && fsync(fd))
        failed = true;
    if (close(fd))
        failed = true;
    if (failed) {
        unlink(tmp);
        qdc_error_set(e, "文件保存失败");
        return -1;
    }
    if (rename(tmp, path)) {
        unlink(tmp);
        qdc_error_set(e, "文件替换失败");
        return -1;
    }
    int dfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0 || fsync(dfd)) {
        if (dfd >= 0)
            close(dfd);
        qdc_error_set(e, "文件已替换，目录同步结果未确认");
        if (e)
            e->uncertain = true;
        return -1;
    }
    close(dfd);
    return 0;
}
int qdc_save_json(const char *p, yyjson_mut_doc *d, qdc_error *e) {
    if (!d || !yyjson_mut_doc_get_root(d)) {
        qdc_error_set(e, "JSON 保存失败");
        return -1;
    }
    size_t n = 0;
    char *s = yyjson_mut_write(d, 0, &n);
    if (!s) {
        qdc_error_set(e, "JSON 保存失败");
        return -1;
    }
    int rc = qdc_atomic_write(p, s, n, e);
    free(s);
    return rc;
}
void qdc_log(const char *s) {
    fprintf(stderr, "smsweb-c: %s\n", s);
}
void qdc_sleep(qdc_app *a, unsigned ms) {
    while (ms && !atomic_load(&a->stopping)) {
        unsigned step = ms > 100 ? 100 : ms;
        struct timespec t = {step / 1000, (long)(step % 1000) * 1000000};
        while (nanosleep(&t, &t) && errno == EINTR) {
        }
        ms -= step;
    }
}
