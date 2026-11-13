#ifndef QDC_IKE_PROC_H
#define QDC_IKE_PROC_H
#include <stddef.h>
#include <sys/types.h>
#include "../lab_mem.h"
struct proc_id { pid_t pid; unsigned long long start; char state; char exe[512]; };
ssize_t read_complete(const char *, char *, size_t);
int proc_identity(pid_t, struct proc_id *);
int proc_matches(pid_t, unsigned long long, const char *);
int supervisor_matches(pid_t, unsigned long long);
long memory_available(void);
int memory_safe(long, long);
int proc_smaps(pid_t, struct qdc_smaps *);
#endif
