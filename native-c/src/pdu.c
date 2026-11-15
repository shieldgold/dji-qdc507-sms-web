#include "qdc_domain.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t basic[128] = {
    '@',   0xa3, '$',  0xa5,  0xe8, 0xe9,  0xf9,  0xec,  0xf2,  0xc7,  '\n',  0xd8,  0xf8,
    '\r',  0xc5, 0xe5, 0x394, '_',  0x3a6, 0x393, 0x39b, 0x3a9, 0x3a0, 0x3a8, 0x3a3, 0x398,
    0x39e, 27,   0xc6, 0xe6,  0xdf, 0xc9,  ' ',   '!',   '"',   '#',   0xa4,  '%',   '&',
    '\'',  '(',  ')',  '*',   '+',  ',',   '-',   '.',   '/',   '0',   '1',   '2',   '3',
    '4',   '5',  '6',  '7',   '8',  '9',   ':',   ';',   '<',   '=',   '>',   '?',   0xa1,
    'A',   'B',  'C',  'D',   'E',  'F',   'G',   'H',   'I',   'J',   'K',   'L',   'M',
    'N',   'O',  'P',  'Q',   'R',  'S',   'T',   'U',   'V',   'W',   'X',   'Y',   'Z',
    0xc4,  0xd6, 0xd1, 0xdc,  0xa7, 0xbf,  'a',   'b',   'c',   'd',   'e',   'f',   'g',
    'h',   'i',  'j',  'k',   'l',  'm',   'n',   'o',   'p',   'q',   'r',   's',   't',
    'u',   'v',  'w',  'x',   'y',  'z',   0xe4,  0xf6,  0xf1,  0xfc,  0xe0};
