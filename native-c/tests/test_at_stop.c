/* A pending long command must notice stop without waiting its whole timeout. */
#include "../src/at.c"
#include <assert.h>
void qdc_sms_notify(qdc_app *a) {
    (void)a;
}
static atomic_bool finished;
static void *command_worker(void *arg) {
    qdc_app *app = arg;
    qdc_buf out = {0};
    qdc_error e = {0};
    assert(qdc_command(app, "AT", 60000, &out, &e) == -1);
    qdc_buf_free(&out);
    atomic_store(&finished, true);
    return NULL;
}
int main(void) {
    int p[2];
    assert(!pipe(p));
    qdc_domain d = {0};
    qdc_app a = {.domain = &d};
    qdc_at at = {.app = &a, .fd = p[1]};
    d.at = &at;
    pthread_mutex_init(&at.serial, NULL);
    pthread_mutex_init(&at.mu, NULL);
    assert(!qdc_cond_init(&at.ready));
    atomic_init(&a.stopping, false);
    atomic_init(&finished, false);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, command_worker, &a));
    char command[3];
    size_t count = 0;
    while (count < 3) {
        ssize_t n = read(p[0], command + count, 3 - count);
        assert(n > 0);
        count += (size_t)n;
    }
    struct timespec settled = {0, 50000000};
    nanosleep(&settled, NULL);
    atomic_store(&a.stopping, true);
    uint64_t until = qdc_mono_ms() + 1000;
    while (!atomic_load(&finished) && qdc_mono_ms() < until) {
        struct timespec t = {0, 10000000};
        nanosleep(&t, NULL);
    }
    assert(atomic_load(&finished));
    pthread_join(thread, NULL);
    pthread_cond_destroy(&at.ready);
    pthread_mutex_destroy(&at.mu);
    pthread_mutex_destroy(&at.serial);
    close(p[0]);
    close(p[1]);
    puts("AT pending command cancellation under 1 second: PASS");
    return 0;
}
