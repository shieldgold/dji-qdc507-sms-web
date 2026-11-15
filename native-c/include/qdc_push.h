#ifndef QDC_PUSH_H
#define QDC_PUSH_H
#include "qdc.h"

#define QDC_APNS_HOST "api.sandbox.push.apple.com"
#define QDC_CLOCK_HOST "www.apple.com"
#define QDC_CLOCK_PATH "/library/test/success.html"
#define QDC_APNS_TOPIC "com.shield.modulephone"

typedef struct {
    const char *name, *value;
} qdc_tls_header;
typedef struct {
    qdc_app *app;
    const char *host, *method, *path, *ca_file;
    const qdc_tls_header *headers;
    size_t header_count;
    qdc_slice body;
    unsigned timeout_ms;
    bool require_h2;
} qdc_tls_request;
typedef struct {
    int status;
    bool http2;
    char date[128];
    qdc_buf body;
} qdc_tls_response;
int qdc_tls_fetch(const qdc_tls_request *, qdc_tls_response *, qdc_error *);
void qdc_tls_response_free(qdc_tls_response *);
int qdc_http_date(qdc_slice, int64_t *);
/* DNS decoder is independent of sockets so malformed packets can be tested. */
int qdc_dns_answer(const unsigned char *, size_t, uint16_t, const char *, char address[16],
                   char cname[254], bool *, qdc_error *);
int qdc_dns_resolve(const char *, char address[16], uint64_t deadline, qdc_error *);

typedef struct {
    char iccid[32], device_id[81], token[513], voip_token[513];
    bool enabled, calls_enabled;
} qdc_push_subscription;
typedef struct {
    char kind[8], event_id[37], token[513], call_id[25], call_uuid[37];
    int count;
    int64_t sent_at;
} qdc_push_delivery;
typedef enum { QDC_PUSH_OK, QDC_PUSH_RETRY, QDC_PUSH_PERMANENT } qdc_push_result;
void qdc_call_push_uuid(const char *, char out[37]);
int qdc_apns_payload(const qdc_push_delivery *, qdc_buf *, qdc_error *);
int qdc_push_register(qdc_app *, const qdc_push_subscription *, yyjson_val *, qdc_error *);
int qdc_push_observe(qdc_app *, yyjson_val *messages, yyjson_val *calls, int64_t now_ms,
                     qdc_error *);
void qdc_push_send_next(qdc_app *, const char *iccid, int64_t now_ms);
yyjson_mut_doc *qdc_push_status(qdc_app *);

#ifdef QDC_TESTING
/* Hooks are absent from production builds and accept numeric loopback only. */
int qdc_tls_test_endpoint(const char *, unsigned port, const char *server_name);
int qdc_dns_test_server(const char *, unsigned port);
typedef qdc_push_result (*qdc_push_test_deliver_fn)(const qdc_push_delivery *, void *, qdc_error *);
void qdc_push_test_deliver(qdc_app *, qdc_push_test_deliver_fn, void *);
int qdc_push_test_event(qdc_app *, const char *kind, int64_t now_ms, qdc_error *);
int qdc_push_test_jwt(qdc_app *, int64_t now_ms, qdc_buf *, qdc_error *);
#endif
#endif
