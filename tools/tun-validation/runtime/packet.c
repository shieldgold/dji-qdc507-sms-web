#include "packet.h"
#include <string.h>
static uint32_t words(const unsigned char *p, size_t n) {
  uint32_t sum = 0;
  while (n >= 2) {
    sum += ((uint32_t)p[0] << 8) | p[1];
    p += 2;
    n -= 2;
  }
  if (n)
    sum += (uint32_t)p[0] << 8;
  return sum;
}
static uint16_t finish(uint32_t sum) {
  while (sum >> 16)
    sum = (sum & 65535) + (sum >> 16);
  return (uint16_t)~sum;
}
uint16_t qdc_checksum(const void *p, size_t n) { return finish(words(p, n)); }
static void put16(unsigned char *p, uint16_t x) {
  p[0] = (unsigned char)(x >> 8);
  p[1] = (unsigned char)x;
}
static uint16_t get16(const unsigned char *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}
size_t qdc_packet(unsigned char *out, size_t cap, uint32_t src, uint32_t dst,
                  uint16_t sport, uint16_t dport, const unsigned char *payload,
                  size_t n) {
  size_t len = 28 + n;
  if (n > 256 || len > cap)
    return 0;
  memset(out, 0, len);
  out[0] = 0x45;
  put16(out + 2, (uint16_t)len);
  out[8] = 64;
  out[9] = 17;
  memcpy(out + 12, &src, 4);
  memcpy(out + 16, &dst, 4);
  put16(out + 10, qdc_checksum(out, 20));
  put16(out + 20, sport);
  put16(out + 22, dport);
  put16(out + 24, (uint16_t)(8 + n));
  memcpy(out + 28, payload, n);
  uint16_t check = finish(words(out + 12, 8) + 17 + (uint32_t)(8 + n) +
                          words(out + 20, 8 + n));
  put16(out + 26, check ? check : 65535);
  return len;
}
int qdc_match(const unsigned char *p, size_t len, uint32_t src, uint32_t dst,
              uint16_t sport, uint16_t dport, const unsigned char *payload,
              size_t n) {
  if (n > 256 || len != 28 + n || p[0] != 0x45 || get16(p + 2) != len ||
      p[9] != 17 || (get16(p + 6) & 0xbfff) || qdc_checksum(p, 20))
    return 0;
  if (memcmp(p + 12, &src, 4) || memcmp(p + 16, &dst, 4) ||
      get16(p + 20) != sport || get16(p + 22) != dport ||
      get16(p + 24) != 8 + n || get16(p + 26) == 0)
    return 0;
  if (finish(words(p + 12, 8) + 17 + (uint32_t)(8 + n) + words(p + 20, 8 + n)))
    return 0;
  return !memcmp(p + 28, payload, n);
}