static const unsigned ext_code[] = {10, 20, 40, 41, 47, 60, 61, 62, 64, 101};
static const uint32_t ext_rune[] = {'\f', '^', '{', '}', '\\', '[', '~', ']', '|', 0x20ac};
static uint32_t rune(qdc_slice s, size_t *i) {
    unsigned c = (unsigned char)s.ptr[(*i)++];
    if (c < 128)
        return c;
    unsigned n = c >= 0xc2 && c <= 0xdf   ? 1
                 : c >= 0xe0 && c <= 0xef ? 2
                 : c >= 0xf0 && c <= 0xf4 ? 3
                                          : 0;
    if (!n || *i + n > s.len)
        return 0xfffd;
    uint32_t v = c & ((1u << (6 - n)) - 1u);
    size_t start = *i;
    for (unsigned j = 0; j < n; j++) {
        unsigned d = (unsigned char)s.ptr[start + j];
        if ((d & 0xc0) != 0x80)
            return 0xfffd;
        v = (v << 6) | (d & 63);
    }
    if ((n == 2 && v < 0x800) || (n == 3 && v < 0x10000) || v > 0x10ffff ||
        (v >= 0xd800 && v <= 0xdfff))
        return 0xfffd;
    *i += n;
    return v;
}
static int utf8(qdc_buf *b, uint32_t r) {
    unsigned char u[4];
    size_t n = 0;
    if (r >= 0xd800 && r <= 0xdfff)
        r = 0xfffd;
    if (r < 128)
        u[n++] = (unsigned char)r;
    else if (r < 2048) {
        u[n++] = 0xc0 | (r >> 6);
        u[n++] = 0x80 | (r & 63);
    } else if (r < 65536) {
        u[n++] = 0xe0 | (r >> 12);
        u[n++] = 0x80 | ((r >> 6) & 63);
        u[n++] = 0x80 | (r & 63);
    } else {
        u[n++] = 0xf0 | (r >> 18);
        u[n++] = 0x80 | ((r >> 12) & 63);
        u[n++] = 0x80 | ((r >> 6) & 63);
        u[n++] = 0x80 | (r & 63);
    }
    return qdc_buf_append(b, u, n);
}
static bool whitespace(uint32_t r) {
    return r == 9 || r == 10 || r == 11 || r == 12 || r == 13 || r == 32 || r == 0x85 ||
           r == 0xa0 || r == 0x1680 || (r >= 0x2000 && r <= 0x200a) || r == 0x2028 || r == 0x2029 ||
           r == 0x202f || r == 0x205f || r == 0x3000;
}
qdc_slice qdc_trim(qdc_slice s) {
    size_t i = 0, start = 0, end = 0;
    bool seen = false;
    while (i < s.len) {
        size_t at = i;
        uint32_t r = rune(s, &i);
        if (!whitespace(r)) {
            if (!seen)
                start = at;
            end = i;
            seen = true;
        }
    }
    return seen ? (qdc_slice){s.ptr + start, end - start} : (qdc_slice){s.ptr, 0};
}
int qdc_request_id(qdc_slice s) {
    if (s.len < 8 || s.len > 80)
        return 0;
    for (size_t i = 0; i < s.len; i++) {
        unsigned c = (unsigned char)s.ptr[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}
static int gsm_code(uint32_t r, unsigned *a, unsigned *b) {
    for (unsigned i = 0; i < 128; i++)
        if (basic[i] == r) {
            *a = i;
            return 1;
        }
    for (unsigned i = 0; i < 10; i++)
        if (ext_rune[i] == r) {
            *a = 27;
            *b = ext_code[i];
            return 2;
        }
    return 0;
}
static int hexn(unsigned c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}
static int puthex(qdc_buf *b, const void *data, size_t n) {
    static const char h[] = "0123456789ABCDEF";
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) {
        char x[2] = {h[p[i] >> 4], h[p[i] & 15]};
        if (qdc_buf_append(b, x, 2))
            return -1;
    }
    return 0;
}
static int make_submit(qdc_slice digits, bool intl, const uint32_t *r, size_t nr, bool gsm,
                       unsigned ref, unsigned total, unsigned seq, qdc_buf *out) {
    unsigned char payload[141] = {0};
    unsigned units = 0, bit = 0, n = 0;
    if (total > 1) {
        unsigned char head[] = {
            5, 0, 3, (unsigned char)ref, (unsigned char)total, (unsigned char)seq};
        memcpy(payload, head, 6);
        if (gsm) {
            units = 7;
            bit = 49;
        } else
            n = 6;
    }
    for (size_t i = 0; i < nr; i++) {
        if (gsm) {
            unsigned a = 0, b = 0;
            int w = gsm_code(r[i], &a, &b);
            for (int j = 0; j < w; j++) {
                unsigned v = j ? b : a;
                for (unsigned k = 0; k < 7; k++, bit++)
                    if (v & (1u << k))
                        payload[bit / 8] |= 1u << (bit % 8);
                units++;
            }
        } else {
            uint32_t v = r[i];
            if (v > 0xffff) {
                v -= 0x10000;
                unsigned hi = 0xd800 + (v >> 10), lo = 0xdc00 + (v & 1023);
                payload[n++] = hi >> 8;
                payload[n++] = hi;
                payload[n++] = lo >> 8;
                payload[n++] = lo;
            } else {
                payload[n++] = v >> 8;
                payload[n++] = v;
            }
        }
    }
    if (gsm)
        n = (bit + 7) / 8;
    else
        units = n;
    if (qdc_buf_printf(out, "00%s00%02X%s", total > 1 ? "41" : "01", (unsigned)digits.len,
                       intl ? "91" : "81"))
        return -1;
    for (size_t i = 0; i < digits.len; i += 2) {
        char pair[2] = {i + 1 < digits.len ? digits.ptr[i + 1] : 'F', digits.ptr[i]};
        if (qdc_buf_append(out, pair, 2))
            return -1;
    }
    if (qdc_buf_printf(out, "00%s%02X", gsm ? "00" : "08", units) || puthex(out, payload, n))
        return -1;
    return 0;
}
void qdc_pdus_free(qdc_pdus *p) {
    if (!p)
        return;
    for (size_t i = 0; i < p->len; i++)
        qdc_buf_free(&p->items[i]);
    free(p->items);
    memset(p, 0, sizeof *p);
}
void qdc_part_free(qdc_part *p) {
    if (!p)
        return;
    qdc_buf_free(&p->sender);
    qdc_buf_free(&p->body);
    memset(p, 0, sizeof *p);
}
int qdc_pdu_submit(qdc_slice number, qdc_slice text, unsigned ref, qdc_pdus *out, qdc_error *e) {
    memset(out, 0, sizeof *out);
    if (!qdc_trim(text).len) {
        qdc_error_set(e, "短信内容为空");
        return -1;
    }
    number = qdc_trim(number);
    bool intl = number.len && number.ptr[0] == '+';
    qdc_slice digits = {number.ptr + intl, number.len - intl};
    for (size_t i = 0; i < digits.len; i++)
        if (digits.ptr[i] < '0' || digits.ptr[i] > '9') {
            qdc_error_set(e, "号码含非法字符");
            return -1;
        }
    if (digits.len > 20) {
        qdc_error_set(e, "号码最多 20 位数字");
        return -1;
    }
    if (!digits.len) {
        qdc_error_set(e, "号码为空");
        return -1;
    }
    uint32_t chars[1601];
    size_t nr = 0, i = 0;
    bool gsm = true;
    unsigned all = 0;
    while (i < text.len) {
        uint32_t r = rune(text, &i);
        if (nr == 1600) {
            qdc_error_set(e, "短信内容过长，请分成多条发送");
            return -1;
        }
        chars[nr++] = r;
        unsigned a, b;
        int w = gsm_code(r, &a, &b);
        if (!w)
            gsm = false;
        all += (unsigned)w;
    }
    unsigned units = 0;
    for (i = 0; i < nr; i++)
        units += gsm ? 0 : chars[i] > 0xffff ? 2 : 1;
    unsigned limit = gsm ? (all <= 160 ? 160 : 153) : (units <= 70 ? 70 : 67);
    size_t starts[11] = {0}, ends[10], count = 0, start = 0;
    unsigned used = 0;
    for (i = 0; i < nr; i++) {
        unsigned a, b;
        unsigned w = gsm ? (unsigned)gsm_code(chars[i], &a, &b) : chars[i] > 0xffff ? 2 : 1;
        if (used + w > limit && i > start) {
            if (count == 10)
                goto too_many;
            starts[count] = start;
            ends[count++] = i;
            start = i;
            used = 0;
        }
        used += w;
    }
    if (count == 10)
        goto too_many;
    starts[count] = start;
    ends[count++] = nr;
    out->items = calloc(count, sizeof *out->items);
    if (!out->items)
        goto oom;
    out->len = count;
    for (i = 0; i < count; i++)
        if (make_submit(digits, intl, chars + starts[i], ends[i] - starts[i], gsm, ref,
                        (unsigned)count, (unsigned)i + 1, &out->items[i]))
            goto oom;
    return 0;
too_many:
    qdc_error_set(e, "一条消息最多拆分为 10 条短信，请缩短内容");
    return -1;
oom:
    qdc_pdus_free(out);
    qdc_error_set(e, "内存不足");
    return -1;
}
static unsigned septet(const unsigned char *p, size_t n, size_t pos) {
    unsigned v = 0;
    for (unsigned i = 0; i < 7; i++)
        if ((pos + i) / 8 < n && (p[(pos + i) / 8] & (1u << ((pos + i) % 8))))
            v |= 1u << i;
    return v;
}
static int gsm_decode(qdc_buf *out, const unsigned char *p, size_t n, unsigned skip,
                      unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        unsigned v = septet(p, n, (skip + i) * 7);
        if (v == 27 && i + 1 < count) {
            v = septet(p, n, (skip + (++i)) * 7);
            for (unsigned j = 0; j < 10; j++)
                if (ext_code[j] == v && utf8(out, ext_rune[j]))
                    return -1;
        } else if (utf8(out, basic[v]))
            return -1;
    }
    return 0;
}
static int dec(unsigned x) {
    return (x & 15) * 10 + (x >> 4);
}
static int64_t civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yo = (unsigned)(y - era * 400), day = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5;
    return (int64_t)era * 146097 + yo * 365 + yo / 4 - yo / 100 + day + d - 1 - 719468;
}
static void civil_format(int64_t sec, char *out, size_t cap) {
    int64_t days = sec / 86400, rem = sec % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097),
             yo = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yo + (int)era * 400;
    unsigned dy = doe - (365 * yo + yo / 4 - yo / 100), mp = (5 * dy + 2) / 153,
             d = dy - (153 * mp + 2) / 5 + 1, m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    snprintf(out, cap, "%04d-%02u-%02u %02d:%02d:%02d", y, m, d, (int)(rem / 3600),
             (int)(rem % 3600 / 60), (int)(rem % 60));
}
static void timestamp(qdc_part *p, const unsigned char *b) {
    int yy = dec(b[0]), y = (yy >= 90 ? 1900 : 2000) + yy, m = dec(b[1]) - 1;
    y += m >= 0 ? m / 12 : (m - 11) / 12;
    m = (m % 12 + 12) % 12 + 1;
    int64_t raw = civil(y, m, dec(b[2])) * 86400 + dec(b[3]) * 3600 + dec(b[4]) * 60 + dec(b[5]);
    int quarters = (b[6] & 7) * 10 + ((b[6] >> 4) & 7);
    if (b[6] & 8)
        quarters = -quarters;
    p->timestamp = raw - quarters * 15 * 60;
    p->valid_time = true;
    civil_format(raw, p->local_time, sizeof p->local_time);
}
int qdc_pdu_deliver(qdc_slice hex, qdc_part *p, qdc_error *e) {
    memset(p, 0, sizeof *p);
    p->ref = -1;
    p->total = p->seq = 1;
    hex = qdc_trim(hex);
    size_t n = (hex.len + 1) / 2;
    unsigned char *data = calloc(n ? n : 1, 1);
    if (!data)
        goto oom;
    for (size_t i = 0; i < hex.len; i++) {
        int v = hexn((unsigned char)hex.ptr[i]);
        if (v < 0) {
            qdc_error_set(e, "PDU 十六进制格式错误");
            goto fail;
        }
        data[i / 2] |= (unsigned char)v << (i % 2 ? 0 : 4);
    }
    size_t at = 0;
#define TAKE(k)                                                                                    \
    do {                                                                                           \
        if ((k) > n - at) {                                                                        \
            qdc_error_set(e, "unexpected EOF");                                                    \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)
    TAKE(1);
    unsigned sca = data[at++];
    TAKE(sca);
    at += sca;
    TAKE(3);
    unsigned fo = data[at++], oa = data[at++], toa = data[at++];
    if ((fo & 3) != 0) {
        qdc_error_set(e, "不是 SMS-DELIVER");
        goto fail;
    }
    size_t olen = (oa + 1) / 2;
    TAKE(olen);
    if (((toa >> 4) & 7) == 5) {
        if (gsm_decode(&p->sender, data + at, olen, 0, oa))
            goto oom_fail;
    } else {
        static const char h[] = "0123456789ABCDEF";
        for (size_t j = 0; j < olen; j++) {
            unsigned v = data[at + j];
            if ((v & 15) != 15 && qdc_buf_append(&p->sender, h + (v & 15), 1))
                goto oom_fail;
            if ((v >> 4) != 15 && qdc_buf_append(&p->sender, h + (v >> 4), 1))
                goto oom_fail;
        }
    }
    at += olen;
    TAKE(10);
    at++;
    unsigned dcs = data[at++];
    timestamp(p, data + at);
    at += 7;
    unsigned udl = data[at++];
    unsigned char *ud = data + at;
    size_t un = n - at, head = 0;
    if ((fo & 64) && un && un >= 1 + (size_t)ud[0])
        head = 1 + (size_t)ud[0];
    if (head >= 6 && ud[1] == 0 && ud[2] == 3) {
        p->ref = ud[3];
        p->total = ud[4];
        p->seq = ud[5];
    } else if (head >= 7 && ud[1] == 8 && ud[2] == 4) {
        p->ref = (ud[3] << 8) | ud[4];
        p->total = ud[5];
        p->seq = ud[6];
    }
    if (((dcs >> 2) & 3) == 2) {
        size_t len = udl >= head ? udl - head : un - head;
        if (len > un - head)
            len = un - head;
        for (size_t j = 0; j + 1 < len; j += 2) {
            uint32_t r = (ud[head + j] << 8) | ud[head + j + 1];
            if (r >= 0xd800 && r < 0xdc00 && j + 3 < len) {
                unsigned lo = (ud[head + j + 2] << 8) | ud[head + j + 3];
                if (lo >= 0xdc00 && lo < 0xe000) {
                    r = 0x10000 + (r - 0xd800) * 1024 + lo - 0xdc00;
                    j += 2;
                }
            }
            if (utf8(&p->body, r))
                goto oom_fail;
        }
    } else if (((dcs >> 2) & 3) == 1) {
        size_t len = udl >= head ? udl - head : un - head;
        if (len > un - head)
            len = un - head;
        if (qdc_buf_append(&p->body, ud + head, len))
            goto oom_fail;
    } else {
        unsigned skip = (unsigned)(head * 8 + 6) / 7;
        if (gsm_decode(&p->body, ud, un, skip, udl > skip ? udl - skip : 0))
            goto oom_fail;
    }
    free(data);
    return 0;
oom_fail:
    qdc_error_set(e, "内存不足");
fail:
    free(data);
    qdc_part_free(p);
    return -1;
oom:
    qdc_error_set(e, "内存不足");
    return -1;
#undef TAKE
}
static bool boundary(unsigned char c) {
    return !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
             (c && strchr("./:_-", c)));
}
static size_t digits_run(qdc_slice s, size_t at) {
    size_t n = 0;
    while (at + n < s.len && s.ptr[at + n] >= '0' && s.ptr[at + n] <= '9')
        n++;
    return n;
}
static qdc_slice code(qdc_slice s) {
    static const char *words[] = {"验证码", "校验码", "动态码", "动态密码",
                                  "确认码", "安全码", "口令",   "code"};
    for (size_t at = 0; at < s.len; at++)
        for (size_t j = 0; j < 8; j++) {
            size_t w = strlen(words[j]);
            if (w > s.len - at || memcmp(s.ptr + at, words[j], w))
                continue;
            size_t p = at + w, gap = 0;
            while (p < s.len && gap <= 8) {
                size_t n = digits_run(s, p);
                if (n >= 4)
                    return (qdc_slice){s.ptr + p, n > 8 ? 8 : n};
                if (n || gap == 8)
                    break;
                (void)rune(s, &p);
                gap++;
            }
        }
    for (size_t at = 0; at < s.len; at++) {
        if (at && !boundary((unsigned char)s.ptr[at - 1]))
            continue;
        size_t n = digits_run(s, at);
        if (n >= 4 && n <= 8 && (at + n == s.len || boundary((unsigned char)s.ptr[at + n])))
            return (qdc_slice){s.ptr + at, n};
    }
    return (qdc_slice){"", 0};
}
static int seq_cmp(const void *a, const void *b) {
    qdc_part *const *x = a, *const *y = b;
    return ((*x)->seq > (*y)->seq) - ((*x)->seq < (*y)->seq);
}
yyjson_doc *qdc_merge_parts(qdc_part *parts, size_t n) {
    yyjson_mut_doc *doc = qdc_json_array();
    if (!doc)
        return NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    bool *seen = calloc(n ? n : 1, sizeof *seen);
    qdc_part **group = calloc(n ? n : 1, sizeof *group);
    if (!seen || !group)
        goto fail;
    for (size_t i = 0; i < n; i++) {
        if (seen[i])
            continue;
        size_t count = 0;
        group[count++] = &parts[i];
        seen[i] = true;
        if (parts[i].total > 1 && parts[i].ref >= 0)
            for (size_t j = i + 1; j < n; j++)
                if (!seen[j] && parts[j].ref == parts[i].ref && parts[j].total == parts[i].total &&
                    parts[j].sender.len == parts[i].sender.len &&
                    (!parts[i].sender.len ||
                     !memcmp(parts[j].sender.data, parts[i].sender.data, parts[i].sender.len))) {
                    seen[j] = true;
                    group[count++] = &parts[j];
                }
        qsort(group, count, sizeof *group, seq_cmp);
        qdc_buf body = {0}, key = {0};
        qdc_part *first = group[0], *earliest = first;
        for (size_t j = 0; j < count; j++) {
            if (qdc_buf_append(&body, group[j]->body.data, group[j]->body.len)) {
                qdc_buf_free(&body);
                goto fail;
            }
            if (group[j]->valid_time &&
                (!earliest->valid_time || group[j]->timestamp < earliest->timestamp))
                earliest = group[j];
        }
        char ts[24];
        if (earliest->valid_time)
            snprintf(ts, sizeof ts, "%s", earliest->local_time);
        else {
            time_t now = time(NULL);
            struct tm t;
            localtime_r(&now, &t);
            strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &t);
        }
        if (qdc_buf_append(&key, first->sender.data, first->sender.len) ||
            qdc_buf_printf(&key, "|%s|", ts)) {
            qdc_buf_free(&body);
            qdc_buf_free(&key);
            goto fail;
        }
        static const char hx[] = "0123456789abcdef";
        for (size_t j = 0; j < body.len; j++) {
            char h[2] = {hx[body.data[j] >> 4], hx[body.data[j] & 15]};
            if (qdc_buf_append(&key, h, 2)) {
                qdc_buf_free(&body);
                qdc_buf_free(&key);
                goto fail;
            }
        }
        yyjson_mut_val *m = yyjson_mut_obj(doc);
        const char *names[] = {"未读", "已读", "待发", "已发", "已收"}, *status = "";
        if (strlen(first->status) == 1 && first->status[0] >= '0' && first->status[0] <= '4')
            status = names[first->status[0] - '0'];
        bool ok = m && qdc_json_add_string(doc, m, "key", key.data, key.len) &&
                  yyjson_mut_obj_add_int(doc, m, "index", first->index) &&
                  qdc_json_add_string(doc, m, "sender", first->sender.data, first->sender.len) &&
                  qdc_json_add_string(doc, m, "body", body.data, body.len) &&
                  yyjson_mut_obj_add_strcpy(doc, m, "time", ts) &&
                  yyjson_mut_obj_add_strcpy(doc, m, "status", status) &&
                  yyjson_mut_obj_add_str(doc, m, "direction", "in");
        qdc_slice c = code((qdc_slice){(char *)body.data, body.len});
        if (c.len)
            ok = ok && qdc_json_add_string(doc, m, "code", c.ptr, c.len);
        qdc_buf_free(&body);
        qdc_buf_free(&key);
        if (!ok || !yyjson_mut_arr_append(root, m))
            goto fail;
    }
    free(seen);
    free(group);
    yyjson_doc *ret = yyjson_mut_doc_imut_copy(doc, NULL);
    yyjson_mut_doc_free(doc);
    return ret;
fail:
    free(seen);
    free(group);
    yyjson_mut_doc_free(doc);
    return NULL;
}
