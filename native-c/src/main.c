#include "qdc.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static qdc_app *running;
static void stop(int signal_number) {
    (void)signal_number;
    if (running)
        atomic_store(&running->stopping, true);
}
static int duration(const char *s, unsigned *out) {
    errno = 0;
    char *end;
    double value = strtod(s, &end);
    if (end == s || errno || value <= 0)
        return -1;
    if (!strcmp(end, "ms")) {
    } else if (!strcmp(end, "s"))
        value *= 1000;
    else if (!strcmp(end, "m"))
        value *= 60000;
    else
        return -1;
    if (!(value >= 1 && value <= 3600000))
        return -1;
    *out = (unsigned)value;
    return 0;
}
static void usage(void) {
    puts("smsweb-c [-at /dev/smd9] [-listen 192.168.225.1] [-port 8787] [-data /data/smsweb] "
         "[-poll 3s] [-debug]\n独立 C 后端；默认仅监听模块 USB 地址，测试可用 127.0.0.1。");
}
int main(int argc, char **argv) {
    qdc_app app = {.config = {.data_dir = "/data/smsweb",
                              .at_path = "/dev/smd9",
                              .listen = "192.168.225.1",
                              .port = "8787",
                              .poll_ms = 3000}};
    for (int i = 1; i < argc; i++) {
        const char *key = argv[i];
        if (*key != '-') {
            usage();
            return 2;
        }
        key++;
        if (*key == '-')
            key++;
        if (!strcmp(key, "help") || !strcmp(key, "h")) {
            usage();
            return 0;
        }
        if (!strcmp(key, "version")) {
            puts("smsweb-c " QDC_VERSION);
            return 0;
        }
        if (!strcmp(key, "debug")) {
            app.config.debug = true;
            continue;
        }
        if (!strcmp(key, "debug=true") || !strcmp(key, "debug=false")) {
            app.config.debug = !strcmp(key, "debug=true");
            continue;
        }
        char option[32];
        const char *value = strchr(key, '=');
        if (value) {
            size_t n = (size_t)(value - key);
            if (n >= sizeof(option))
                return 2;
            memcpy(option, key, n);
            option[n] = 0;
            key = option;
            value++;
        } else {
            if (++i >= argc) {
                usage();
                return 2;
            }
            value = argv[i];
        }
        if (!strcmp(key, "at"))
            app.config.at_path = value;
        else if (!strcmp(key, "data"))
            app.config.data_dir = value;
        else if (!strcmp(key, "port"))
            app.config.port = value;
        else if (!strcmp(key, "listen"))
            app.config.listen = value;
        else if (!strcmp(key, "poll")) {
            if (duration(value, &app.config.poll_ms)) {
                fputs("轮询间隔无效\n", stderr);
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    qdc_error error = {0};
    if (qdc_listen_address(app.config.listen, app.config.port, &error)) {
        qdc_log(error.message);
        return 2;
    }
    if (qdc_mkdir_private(app.config.data_dir, &error)) {
        qdc_log(error.message);
        return 1;
    }
    pthread_mutex_init(&app.action_mu, NULL);
    pthread_mutex_init(&app.op_mu, NULL);
    atomic_init(&app.stopping, false);
    running = &app;
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = stop;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
    int rc = 1;
    if (qdc_clock_init(&app, &error) || qdc_auth_init(&app, &error) ||
        qdc_domain_init(&app, &error) || qdc_monitor_init(&app, &error) ||
        qdc_audio_init(&app, &error) || qdc_push_init(&app, &error))
        goto done;
    if (qdc_domain_start(&app, &error) || qdc_monitor_start(&app, &error))
        goto done;
    if (qdc_audio_start(&app, &error))
        qdc_log(error.message);
    if (qdc_push_start(&app, &error))
        goto done;
    rc = qdc_http_serve(&app, &error) ? 1 : 0;
done:
    if (rc && error.message[0])
        qdc_log(error.message);
    atomic_store(&app.stopping, true);
    qdc_push_free(&app);
    qdc_monitor_free(&app);
    qdc_audio_free(&app);
    qdc_domain_free(&app);
    qdc_auth_free(&app);
    qdc_clock_free(&app);
    running = NULL;
    pthread_mutex_destroy(&app.action_mu);
    pthread_mutex_destroy(&app.op_mu);
    return rc;
}
