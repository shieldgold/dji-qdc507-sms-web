#ifndef QDC_HTTP_PROBE_H
#define QDC_HTTP_PROBE_H
#include <stddef.h>

struct qdc_http_result { int status, transport; size_t body; };
enum qdc_http_status { QDC_HTTP_OK = 0, QDC_HTTP_FAILED = 1, QDC_HTTP_CANCELLED = 2 };

/* Read-only address check. No socket or request is created. */
int qdc_http_local_address(const char *address);

/* Fixed GET / on local TCP 8787. Emits metadata only; never response content. */
int qdc_http_once(const char *address);

/* No fork or output. pump runs before/after bounded socket polls, including
 * stalled peers. A nonzero pump result returns QDC_HTTP_CANCELLED; callers
 * must not retry it as an ordinary connection failure. Every exit closes fd. */
int qdc_http_run(const char *address, int (*pump)(void *), void *opaque,
                 struct qdc_http_result *result);

#endif
