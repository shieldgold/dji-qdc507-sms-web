#include "qdc_domain.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

struct qdc_pin {
    pthread_mutex_t mu;
    char guard[QDC_PATH_MAX];
};
static yyjson_doc *pin_doc(const char *state, bool blocked) {
    yyjson_mut_doc *d = qdc_json_object();
    if (!d)
        return NULL;
    yyjson_mut_val *r = yyjson_mut_doc_get_root(d);
    yyjson_mut_obj_add_strcpy(d, r, "state", state);
    yyjson_mut_obj_add_bool(d, r, "blocked", blocked);
    yyjson_doc *ret = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    return ret;
}
static yyjson_doc *status_locked(qdc_app *a) {
    qdc_pin *p = ((qdc_domain *)a->domain)->pin;
    qdc_buf lines = {0};
    qdc_error e = {0};
    char state[128] = "UNKNOWN";
    if (!qdc_command(a, "AT+CPIN?", 5000, &lines, &e))
        for (size_t i = 0; i < lines.len;) {
            size_t end = i;
            while (end < lines.len && lines.data[end] != '\n')
                end++;
            if (end - i >= 6 && !memcmp(lines.data + i, "+CPIN:", 6)) {
                qdc_slice s = qdc_trim((qdc_slice){(char *)lines.data + i + 6, end - i - 6});
                if (s.len < sizeof state && !memchr(s.ptr, 0, s.len)) {
                    memcpy(state, s.ptr, s.len);
                    state[s.len] = 0;
                }
            }
            i = end + 1;
        }
    qdc_buf_free(&lines);
    if (!strcmp(state, "READY"))
        (void)unlink(p->guard);
    struct stat st;
    bool blocked = stat(p->guard, &st) == 0 || errno != ENOENT;
    return pin_doc(state, blocked);
}
int qdc_pin_init(qdc_app *a, qdc_error *e) {
    qdc_pin *p = calloc(1, sizeof *p);
    if (!p) {
        qdc_error_set(e, "内存不足");
        return -1;
    }
    pthread_mutex_init(&p->mu, NULL);
    ((qdc_domain *)a->domain)->pin = p;
    if (qdc_path(p->guard, sizeof p->guard, a->config.data_dir, "pin-unlock.pending"))
        return -1;
    return 0;
}
void qdc_pin_free(qdc_app *a) {
    qdc_domain *d = a->domain;
    if (!d->pin)
        return;
    pthread_mutex_destroy(&d->pin->mu);
    free(d->pin);
    d->pin = NULL;
}
yyjson_doc *qdc_pin_snapshot(qdc_app *a) {
    qdc_pin *p = ((qdc_domain *)a->domain)->pin;
    pthread_mutex_lock(&p->mu);
    yyjson_doc *d = status_locked(a);
    pthread_mutex_unlock(&p->mu);
    return d;
}
static int persist_guard(qdc_app *a, qdc_error *e) {
    qdc_pin *p = ((qdc_domain *)a->domain)->pin;
    int fd = open(p->guard, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        qdc_error_set(e, "无法保存解锁保护记录，未提交 PIN");
        return -1;
    }
    const char text[] = "attempt pending\n";
    size_t n = 0;
    int rc = 0;
    while (n < sizeof text - 1) {
        ssize_t w = write(fd, text + n, sizeof text - 1 - n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            rc = -1;
            break;
        }
        n += (size_t)w;
    }
    if (!rc && fsync(fd))
        rc = -1;
    if (close(fd))
        rc = -1;
    int dir = -1;
    if (!rc) {
        dir = open(a->config.data_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0 || fsync(dir))
            rc = -1;
    }
    if (dir >= 0)
        close(dir);
    if (rc)
        qdc_error_set(e, "解锁保护记录未保存，未提交 PIN");
    return rc;
}
int qdc_pin_unlock(qdc_app *a, qdc_slice pin, qdc_slice id, yyjson_doc **snapshot, qdc_error *e) {
    *snapshot = NULL;
    qdc_pin *p = ((qdc_domain *)a->domain)->pin;
    pthread_mutex_lock(&p->mu);
    if (pin.len < 4 || pin.len > 8) {
        qdc_error_set(e, "PIN 必须为 4–8 位数字");
        goto invalid;
    }
    for (size_t i = 0; i < pin.len; i++)
        if (pin.ptr[i] < '0' || pin.ptr[i] > '9') {
            qdc_error_set(e, "PIN 必须为 4–8 位数字");
            goto invalid;
        }
    if (!id.len || id.len > 128) {
        qdc_error_set(e, "缺少解锁请求标识");
        goto invalid;
    }
    *snapshot = status_locked(a);
    if (!*snapshot)
        goto fail;
    yyjson_val *r = yyjson_doc_get_root(*snapshot);
    qdc_slice state = qdc_json_slice(yyjson_obj_get(r, "state"));
    if (qdc_slice_eq(state, "READY")) {
        pthread_mutex_unlock(&p->mu);
        return 0;
    }
    if (!qdc_slice_eq(state, "SIM PIN")) {
        qdc_error_set(e, "当前不是 SIM PIN 锁定状态，请检查 SIM 状态；不能在这里输入 PUK 或 PIN2");
        goto fail;
    }
    if (yyjson_get_bool(yyjson_obj_get(r, "blocked"))) {
        qdc_error_set(e, "上次解锁未确认成功；请核实 PIN 后明确允许再试，程序不会自动重试");
        goto fail;
    }
    if (persist_guard(a, e))
        goto fail;
    char cmd[32];
    memcpy(cmd, "AT+CPIN=\"", 9);
    memcpy(cmd + 9, pin.ptr, pin.len);
    cmd[9 + pin.len] = '"';
    cmd[10 + pin.len] = 0;
    qdc_buf response = {0};
    qdc_error hidden = {0};
    int rc = qdc_command(a, cmd, 10000, &response, &hidden);
    memset(cmd, 0, sizeof cmd);
    qdc_buf_free(&response);
    yyjson_doc_free(*snapshot);
    *snapshot = status_locked(a);
    if (*snapshot &&
        qdc_slice_eq(qdc_json_slice(yyjson_obj_get(yyjson_doc_get_root(*snapshot), "state")),
                     "READY")) {
        pthread_mutex_unlock(&p->mu);
        return 0;
    }
    qdc_error_set(e, rc ? "PIN 未确认解锁成功。请核对 PIN，切勿连续尝试；SIM 可能扣除了重试次数"
                        : "模块已接受解锁请求，但尚未确认 READY；请刷新状态，暂勿重试");
    goto fail;
invalid:
    *snapshot = pin_doc("", false);
fail:
    pthread_mutex_unlock(&p->mu);
    return -1;
}
int qdc_pin_retry(qdc_app *a, bool confirmed, qdc_error *e) {
    qdc_pin *p = ((qdc_domain *)a->domain)->pin;
    pthread_mutex_lock(&p->mu);
    if (!confirmed) {
        qdc_error_set(e, "请先确认已核实正确 PIN");
        pthread_mutex_unlock(&p->mu);
        return -1;
    }
    yyjson_doc *s = status_locked(a);
    bool ok = s && qdc_slice_eq(qdc_json_slice(yyjson_obj_get(yyjson_doc_get_root(s), "state")),
                                "SIM PIN");
    if (s)
        yyjson_doc_free(s);
    if (!ok) {
        qdc_error_set(e, "仅 SIM PIN 状态允许重新授权");
        pthread_mutex_unlock(&p->mu);
        return -1;
    }
    int rc = unlink(p->guard);
    if (rc && errno == ENOENT)
        rc = 0;
    if (rc)
        qdc_error_set(e, "无法清除解锁保护记录");
    pthread_mutex_unlock(&p->mu);
    return rc;
}
int qdc_ensure_sms(qdc_app *a, qdc_error *e) {
    qdc_domain *d = a->domain;
    yyjson_doc *s = qdc_pin_snapshot(a);
    bool ready =
        s && qdc_slice_eq(qdc_json_slice(yyjson_obj_get(yyjson_doc_get_root(s), "state")), "READY");
    if (s)
        yyjson_doc_free(s);
    if (!ready) {
        d->sms_configured = false;
        qdc_error_set(e, "SIM 尚未就绪，请先解锁 PIN 或检查 SIM 卡");
        return -1;
    }
    if (d->sms_configured)
        return 0;
    static const char *steps[] = {"AT", "ATE0", "AT+CMGF=0", "AT+CPMS=\"ME\",\"ME\",\"ME\"",
                                  "AT+CNMI=2,1,0,0,0"};
    for (size_t i = 0; i < 5; i++) {
        qdc_buf b = {0};
        int rc = qdc_command(a, steps[i], 5000, &b, e);
        qdc_buf_free(&b);
        if (rc)
            return -1;
    }
    d->sms_configured = true;
    return 0;
}
