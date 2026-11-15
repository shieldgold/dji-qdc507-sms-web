#include "qdc_auth.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int handled(int rc) {
    return rc ? -1 : 1;
}
static int export_backup(qdc_app *app, qdc_response *response) {
    yyjson_doc *messages = qdc_messages(app);
    yyjson_mut_doc *doc = qdc_json_object();
    if (!messages || !doc) {
        if (messages)
            yyjson_doc_free(messages);
        if (doc)
            yyjson_mut_doc_free(doc);
        return -1;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    char now[40];
    qdc_rfc3339(qdc_system_ms(), now);
    yyjson_mut_val *copy = yyjson_val_mut_copy(doc, yyjson_doc_get_root(messages));
    bool ok = copy && yyjson_mut_obj_add_uint(doc, root, "version", 1) &&
              qdc_json_add_string(doc, root, "exported_at", now, strlen(now)) &&
              yyjson_mut_obj_add_val(doc, root, "messages", copy);
    yyjson_doc_free(messages);
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return -1;
    }
    if (qdc_response_header(response, "Content-Disposition",
                            "attachment; filename=\"module-sms-backup.json\"")) {
        yyjson_mut_doc_free(doc);
        return -1;
    }
    return handled(qdc_response_json(response, 200, doc));
}
static bool version_one(yyjson_val *v) {
    return (yyjson_is_uint(v) && yyjson_get_uint(v) == 1) ||
           (yyjson_is_sint(v) && yyjson_get_sint(v) == 1);
}
static int restore_backup(qdc_app *app, qdc_request *request, qdc_response *response) {
    pthread_mutex_lock(&app->action_mu);
    if (qdc_outbox_busy(app)) {
        pthread_mutex_unlock(&app->action_mu);
        return handled(qdc_response_error(response, 409, "短信正在提交，暂时不能恢复备份"));
    }
    pthread_mutex_lock(&app->op_mu);
    qdc_error e = {0};
    yyjson_val *root = qdc_request_json(request, &e);
    yyjson_val *array = root ? yyjson_obj_get(root, "messages") : NULL;
    yyjson_val *exported = root ? yyjson_obj_get(root, "exported_at") : NULL;
    bool schema = root && yyjson_is_obj(root) && version_one(yyjson_obj_get(root, "version")) &&
                  (!exported || yyjson_is_null(exported) || yyjson_is_str(exported)) &&
                  (!array || yyjson_is_null(array) || yyjson_is_arr(array));
    int rc;
    if (!schema)
        rc = handled(qdc_response_error(response, 400, "不支持的短信备份文件"));
    else if (yyjson_arr_size(array) > 300)
        rc = handled(qdc_response_error(response, 400, "一次最多恢复 300 条短信"));
    else {
        yyjson_doc *empty = NULL;
        if (!array || yyjson_is_null(array)) {
            empty = yyjson_read("[]", 2, 0);
            array = empty ? yyjson_doc_get_root(empty) : NULL;
        }
        int added = 0;
        if (!array)
            rc = -1;
        else if (qdc_store_restore(app, array, &added, &e))
            rc = handled(qdc_response_error(response, 400, e.message));
        else {
            yyjson_mut_doc *doc = qdc_json_object();
            bool ok = doc != NULL;
            if (ok) {
                yyjson_mut_val *out = yyjson_mut_doc_get_root(doc);
                char note[192];
                int n = snprintf(note, sizeof(note),
                                 "已合并 %d 条短信，原有短信保留；归档最多 300 条", added);
                ok = n >= 0 && (size_t)n < sizeof(note) &&
                     yyjson_mut_obj_add_sint(doc, out, "added", added) &&
                     qdc_json_add_string(doc, out, "note", note, (size_t)n);
            }
            if (!ok) {
                if (doc)
                    yyjson_mut_doc_free(doc);
                rc = -1;
            } else
                rc = handled(qdc_response_json(response, 200, doc));
        }
        if (empty)
            yyjson_doc_free(empty);
    }
    pthread_mutex_unlock(&app->op_mu);
    pthread_mutex_unlock(&app->action_mu);
    return rc;
}
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t ready;
    bool published;
    pid_t pid;
} rollback_reaper;
static void *reap_rollback(void *opaque) {
    rollback_reaper *r = opaque;
    pthread_mutex_lock(&r->mu);
    while (!r->published)
        pthread_cond_wait(&r->ready, &r->mu);
    pid_t pid = r->pid;
    pthread_mutex_unlock(&r->mu);
    if (pid > 0)
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
    pthread_cond_destroy(&r->ready);
    pthread_mutex_destroy(&r->mu);
    free(r);
    return NULL;
}
static void publish_child(rollback_reaper *r, pid_t pid) {
    pthread_mutex_lock(&r->mu);
    r->pid = pid;
    r->published = true;
    pthread_cond_signal(&r->ready);
    pthread_mutex_unlock(&r->mu);
}
static int previous_available(const char *path, void *context) {
    (void)context;
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && access(path, X_OK) == 0;
}
static int launch_rollback(const char *script, void *context) {
    (void)context;
    /* No caller-controlled executable, directory, arguments, or shell command. */
    if (strcmp(script, "/data/smsweb/restore-version.sh"))
        return -1;
    struct stat st;
    if (lstat(script, &st) || !S_ISREG(st.st_mode) || access(script, R_OK))
        return -1;
    int error_pipe[2];
    if (pipe(error_pipe))
        return -1;
    if (fcntl(error_pipe[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) < 0) {
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }
    rollback_reaper *reaper = calloc(1, sizeof(*reaper));
    if (!reaper) {
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }
    if (pthread_mutex_init(&reaper->mu, NULL)) {
        free(reaper);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }
    if (pthread_cond_init(&reaper->ready, NULL)) {
        pthread_mutex_destroy(&reaper->mu);
        free(reaper);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }
    pthread_attr_t attr;
    pthread_t thread;
    if (pthread_attr_init(&attr)) {
        pthread_cond_destroy(&reaper->ready);
        pthread_mutex_destroy(&reaper->mu);
        free(reaper);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }
    int rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (!rc)
        rc = pthread_create(&thread, &attr, reap_rollback, reaper);
    pthread_attr_destroy(&attr);
    if (rc) {
        pthread_cond_destroy(&reaper->ready);
        pthread_mutex_destroy(&reaper->mu);
        free(reaper);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(error_pipe[0]);
        if (setsid() < 0) {
            int error = errno;
            (void)write(error_pipe[1], &error, sizeof(error));
            _exit(127);
        }
        execl("/bin/sh", "/bin/sh", script, (char *)NULL);
        int error = errno;
        (void)write(error_pipe[1], &error, sizeof(error));
        _exit(127);
    }
    close(error_pipe[1]);
    publish_child(reaper, pid);
    if (pid < 0) {
        close(error_pipe[0]);
        return -1;
    }
    int child_error = 0;
    ssize_t n;
    do {
        n = read(error_pipe[0], &child_error, sizeof(child_error));
    } while (n < 0 && errno == EINTR);
    close(error_pipe[0]);
    return n == 0 ? 0 : -1;
}
int qdc_rollback_with_ops(qdc_app *app, qdc_response *response, const qdc_rollback_ops *ops) {
    pthread_mutex_lock(&app->action_mu);
    int status;
    const char *message;
    if (app->maintenance || qdc_outbox_busy(app) || qdc_calls_busy(app)) {
        status = 409;
        message = "正在处理短信、通话或维护，请完成后再恢复版本";
    } else if (!app->config.data_dir || strcmp(app->config.data_dir, "/data/smsweb")) {
        status = 400;
        message = "此环境不支持恢复模块版本";
    } else if (!ops || !ops->previous_available || !ops->launch ||
               ops->previous_available("/data/smsweb/previous/smsweb", ops->context) <= 0) {
        status = 400;
        message = "没有可恢复的旧版本";
    } else {
        qdc_error e = {0};
        if (qdc_traffic_save(app, &e)) {
            status = 500;
            message = "流量统计未能保存，已取消维护";
        } else if (ops->launch("/data/smsweb/restore-version.sh", ops->context)) {
            status = 500;
            message = "无法启动恢复程序";
        } else {
            app->maintenance = true;
            pthread_mutex_unlock(&app->action_mu);
            return handled(qdc_response_ok(response, 202));
        }
    }
    pthread_mutex_unlock(&app->action_mu);
    return handled(qdc_response_error(response, status, message));
}
int qdc_backup_route(qdc_app *app, qdc_request *request, qdc_response *response) {
    if (!strcmp(request->method, "GET") && !strcmp(request->path, "/api/export"))
        return export_backup(app, response);
    if (strcmp(request->method, "POST"))
        return 0;
    if (!strcmp(request->path, "/api/restore"))
        return restore_backup(app, request, response);
    if (!strcmp(request->path, "/api/maintenance/rollback")) {
        const qdc_rollback_ops ops = {previous_available, launch_rollback, NULL};
        return qdc_rollback_with_ops(app, response, &ops);
    }
    return 0;
}
