/* Exercise the private queue state without opening any modem/PTY. */
#include "../src/at.c"
#include <assert.h>
void qdc_sms_notify(qdc_app *a) {
    (void)a;
}
int main(void) {
    int f[2];
    assert(!pipe(f));
    fcntl(f[0], F_SETFL, O_NONBLOCK);
    qdc_domain d = {0};
    qdc_app a = {.domain = &d};
    qdc_at at = {.app = &a, .fd = f[1]};
    d.at = &at;
    pthread_mutex_init(&at.serial, NULL);
    pthread_mutex_init(&at.mu, NULL);
    assert(!qdc_cond_init(&at.ready));
    for (unsigned i = 0; i < 4097; i++)
        push(&at, (const unsigned char *)"value", 5);
    assert(at.closed);
    qdc_buf out = {0};
    qdc_error e = {0};
    assert(qdc_command(&a, "ATD10010;", 1, &out, &e) == -1);
    char b[64];
    ssize_t n = read(f[0], b, sizeof(b));
    assert(n == -1 && errno == EAGAIN);
    qdc_buf_free(&out);
    drain(&at);
    at.closed = false;
    unsigned char *large = malloc(768 * 1024);
    assert(large);
    memset(large, 'A', 768 * 1024);
    push(&at, large, 768 * 1024);
    push(&at, large, 768 * 1024);
    assert(at.closed);
    free(large);
    drain(&at);
    pthread_cond_destroy(&at.ready);
    pthread_mutex_destroy(&at.mu);
    pthread_mutex_destroy(&at.serial);
    close(f[0]);
    close(f[1]);
    puts("AT line/byte limits fail before physical command: PASS");
    return 0;
}
