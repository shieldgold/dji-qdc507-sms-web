#ifndef QDC_HTTP_PROBE_H
#define QDC_HTTP_PROBE_H

/* Read-only address check. No socket or request is created. */
int qdc_http_local_address(const char *address);

/* Fixed GET / on local TCP 8787. Emits metadata only; never response content. */
int qdc_http_once(const char *address);

#endif
