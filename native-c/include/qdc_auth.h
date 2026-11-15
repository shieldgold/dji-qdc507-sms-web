#ifndef QDC_AUTH_H
#define QDC_AUTH_H
#include "qdc.h"
/* Explicit slices preserve decoded JSON NULs and UTF-8 password byte lengths. */
bool qdc_auth_registered(qdc_app *, const char *);
bool qdc_auth_token_valid(qdc_app *, const char *, const char *);
int qdc_auth_setup(qdc_app *, const qdc_sim_identity *, qdc_slice, bool, char token[65],
                   qdc_error *);
int qdc_auth_login(qdc_app *, const char *, qdc_slice, bool, char token[65], qdc_error *);
int qdc_auth_logout(qdc_app *, const char *, qdc_error *);
int qdc_auth_change(qdc_app *, const char *, qdc_slice, qdc_slice, qdc_error *);
int qdc_auth_issue(qdc_app *, const char *, bool, char token[65], qdc_error *);
int qdc_auth_ticket(qdc_app *, const char *, const char *, char ticket[65], qdc_error *);
int qdc_auth_redeem(qdc_app *, const char *, const char *, char token[65], qdc_error *);
/* clock route helper; caller applies common authentication before routing. */
int qdc_clock_route(qdc_app *, qdc_request *, qdc_response *);
/* Tests inject filesystem/launcher results; defaults never run outside /data/smsweb. */
typedef struct {
    int (*previous_available)(const char *path, void *context);
    int (*launch)(const char *script, void *context);
    void *context;
} qdc_rollback_ops;
int qdc_rollback_with_ops(qdc_app *, qdc_response *, const qdc_rollback_ops *);
#endif
