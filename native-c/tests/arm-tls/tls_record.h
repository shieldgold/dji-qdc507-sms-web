#ifndef QDC_ARM_TLS_RECORD_H
#define QDC_ARM_TLS_RECORD_H
#include <stddef.h>
#include <string.h>

/* Observes only the framing and two plaintext alert bytes. No handshake,
 * application data, secrets or random bytes are retained or logged. */
struct lab_tls_records {
    unsigned char header[5], alert[2];
    size_t header_used, body_used, body_length;
    unsigned certificate_alert, description, malformed;
};
static inline void lab_tls_records_feed(struct lab_tls_records *r,
                                        const unsigned char *p, size_t n) {
    while (n && !r->malformed) {
        if (r->header_used < 5) {
            r->header[r->header_used++] = *p++;
            n--;
            if (r->header_used < 5) continue;
            r->body_length = ((size_t)r->header[3] << 8) | r->header[4];
            r->body_used = 0;
            memset(r->alert, 0, sizeof(r->alert));
            if (r->body_length > 18432 || r->header[1] != 3 || r->header[2] > 3) {
                r->malformed = 1;
                return;
            }
            if (!r->body_length) r->header_used = 0;
            continue;
        }
        if (r->header[0] == 21 && r->body_used < 2) r->alert[r->body_used] = *p;
        p++; n--; r->body_used++;
        if (r->body_used == r->body_length) {
            if (r->header[0] == 21 && r->header[2] == 3 && r->body_length == 2 &&
                r->alert[0] == 2 && (r->alert[1] == 42 || r->alert[1] == 46 || r->alert[1] == 48)) {
                r->certificate_alert = 1;
                r->description = r->alert[1];
            }
            r->header_used = 0;
        }
    }
}
#endif
