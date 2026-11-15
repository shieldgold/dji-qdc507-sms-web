#ifndef QDC_H
#define QDC_H
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <yyjson.h>

#define QDC_VERSION "2.0.0"
#define QDC_PATH_MAX 4096
#define QDC_BODY_MAX 32768
#define QDC_RESTORE_MAX 1048576

typedef struct {
    unsigned char *data;
    size_t len, cap;
} qdc_buf;
typedef struct {
    const char *ptr;
    size_t len;
} qdc_slice;
typedef struct {
    char message[512];
    bool uncertain;
} qdc_error;
typedef struct {
    const char *method, *path, *query, *cookie;
    qdc_slice body;
    yyjson_doc *json; /* request-owned; lazily parsed by qdc_request_json */
} qdc_request;
typedef struct {
    char name[48], value[512];
} qdc_header;
typedef struct {
    int status;
    const char *content_type;
    qdc_buf body;
    const unsigned char *static_body;
    size_t static_len;
    qdc_header headers[16];
    size_t header_count;
} qdc_response;
typedef struct {
    const char *data_dir, *at_path, *listen, *port;
    unsigned poll_ms;
    bool debug;
} qdc_config;
typedef struct qdc_app {
    qdc_config config;
    pthread_mutex_t action_mu, op_mu;
    atomic_bool stopping;
    bool maintenance; /* action_mu */
    void *domain, *auth, *clock, *push, *monitor, *audio;
} qdc_app;
typedef struct {
    char key[128], iccid[32], imsi[32], number[96];
} qdc_sim_identity;

void qdc_error_set(qdc_error *, const char *, ...);
void qdc_buf_free(qdc_buf *);
int qdc_buf_reserve(qdc_buf *, size_t);
int qdc_buf_append(qdc_buf *, const void *, size_t);
int qdc_buf_puts(qdc_buf *, const char *);
int qdc_buf_printf(qdc_buf *, const char *, ...);
qdc_slice qdc_json_slice(yyjson_val *);
bool qdc_slice_eq(qdc_slice, const char *);
/* Checked copy: embedded NUL/overlength rejected, never silently truncated. */
int qdc_json_cstr(yyjson_val *, char *, size_t);
yyjson_doc *qdc_json_read(const void *, size_t, qdc_error *);
yyjson_val *qdc_request_json(qdc_request *, qdc_error *);
yyjson_mut_doc *qdc_json_object(void);
yyjson_mut_doc *qdc_json_array(void);
/* Replace invalid UTF-8 exactly as Go encoding/json, preserving U+0000. */
yyjson_mut_val *qdc_json_string(yyjson_mut_doc *, const void *, size_t);
bool qdc_json_add_string(yyjson_mut_doc *, yyjson_mut_val *, const char *, const void *, size_t);
int qdc_response_json(qdc_response *, int, yyjson_mut_doc *); /* consumes doc */
int qdc_response_error(qdc_response *, int, const char *);
int qdc_response_ok(qdc_response *, int);
int qdc_response_header(qdc_response *, const char *, const char *);
void qdc_response_free(qdc_response *);
int qdc_query(const char *, const char *, qdc_buf *);
int qdc_cookie(const char *, const char *, char *, size_t);
uint64_t qdc_mono_ms(void);
int qdc_cond_init(pthread_cond_t *);
int qdc_cond_until(pthread_cond_t *, pthread_mutex_t *, uint64_t mono_deadline);
int64_t qdc_system_ms(void);
void qdc_rfc3339(int64_t, char out[40]);
int qdc_parse_rfc3339(qdc_slice, int64_t *);
int qdc_path(char *, size_t, const char *, const char *);
int qdc_mkdir_private(const char *, qdc_error *);
/* 0 success; 1 missing; -1 unreadable/too large. buf NUL-terminated for convenience. */
int qdc_read_file(const char *, size_t, qdc_buf *, qdc_error *);
/* rename+parent fsync. error.uncertain=true if rename committed but dir sync failed. */
int qdc_atomic_write(const char *, const void *, size_t, qdc_error *);
int qdc_save_json(const char *, yyjson_mut_doc *, qdc_error *); /* borrows doc */
int qdc_private_file(const char *, size_t, qdc_buf *, qdc_error *);
void qdc_log(const char *); /* fixed diagnostic text only, no credentials/content */
void qdc_sleep(qdc_app *, unsigned);

