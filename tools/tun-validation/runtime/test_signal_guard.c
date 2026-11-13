#include "host/signal_guard.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/select.h>
#include <unistd.h>

static volatile sig_atomic_t delivered;
static void handler(int sig) { (void)sig; delivered++; }
static void *inject(void *arg) {
  usleep(20000);
  assert(pthread_kill(*(pthread_t *)arg, SIGURG) == 0);
  return NULL;
}
int main(void) {
  struct sigaction action = {0};
  action.sa_handler = handler;
  sigemptyset(&action.sa_mask);
  assert(sigaction(SIGURG, &action, NULL) == 0);
  sigset_t original, during, restored, old;
  assert(pthread_sigmask(SIG_SETMASK, NULL, &original) == 0);
  assert(qdc_signal_enter(&old) == 0);
  assert(pthread_sigmask(SIG_SETMASK, NULL, &during) == 0);
  assert(sigismember(&during, SIGURG) == 1);
  // No other signal's mask may change, particularly termination signals.
  for (int s = 1; s < NSIG; s++) {
    if (s != SIGURG) assert(sigismember(&during, s) == sigismember(&original, s));
  }
  pthread_t reader = pthread_self(), sender;
  assert(pthread_create(&sender, NULL, inject, &reader) == 0);
  struct timeval timeout = {0, 100000};
  assert(select(0, NULL, NULL, NULL, &timeout) == 0);
  assert(delivered == 0);
  assert(qdc_signal_leave(&old) == 0);
  assert(pthread_join(sender, NULL) == 0);
  assert(delivered == 1);
  assert(pthread_sigmask(SIG_SETMASK, NULL, &restored) == 0);
  for (int s = 1; s < NSIG; s++) assert(sigismember(&restored, s) == sigismember(&original, s));
  puts("SIGNAL_GUARD_OK");
  return 0;
}
