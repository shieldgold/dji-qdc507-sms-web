#ifndef QDC_DOMAIN_H
#define QDC_DOMAIN_H
#include "qdc.h"

typedef struct {
    qdc_buf sender, body;
    int ref, total, seq, index;
    char status[32], local_time[24];
    int64_t timestamp;
    bool valid_time;
} qdc_part;
typedef struct {
    qdc_buf *items;
    size_t len;
} qdc_pdus;
typedef struct qdc_at qdc_at;
typedef struct qdc_store qdc_store;
typedef struct qdc_pin qdc_pin;
typedef struct qdc_calls qdc_calls;
typedef struct qdc_outbox qdc_outbox;
static inline int qdc_domain_thread(pthread_t *t, void *(*fn)(void *), void *arg) {
    pthread_attr_t at;
    int rc = pthread_attr_init(&at);
    if (rc)
        return rc;
    rc = pthread_attr_setstacksize(&at, 128 * 1024);
    if (!rc)
        rc = pthread_create(t, &at, fn, arg);
    pthread_attr_destroy(&at);
    return rc;
}
typedef struct {
    qdc_app *app;
    qdc_at *at;
    qdc_store *store;
    qdc_pin *pin;
    qdc_calls *calls;
    qdc_outbox *outbox;
    pthread_mutex_t state_mu, identity_mu, wake_mu;
    pthread_cond_t wake;
    pthread_t poll_thread, calls_thread;
    bool poll_started, calls_started, sms_configured, poll_wake;
    uint64_t identity_at;
    qdc_sim_identity identity;
    char last_poll[16], last_error[512];
} qdc_domain;

void qdc_part_free(qdc_part *);
void qdc_pdus_free(qdc_pdus *);
int qdc_pdu_submit(qdc_slice, qdc_slice, unsigned, qdc_pdus *, qdc_error *);
int qdc_pdu_deliver(qdc_slice, qdc_part *, qdc_error *);
yyjson_doc *qdc_merge_parts(qdc_part *, size_t);
int qdc_request_id(qdc_slice);
qdc_slice qdc_trim(qdc_slice);

int qdc_at_open(qdc_app *, qdc_error *);
void qdc_at_close(qdc_app *);
int qdc_at_submit(qdc_app *, qdc_slice, bool, qdc_buf *, qdc_error *);
int qdc_ensure_sms(qdc_app *, qdc_error *);
void qdc_sms_notify(qdc_app *);

int qdc_store_init(qdc_app *, qdc_error *);
void qdc_store_free(qdc_app *);
yyjson_doc *qdc_store_get(qdc_app *, qdc_slice);
int qdc_store_insert(qdc_app *, yyjson_val *, qdc_error *);
int qdc_store_remove(qdc_app *, qdc_slice, qdc_error *);
int qdc_store_merge(qdc_app *, yyjson_val *, int *, qdc_error *);
int qdc_store_update_send(qdc_app *, qdc_slice, const char *, const char *, int, const char *,
                          qdc_error *);
int qdc_store_recover(qdc_app *, qdc_error *);
#ifdef QDC_TESTING
size_t qdc_store_test_pool(qdc_app *);
#endif

int qdc_outbox_init(qdc_app *, qdc_error *);
void qdc_outbox_free(qdc_app *);
int qdc_outbox_enqueue(qdc_app *, qdc_slice, qdc_slice, qdc_slice, yyjson_doc **, qdc_error *);
int qdc_pin_init(qdc_app *, qdc_error *);
void qdc_pin_free(qdc_app *);
int qdc_pin_unlock(qdc_app *, qdc_slice, qdc_slice, yyjson_doc **, qdc_error *);
int qdc_pin_retry(qdc_app *, bool, qdc_error *);
int qdc_calls_init(qdc_app *, qdc_error *);
void qdc_calls_free(qdc_app *);
int qdc_calls_refresh(qdc_app *, qdc_error *);
int qdc_calls_dial(qdc_app *, qdc_slice, qdc_slice, yyjson_doc **, qdc_error *);
int qdc_calls_action(qdc_app *, const char *, const char *, qdc_slice, qdc_error *);
#endif