/* Crypto functions return 0 success, -1 failure; randomness fails closed. */
int qdc_random(void *, size_t, qdc_error *);
int qdc_random_hex(char *, size_t bytes, qdc_error *); /* destination >=2*bytes+1 */
int qdc_sha256(const void *, size_t, unsigned char out[32]);
int qdc_hmac256(const void *, size_t, const void *, size_t, unsigned char out[32]);
int qdc_pbkdf2(qdc_slice password, qdc_slice salt, unsigned iterations, unsigned char out[32]);
bool qdc_constant_equal(const void *, const void *, size_t);

/* Module lifecycle: init does not spawn workers; start only after all init. */
int qdc_clock_init(qdc_app *, qdc_error *);
void qdc_clock_free(qdc_app *);
int64_t qdc_now_ms(qdc_app *);
int qdc_clock_sync(qdc_app *, int64_t, qdc_error *);
int qdc_auth_init(qdc_app *, qdc_error *);
void qdc_auth_free(qdc_app *);
int qdc_auth_bind(qdc_app *, const char *key, qdc_error *);
bool qdc_auth_check(qdc_app *, const char *cookie, const char *key);
int qdc_auth_require(qdc_app *, qdc_request *, qdc_response *);
int qdc_auth_route(qdc_app *, qdc_request *, qdc_response *); /* 1 handled, 0 other */
int qdc_backup_route(qdc_app *, qdc_request *, qdc_response *);

int qdc_domain_init(qdc_app *, qdc_error *);
int qdc_domain_start(qdc_app *, qdc_error *);
void qdc_domain_free(qdc_app *); /* stops/joins all domain threads */
int qdc_domain_route(qdc_app *, qdc_request *, qdc_response *);
/* fresh=true requires caller holding op_mu; always probes ICCID, binds auth/push.
 * cached=false path owns any needed op_mu itself; never call with op_mu held. */
int qdc_domain_identity(qdc_app *, bool fresh, qdc_sim_identity *, qdc_error *);
int qdc_command(qdc_app *, const char *, unsigned timeout_ms, qdc_buf *, qdc_error *);
bool qdc_outbox_busy(qdc_app *);
bool qdc_calls_busy(qdc_app *);
yyjson_doc *qdc_calls_snapshot(qdc_app *); /* owned; root object */
int qdc_calls_hangup(qdc_app *, const char *id, qdc_error *);
yyjson_doc *qdc_pin_snapshot(qdc_app *);
yyjson_doc *qdc_messages(qdc_app *);      /* owned; root sorted array */
yyjson_doc *qdc_push_messages(qdc_app *); /* owned; array of {key,direction}, no bodies */
/* restore called with action_mu/op_mu; validates all records first, backs up,
 * merges index=0/queued->uncertain, saves; no physical send. */
int qdc_store_restore(qdc_app *, yyjson_val *array, int *added, qdc_error *);
void qdc_domain_last_error(qdc_app *, char *, size_t);

int qdc_push_init(qdc_app *, qdc_error *);
int qdc_push_start(qdc_app *, qdc_error *);
void qdc_push_free(qdc_app *);
void qdc_push_bind(qdc_app *, const char *iccid);
int qdc_push_route(qdc_app *, qdc_request *, qdc_response *);

int qdc_monitor_init(qdc_app *, qdc_error *);
int qdc_monitor_start(qdc_app *, qdc_error *);
void qdc_monitor_free(qdc_app *);
int qdc_monitor_route(qdc_app *, qdc_request *, qdc_response *);
int qdc_traffic_save(qdc_app *, qdc_error *);
int qdc_audio_init(qdc_app *, qdc_error *);
int qdc_audio_start(qdc_app *, qdc_error *);
void qdc_audio_free(qdc_app *);
bool qdc_audio_available(qdc_app *);
yyjson_doc *qdc_audio_snapshot(qdc_app *);
int qdc_audio_route(qdc_app *, qdc_request *, qdc_response *);

int qdc_dispatch(qdc_app *, qdc_request *, qdc_response *);
int qdc_http_serve(qdc_app *, qdc_error *);
void qdc_http_conn(qdc_app *, int); /* owns fd, exposed for socketpair tests */
int qdc_listen_address(const char *, const char *, qdc_error *);

typedef struct {
    const char *path, *type;
    const unsigned char *data;
    size_t len;
} qdc_asset;
extern const qdc_asset qdc_assets[];
extern const size_t qdc_asset_count;
#endif
