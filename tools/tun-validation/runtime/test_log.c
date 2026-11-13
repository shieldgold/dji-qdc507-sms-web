#include "log_watch.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  uint64_t seq = 0;
  int seen = 0;
  assert(!qdc_log_problem("6,101,100,-;normal\n", &seq, &seen));
  assert(seq == 101 && seen);
  assert(!qdc_log_problem("6,102,101,-;normal\n", &seq, &seen));
  assert(qdc_log_problem("6,104,102,-;normal\n", &seq, &seen));
  seen = 0;
  assert(qdc_log_problem("4,200,300,-;WARNING: example\n", &seq, &seen));
  seen = 0;
  assert(qdc_log_problem("not a kmsg record", &seq, &seen));
  puts("kmsg checks: sequence, gap, warning and malformed record passed");
  return 0;
}
