#ifndef QDC_ICMP_PROBE_H
#define QDC_ICMP_PROBE_H

#include <stddef.h>
#include <stdint.h>

#define QDC_ICMP_PAYLOAD_SIZE 128u

/* Linux IPv4 raw-socket echo. IPv4 literals only; no DNS, fork or output.
 * Sends one 128-byte payload and waits at most 1000 ms in <=100 ms polls.
 * pump is required, must not block, and returns 0 to continue; any other
 * return aborts immediately. The caller retains its monitoring schedule.
 * Returns 0 for a matching reply or -1 for any failure; always closes fd.
 */
int qdc_icmp_once(const char *source, const char *target,
                  int (*pump)(void *), void *opaque);

/* Internal pure parser, public for unprivileged CPU tests.
 * source/target are the outbound in_addr.s_addr values (network byte order).
 * identifier/sequence are host byte order. Input includes a wire IPv4 header.
 * Returns 1 only for a complete, checksum-valid, unfragmented echo reply.
 */
int qdc_icmp_reply_matches(const unsigned char *packet, size_t length,
                           uint32_t source, uint32_t target,
                           uint16_t identifier, uint16_t sequence,
                           const unsigned char payload[QDC_ICMP_PAYLOAD_SIZE]);

#endif
