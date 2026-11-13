#include "packet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  unsigned char odd[] = {1, 2, 3};
  assert(qdc_checksum(odd, 3) == 0xfbfd);
  unsigned char p[1024];
  uint32_t src = 0x010200c0, dst = 0x020200c0;
  unsigned char msg[] = "R20261113:1:payload";
  size_t n = qdc_packet(p, sizeof(p), src, dst, 39000, 39001, msg, sizeof(msg));
  assert(n == 28 + sizeof(msg));
  assert(qdc_match(p, n, src, dst, 39000, 39001, msg, sizeof(msg)));
  p[n - 1] ^= 1;
  assert(!qdc_match(p, n, src, dst, 39000, 39001, msg, sizeof(msg)));
  p[n - 1] ^= 1;
  assert(!qdc_match(p, n - 1, src, dst, 39000, 39001, msg, sizeof(msg)));
  assert(!qdc_match(p, n, src, dst, 39001, 39000, msg, sizeof(msg)));
  assert(qdc_packet(p, 16, src, dst, 1, 2, msg, sizeof(msg)) == 0);
  assert(qdc_packet(p, sizeof(p), src, dst, 1, 2, msg, 257) == 0);
  puts("packet checks: checksum, encode, decode, corruption, truncation, "
       "ports, bounds passed");
  return 0;
}
