#include "lab_mem.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  struct qdc_mem m;
  assert(qdc_parse_mem("Name:\tcharon\nVmSize:\t99999 kB\nVmRSS:\t123 kB\nVmHWM: 456 kB\nThreads: 8\n", &m) == 0);
  assert(m.rss == 123 && m.hwm == 456 && m.vsize == 99999 && m.threads == 8);
  assert(qdc_parse_mem("VmSize: 8888 kB\nThreads: 2\n", &m) != 0);
  struct qdc_smaps sm;
  assert(qdc_parse_smaps("Size: 99999 kB\nRss: 64 kB\nPss: 64 kB\nAnonymous: 16 kB\nPrivate_Dirty: 64 kB\nRss: 32 kB\nPss: 32 kB\nAnonymous: 4 kB\nPrivate_Dirty: 32 kB\n", &sm) == 0);
  assert(sm.rss == 96 && sm.pss == 96 && sm.anonymous == 20 && sm.dirty == 96);
  assert(qdc_parse_smaps("VmRSS: 999 kB\n", &sm) != 0);
  puts("MEM_PARSER_OK");
  return 0;
}
