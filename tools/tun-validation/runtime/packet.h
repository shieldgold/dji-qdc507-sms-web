#ifndef QDC_TUN_PACKET_H
#define QDC_TUN_PACKET_H
#include <stddef.h>
#include <stdint.h>
uint16_t qdc_checksum(const void *, size_t);
size_t qdc_packet(unsigned char *, size_t, uint32_t, uint32_t, uint16_t,
                  uint16_t, const unsigned char *, size_t);
int qdc_match(const unsigned char *, size_t, uint32_t, uint32_t, uint16_t,
              uint16_t, const unsigned char *, size_t);
#endif
