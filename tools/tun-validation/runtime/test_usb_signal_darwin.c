// Host-only diagnostic: reads idle ADB IN, never sends an ADB command.
// Compile explicitly; do not run while any other controller owns the interface.
#include "host/usb_darwin.h"
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t delivered;
static void handler(int sig) { (void)sig; delivered++; }
static void *inject(void *arg) {
  usleep(100000);
  pthread_kill(*(pthread_t *)arg, SIGURG);
  return NULL;
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  int masked = atoi(argv[1]);
  struct sigaction action = {0};
  action.sa_handler = handler;
  sigemptyset(&action.sa_mask);
  sigaction(SIGURG, &action, NULL);
  qdc_usb *h = qdc_open(0x2ca3, 0x4006, 0x100000);
  if (!h) { fprintf(stderr, "%s\n", qdc_error()); return 1; }
  int failures = 0;
  for (int n = 0; n < 3; n++) {
    pthread_t reader = pthread_self(), sender;
    sigset_t set, old;
    sigemptyset(&set); sigaddset(&set, SIGURG);
    if (masked) pthread_sigmask(SIG_BLOCK, &set, &old);
    pthread_create(&sender, NULL, inject, &reader);
    uint8_t buffer[4096]; uint32_t count = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int result = qdc_read(h, buffer, sizeof(buffer), 1000, &count);
    if (result) failures++;
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (masked) pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_join(sender, NULL);
    double ms = (end.tv_sec-start.tv_sec)*1000.0 + (end.tv_nsec-start.tv_nsec)/1e6;
    printf("masked=%d round=%d result=%d bytes=%u elapsed_ms=%.3f signals=%d error=%s\n",
           masked, n, result, count, ms, delivered, result ? qdc_error() : "none");
    fflush(stdout);
  }
  qdc_close(h);
  return failures != 0;
}
