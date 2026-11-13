#ifndef QDC_SIGNAL_GUARD_H
#define QDC_SIGNAL_GUARD_H
#include <pthread.h>
#include <signal.h>

// Go's asynchronous preemption uses SIGURG. A synchronous IOKit operation
// interrupted by a signal returns kIOReturnAborted. Defer only SIGURG on this
// C thread, then restore the exact previous mask before returning to Go.
// Other signals and all genuine I/O errors retain their normal behavior.
static inline int qdc_signal_enter(sigset_t *old) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGURG);
  return pthread_sigmask(SIG_BLOCK, &set, old);
}
static inline int qdc_signal_leave(const sigset_t *old) {
  return pthread_sigmask(SIG_SETMASK, old, NULL);
}
#endif
