#include "qdc.h"
#include <string.h>

typedef struct {
    const char *method, *path;
    bool authenticated;
} route;
static const route routes[] = {{"GET", "/api/auth/status", false},
                               {"GET", "/api/push/status", true},
                               {"POST", "/api/push/register", true},
                               {"POST", "/api/push/test", true},
                               {"POST", "/api/time", true},
                               {"POST", "/api/maintenance/rollback", true},
                               {"POST", "/api/auth/setup", false},
                               {"POST", "/api/auth/login", false},
                               {"POST", "/api/auth/browser/ticket", true},
                               {"POST", "/api/auth/browser/redeem", false},
                               {"POST", "/api/auth/logout", false},
                               {"POST", "/api/auth/change", true},
                               {"GET", "/api/overview", true},
                               {"POST", "/api/diagnostics", true},
                               {"GET", "/api/export", true},
                               {"POST", "/api/restore", true},
                               {"GET", "/api/state", true},
                               {"GET", "/api/send/status", true},
                               {"POST", "/api/send", true},
                               {"POST", "/api/delete", true},
                               {"GET", "/api/sim", true},
                               {"GET", "/api/sim/binding", false},
                               {"GET", "/api/calls", true},
                               {"POST", "/api/calls/dial", true},
                               {"POST", "/api/calls/answer", true},
                               {"POST", "/api/calls/hangup", true},
                               {"POST", "/api/calls/dtmf", true},
                               {"POST", "/api/calls/audio", true},
                               {"POST", "/api/sim/unlock", true},
                               {"POST", "/api/sim/retry", true}};
int qdc_dispatch(qdc_app *a, qdc_request *r, qdc_response *s) {
    const route *match = NULL;
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        if (!strcmp(r->method, routes[i].method) && !strcmp(r->path, routes[i].path)) {
            match = &routes[i];
            break;
        }
    if (match) {
        if (match->authenticated) {
            int auth = qdc_auth_require(a, r, s);
            if (auth)
                return auth < 0 ? -1 : 0;
        }
        int n = qdc_auth_route(a, r, s);
        if (n)
            return n < 0 ? -1 : 0;
        n = qdc_backup_route(a, r, s);
        if (n)
            return n < 0 ? -1 : 0;
        n = qdc_audio_route(a, r, s);
        if (n)
            return n < 0 ? -1 : 0;
        n = qdc_domain_route(a, r, s);
        if (n)
            return n < 0 ? -1 : 0;
        n = qdc_push_route(a, r, s);
        if (n)
            return n < 0 ? -1 : 0;
        n = qdc_monitor_route(a, r, s);
        if (n)
            return n < 0 ? -1 : 0;
        return qdc_response_error(s, 500, "接口实现未接入");
    }
    if (!strcmp(r->method, "GET"))
        for (size_t i = 0; i < qdc_asset_count; i++)
            if (!strcmp(r->path, qdc_assets[i].path)) {
                s->status = 200;
                s->content_type = qdc_assets[i].type;
                s->static_body = qdc_assets[i].data;
                s->static_len = qdc_assets[i].len;
                return 0;
            }
    s->status = 404;
    s->content_type = "text/plain; charset=utf-8";
    return qdc_buf_puts(&s->body, "not found");
}
