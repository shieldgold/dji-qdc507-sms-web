#include "log_watch.h"
#include <stdlib.h>
int main(int argc, char **argv) {
  if (argc < 2)
    return 64;
  return qdc_watch(atoi(getenv("QDC_TEST_LOG_FD")), "__READY_TEST__", 0, 1,
                   argv + 1);
}
