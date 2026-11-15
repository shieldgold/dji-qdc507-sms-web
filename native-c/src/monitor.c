#include "qdc_monitor.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

typedef struct {
    pthread_mutex_t mu, refresh_mu;
    qdc_app *app;
    qdc_network network;
    char firmware[256];
    qdc_traffic *traffic;
    pthread_t thread;
    bool started;
} monitor;

static char *trim(char *p) {
    while (isspace((unsigned char)*p))
        p++;
    char *end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1]))
        *--end = 0;
    return p;
}
static bool copy(char *out, size_t cap, const char *s) {
    size_t n = strlen(s);
    if (n >= cap)
        return false;
    memcpy(out, s, n + 1);
    return true;
}
/* Small CSV reader for one modem record. Quoted commas/doubled quotes are
 * supported; malformed or oversized telemetry is rejected, never truncated. */
static int csv(const char *input, qdc_buf fields[32], size_t *count) {
    const char *p = input;
    *count = 0;
    while (*count < 32) {
        while (*p == ' ' || *p == '\t')
            p++;
        qdc_buf *f = &fields[*count];
        bool quoted = *p == '"';
        if (quoted)
            p++;
        while (*p) {
            if (quoted && *p == '"') {
                p++;
                if (*p == '"') {
                    if (qdc_buf_append(f, p++, 1))
                        return -1;
                } else {
                    quoted = false;
                    while (*p == ' ' || *p == '\t')
                        p++;
                    if (*p && *p != ',')
                        return -1;
                    break;
                }
            } else if (!quoted && *p == ',')
                break;
            else {
                if (!quoted && *p == '"')
                    return -1;
                if (qdc_buf_append(f, p++, 1))
                    return -1;
            }
        }
        if (quoted)
            return -1;
        if (!f->data && qdc_buf_append(f, "", 0))
            return -1;
        char *clean = trim((char *)f->data);
        size_t n = strlen(clean);
        memmove(f->data, clean, n + 1);
        f->len = n;
        (*count)++;
        if (!*p)
            return 0;
        if (*p++ != ',')
            return -1;
    }
    return -1;
}
int qdc_network_apply(qdc_network *n, const char *line) {
    const char *colon = strchr(line, ':');
    if (!colon)
        return 0;
    size_t prefix = (size_t)(colon - line);
    qdc_buf f[32] = {{0}};
    size_t count = 0;
    int rc = csv(colon + 1, f, &count);
    if (rc)
        goto done;
#define FIELD(i) ((char *)f[(i)].data)
#define SET(member, value)                                                                         \
    do {                                                                                           \
        if (!copy(n->member, sizeof(n->member), (value))) {                                        \
            rc = -1;                                                                               \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
    if (prefix == 8 && !memcmp(line, "+QNWINFO", 8)) {
        if (count >= 4) {
            SET(technology, FIELD(0));
            SET(operator_name, FIELD(1));
            SET(band, FIELD(2));
            SET(channel, FIELD(3));
        }
    } else if (prefix == 6 && !memcmp(line, "+CEREG", 6)) {
        if (count >= 2) {
            const char *s = FIELD(1);
            n->registered = !strcmp(s, "1") || !strcmp(s, "5");
            const char *states[] = {"未注册",     "已注册",   "正在搜索网络",
                                    "注册被拒绝", "状态未知", "已注册（漫游）"};
            SET(registration,
                strlen(s) == 1 && s[0] >= '0' && s[0] <= '5' ? states[s[0] - '0'] : "");
        }
    } else if (prefix == 5 && !memcmp(line, "+QCSQ", 5)) {
        n->rsrp[0] = n->rsrq[0] = n->sinr[0] = 0;
        if (count >= 5 && !strcmp(FIELD(0), "LTE")) {
            if (strlen(FIELD(2)) + 5 >= sizeof(n->rsrp) ||
                strlen(FIELD(4)) + 4 >= sizeof(n->rsrq)) {
                rc = -1;
                goto done;
            }
            snprintf(n->rsrp, sizeof(n->rsrp), "%s dBm", FIELD(2));
            snprintf(n->rsrq, sizeof(n->rsrq), "%s dB", FIELD(4));
            char *end;
            errno = 0;
            long x = strtol(FIELD(3), &end, 10);
            if (!errno && *FIELD(3) && !*end)
                snprintf(n->sinr, sizeof(n->sinr), "%.1f dB", (double)x / 5 - 20);
        }
    } else if (prefix == 8 && !memcmp(line, "+CGDCONT", 8)) {
        if (count >= 3 && !strcmp(FIELD(0), "1"))
            SET(apn, FIELD(2));
    } else if (prefix == 5 && !memcmp(line, "+QCFG", 5)) {
        if (count >= 2 && !strcmp(FIELD(0), "ims")) {
            const char *s = FIELD(1), *modes[] = {"自动（按运营商配置）", "强制启用", "强制关闭"};
            SET(ims, strlen(s) == 1 && s[0] >= '0' && s[0] <= '2' ? modes[s[0] - '0'] : "未知配置");
            if (count > 2)
                strcat(n->ims, !strcmp(FIELD(2), "1") ? "；VoLTE 功能已启用，注册状态待验证"
                                                      : "；VoLTE 功能未启用");
        }
    }
done:
    for (size_t i = 0; i < 32; i++)
        qdc_buf_free(&f[i]);
    return rc;
#undef FIELD
#undef SET
}
static void apply_lines(qdc_network *n, qdc_buf *b) {
    char *p = (char *)b->data;
    while (p && *p) {
        char *end = strchr(p, '\n');
        if (end)
            *end = 0;
        if (qdc_network_apply(n, p))
            strcpy(n->error, "部分网络信息暂不可用");
        if (!end)
            break;
        p = end + 1;
    }
}
static void refresh(monitor *m) {
    if (qdc_calls_busy(m->app))
        return;
    pthread_mutex_lock(&m->refresh_mu);
    qdc_network n = {0};
    strcpy(n.registration, "未知");
    strcpy(n.ims, "未知");
    const char *commands[] = {"AT+QNWINFO", "AT+CEREG?", "AT+QCSQ", "AT+CGDCONT?",
                              "AT+QCFG=\"ims\""};
    for (size_t i = 0; i < 5; i++) {
        qdc_buf b = {0};
        qdc_error e = {0};
        if (qdc_command(m->app, commands[i], 3000, &b, &e))
            strcpy(n.error, "部分网络信息暂不可用");
        else
            apply_lines(&n, &b);
        qdc_buf_free(&b);
        if (atomic_load(&m->app->stopping))
            break;
    }
    qdc_rfc3339(qdc_now_ms(m->app), n.updated_at);
    pthread_mutex_lock(&m->mu);
    m->network = n;
    pthread_mutex_unlock(&m->mu);
    pthread_mutex_unlock(&m->refresh_mu);
}
static uint64_t uint_file(const char *path) {
    qdc_buf b = {0};
    qdc_error e = {0};
    uint64_t v = 0;
    if (qdc_read_file(path, 4096, &b, &e) == 0 && b.data) {
        char *end;
        errno = 0;
        unsigned long long n = strtoull((char *)b.data, &end, 10);
        if (!errno && end != (char *)b.data)
            v = (uint64_t)n;
    }
    qdc_buf_free(&b);
    return v;
}
static void sample(monitor *m) {
    qdc_buf b = {0};
    qdc_error e = {0};
    if (qdc_read_file("/proc/sys/kernel/random/boot_id", 256, &b, &e) == 0 && b.data) {
        struct stat st;
        if (!stat("/sys/class/net/ecm0/statistics/rx_bytes", &st))
            qdc_traffic_sample(m->traffic, trim((char *)b.data),
                               uint_file("/sys/class/net/ecm0/statistics/rx_bytes"),
                               uint_file("/sys/class/net/ecm0/statistics/tx_bytes"),
                               qdc_now_ms(m->app));
    }
    qdc_buf_free(&b);
}
static void *loop(void *p) {
    monitor *m = p;
    qdc_buf b = {0};
    qdc_error e = {0};
    if (!qdc_command(m->app, "ATI", 3000, &b, &e) && b.data) {
        char *at = strstr((char *)b.data, "Revision:");
        if (at) {
            at += 9;
            char *end = strchr(at, '\n');
            if (end)
                *end = 0;
            pthread_mutex_lock(&m->mu);
            copy(m->firmware, sizeof(m->firmware), trim(at));
            pthread_mutex_unlock(&m->mu);
        }
    }
    qdc_buf_free(&b);
    for (unsigned i = 0; !atomic_load(&m->app->stopping); i++) {
        sample(m);
        if (i % 3 == 0)
            refresh(m);
        if (i % 12 == 0)
            qdc_traffic_write(m->traffic, &e);
        qdc_sleep(m->app, 5000);
    }
    return NULL;
}
int qdc_monitor_init(qdc_app *a, qdc_error *e) {
    monitor *m = calloc(1, sizeof(*m));
    if (!m)
        return -1;
    m->app = a;
    pthread_mutex_init(&m->mu, NULL);
    pthread_mutex_init(&m->refresh_mu, NULL);
    m->traffic = qdc_traffic_load(a->config.data_dir, e);
    if (!m->traffic) {
        pthread_mutex_destroy(&m->mu);
        pthread_mutex_destroy(&m->refresh_mu);
        free(m);
        return -1;
    }
    a->monitor = m;
    return 0;
}
int qdc_monitor_start(qdc_app *a, qdc_error *e) {
    monitor *m = a->monitor;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    int rc = pthread_create(&m->thread, &attr, loop, m);
    pthread_attr_destroy(&attr);
    if (rc) {
        qdc_error_set(e, "监控线程启动失败");
        return -1;
    }
    m->started = true;
    return 0;
}
void qdc_monitor_free(qdc_app *a) {
    monitor *m = a->monitor;
    if (!m)
        return;
    if (m->started)
        pthread_join(m->thread, NULL);
    qdc_error e = {0};
    qdc_traffic_write(m->traffic, &e);
    qdc_traffic_free(m->traffic);
    pthread_mutex_destroy(&m->mu);
    pthread_mutex_destroy(&m->refresh_mu);
    free(m);
    a->monitor = NULL;
}
int qdc_traffic_save(qdc_app *a, qdc_error *e) {
    monitor *m = a->monitor;
    if (!m) {
        qdc_error_set(e, "流量统计尚未准备好");
        return -1;
    }
    return qdc_traffic_write(m->traffic, e);
}
static yyjson_mut_val *network_json(yyjson_mut_doc *d, const qdc_network *n) {
    yyjson_mut_val *v = yyjson_mut_obj(d);
#define STR(key, field) yyjson_mut_obj_add_strcpy(d, v, key, n->field)
    STR("updated_at", updated_at);
    STR("technology", technology);
    STR("operator", operator_name);
    STR("band", band);
    STR("channel", channel);
    STR("registration", registration);
    STR("rsrp", rsrp);
    STR("rsrq", rsrq);
    STR("sinr", sinr);
    STR("apn", apn);
    STR("ims", ims);
    if (n->error[0])
        STR("error", error);
#undef STR
    yyjson_mut_obj_add_bool(d, v, "registered", n->registered);
    return v;
}
static yyjson_mut_val *device_json(monitor *m, yyjson_mut_doc *d) {
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, v, "version", QDC_VERSION);
    pthread_mutex_lock(&m->mu);
    yyjson_mut_obj_add_strcpy(d, v, "firmware", m->firmware);
    pthread_mutex_unlock(&m->mu);
    qdc_buf b = {0};
    qdc_error e = {0};
    uint64_t uptime = 0, total = 0, available = 0;
    if (!qdc_read_file("/proc/uptime", 4096, &b, &e) && b.data) {
        double x = strtod((char *)b.data, NULL);
        if (x >= 0)
            uptime = (uint64_t)x;
    }
    qdc_buf_free(&b);
    if (!qdc_read_file("/proc/meminfo", 65536, &b, &e) && b.data) {
        char *save = NULL;
        for (char *line = strtok_r((char *)b.data, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            unsigned long long n;
            if (sscanf(line, "MemTotal: %llu", &n) == 1)
                total = (uint64_t)n * 1024;
            if (sscanf(line, "MemAvailable: %llu", &n) == 1)
                available = (uint64_t)n * 1024;
        }
    }
    qdc_buf_free(&b);
    yyjson_mut_obj_add_uint(d, v, "uptime", uptime);
    yyjson_mut_obj_add_uint(d, v, "memory_total", total);
    yyjson_mut_obj_add_uint(d, v, "memory_available", available);
    struct statvfs fs;
    uint64_t disk_total = 0, disk_available = 0;
    if (!statvfs(m->app->config.data_dir, &fs)) {
        disk_total = (uint64_t)fs.f_blocks * fs.f_frsize;
        disk_available = (uint64_t)fs.f_bavail * fs.f_frsize;
    }
    yyjson_mut_obj_add_uint(d, v, "disk_total", disk_total);
    yyjson_mut_obj_add_uint(d, v, "disk_available", disk_available);
    bool audio = false;
    if (!qdc_read_file("/proc/asound/cards", 65536, &b, &e) && b.data)
        audio = *trim((char *)b.data) && !strstr((char *)b.data, "no soundcards");
    qdc_buf_free(&b);
    yyjson_mut_obj_add_bool(d, v, "audio_ready", audio);
    char previous[QDC_PATH_MAX];
    struct stat st;
    bool rollback =
        qdc_path(previous, sizeof(previous), m->app->config.data_dir, "previous/smsweb") == 0 &&
        stat(previous, &st) == 0;
    yyjson_mut_obj_add_bool(d, v, "rollback_available", rollback);
    return v;
}
static void check(yyjson_mut_doc *d, yyjson_mut_val *a, const char *label, const char *state,
                  const char *detail) {
    yyjson_mut_val *c = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, c, "label", label);
    yyjson_mut_obj_add_strcpy(d, c, "state", state);
    yyjson_mut_obj_add_strcpy(d, c, "detail", detail);
    yyjson_mut_arr_add_val(a, c);
}
int qdc_monitor_route(qdc_app *a, qdc_request *r, qdc_response *resp) {
    bool overview = !strcmp(r->method, "GET") && !strcmp(r->path, "/api/overview"),
         diagnostics = !strcmp(r->method, "POST") && !strcmp(r->path, "/api/diagnostics");
    if (!overview && !diagnostics)
        return 0;
    monitor *m = a->monitor;
    if (diagnostics && qdc_outbox_busy(a))
        return qdc_response_error(resp, 409, "正在发送短信，请完成后再诊断") < 0 ? -1 : 1;
    if (diagnostics)
        refresh(m);
    pthread_mutex_lock(&m->mu);
    qdc_network n = m->network;
    pthread_mutex_unlock(&m->mu);
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return -1;
    yyjson_mut_val *v = yyjson_mut_doc_get_root(d);
    if (overview) {
        yyjson_mut_obj_add_val(d, v, "network", network_json(d, &n));
        yyjson_mut_doc *traffic = qdc_traffic_view(m->traffic, qdc_now_ms(a));
        if (!traffic) {
            yyjson_mut_doc_free(d);
            return -1;
        }
        yyjson_mut_obj_add_val(d, v, "traffic",
                               yyjson_mut_val_mut_copy(d, yyjson_mut_doc_get_root(traffic)));
        yyjson_mut_doc_free(traffic);
        yyjson_mut_obj_add_val(d, v, "device", device_json(m, d));
    } else {
        yyjson_mut_val *checks = yyjson_mut_arr(d);
        yyjson_mut_obj_add_val(d, v, "checks", checks);
        check(d, checks, "管理页面连接", "ok", "已通过 USB 网络连接到模块");
        yyjson_doc *pin = qdc_pin_snapshot(a);
        yyjson_val *state = pin ? yyjson_obj_get(yyjson_doc_get_root(pin), "state") : NULL;
        const char *sim = yyjson_get_str(state);
        check(d, checks, "SIM 卡", sim && !strcmp(sim, "READY") ? "ok" : "warn",
              sim ? sim : "UNKNOWN");
        if (pin)
            yyjson_doc_free(pin);
        check(d, checks, "运营商注册", n.registered ? "ok" : "warn", n.registration);
        char signal[520];
        snprintf(signal, sizeof(signal), "%s / %s", n.rsrp, n.rsrq);
        check(d, checks, "无线信号", n.rsrp[0] ? "ok" : "warn", trim(signal));
        char error[512];
        qdc_domain_last_error(a, error, sizeof(error));
        check(d, checks, "短信读取", error[0] ? "warn" : "ok",
              error[0] ? error : "最近一次轮询正常");
        check(d, checks, "4G 外网访问", "manual",
              "请在 iPhone 关闭 Wi-Fi 和自身蜂窝数据后打开测试网页；本页连接成功不代表 4G 已联网");
        char now[40];
        qdc_rfc3339(qdc_now_ms(a), now);
        yyjson_mut_obj_add_strcpy(d, v, "checked_at", now);
    }
    return qdc_response_json(resp, 200, d) < 0 ? -1 : 1;
}
