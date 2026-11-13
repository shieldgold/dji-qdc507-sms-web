#ifndef QDC_LOG_WATCH_H
#define QDC_LOG_WATCH_H
#include <stdint.h>
int qdc_log_problem(const char *, uint64_t *, int *);
int qdc_watch(int, const char *, int, int, char **);
#endif
