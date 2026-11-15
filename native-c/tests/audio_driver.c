#include "qdc_audio.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static pthread_mutex_t calls_mu = PTHREAD_MUTEX_INITIALIZER;
static char call_id[25] = "aaaaaaaaaaaaaaaaaaaaaaaa";
static bool present = true, active = false;
static unsigned hangups;
yyjson_doc *qdc_calls_snapshot(qdc_app *a) {
    (void)a;
    yyjson_mut_doc *d = qdc_json_object();
    yyjson_mut_val *r = yyjson_mut_doc_get_root(d), *calls = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, r, "calls", calls);
    pthread_mutex_lock(&calls_mu);
    if (present) {
        yyjson_mut_val *v = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, v, "id", call_id);
        yyjson_mut_obj_add_strcpy(d, v, "state", active ? "active" : "incoming");
        yyjson_mut_arr_add_val(calls, v);
    }
    pthread_mutex_unlock(&calls_mu);
    yyjson_doc *out = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    return out;
}
int qdc_calls_hangup(qdc_app *a, const char *id, qdc_error *e) {
    (void)a;
    (void)e;
    pthread_mutex_lock(&calls_mu);
    if (present && !strcmp(id, call_id)) {
        hangups++;
        present = false;
    }
    pthread_mutex_unlock(&calls_mu);
    return 0;
}
static void doc_print(yyjson_doc *d) {
    size_t n;
    char *s = yyjson_write(d, 0, &n);
    assert(s);
    fwrite(s, 1, n, stdout);
    putchar('\n');
    fflush(stdout);
    free(s);
    yyjson_doc_free(d);
}
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    signal(SIGPIPE, SIG_IGN);
    qdc_app a = {.config = {.data_dir = argv[1], .listen = "127.0.0.1"}};
    atomic_init(&a.stopping, false);
    qdc_error e = {0};
    if (qdc_audio_init(&a, &e))
        return 3;
    char pcm[4096], marker[4096], proc[4096], log[4096];
    qdc_path(pcm, sizeof(pcm), argv[1], "pcm");
    qdc_path(marker, sizeof(marker), argv[1], "ready");
    qdc_path(proc, sizeof(proc), argv[1], "proc");
    qdc_path(log, sizeof(log), argv[1], "voice.log");
    if (qdc_audio_test_paths(&a, pcm, marker, proc, log, (unsigned)atoi(argv[2])) ||
        qdc_audio_start(&a, &e))
        return 4;
    puts("READY");
    fflush(stdout);
    char command[64];
    while (fgets(command, sizeof(command), stdin)) {
        command[strcspn(command, "\n")] = 0;
        if (!strcmp(command, "ticket")) {
            qdc_request r = {.method = "POST", .path = "/api/calls/audio"};
            qdc_response s = {0};
            if (qdc_audio_route(&a, &r, &s) < 0)
                return 5;
            fwrite(s.body.data, 1, s.body.len, stdout);
            putchar('\n');
            qdc_response_free(&s);
        } else if (!strcmp(command, "status")) {
            doc_print(qdc_audio_snapshot(&a));
            continue;
        } else if (!strcmp(command, "stats")) {
            pthread_mutex_lock(&calls_mu);
            printf("{\"hangups\":%u}\n", hangups);
            pthread_mutex_unlock(&calls_mu);
        } else if (!strcmp(command, "quit"))
            break;
        else {
            pthread_mutex_lock(&calls_mu);
            if (!strcmp(command, "callb")) {
                strcpy(call_id, "bbbbbbbbbbbbbbbbbbbbbbbb");
                present = true;
            } else if (!strcmp(command, "active")) {
                active = true;
                present = true;
            } else if (!strcmp(command, "none"))
                present = false;
            else if (!strcmp(command, "ringing")) {
                active = false;
                present = true;
            }
            pthread_mutex_unlock(&calls_mu);
            puts("{}");
        }
        fflush(stdout);
    }
    atomic_store(&a.stopping, true);
    qdc_audio_free(&a);
    return 0;
}
