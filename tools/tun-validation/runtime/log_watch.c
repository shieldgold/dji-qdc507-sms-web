#define _GNU_SOURCE
#include "log_watch.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    return -1;
  return t.tv_sec + t.tv_nsec / 1e9;
}
int qdc_log_problem(const char *text, uint64_t *seq, int *seen) {
  const char *comma = strchr(text, ',');
  if (!comma)
    return 1;
  char *end = NULL;
  errno = 0;
  unsigned long long n = strtoull(comma + 1, &end, 10);
  if (errno || end == comma + 1 || *end != ',')
    return 1;
  int gap = *seen && n != *seq + 1;
  *seq = n;
  *seen = 1;
  const char *bad[] = {"WARNING:",
                       "BUG:",
                       "Oops:",
                       "Unable to handle kernel",
                       "Out of memory",
                       "page allocation failure",
                       "detected stalls",
                       "RCU stall",
                       NULL};
  for (int i = 0; bad[i]; i++)
    if (strstr(text, bad[i]))
      return 1;
  return gap;
}
static long available(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f)
    return -1;
  char line[160];
  long n = -1;
  while (fgets(line, sizeof(line), f))
    if (sscanf(line, "MemAvailable: %ld kB", &n) == 1)
      break;
  fclose(f);
  return n;
}
static long rss(pid_t pid) {
  char path[80], line[160];
  snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  long value = 0;
  while (fgets(line, sizeof(line), f))
    if (sscanf(line, "VmRSS: %ld kB", &value) == 1)
      break;
  fclose(f);
  return value;
}
static int wait_go(void) {
  double end = now() + 5;
  char input[8];
  size_t count = 0;
  while (now() < end && count < sizeof(input)) {
    struct pollfd p = {STDIN_FILENO, POLLIN, 0};
    int rc = poll(&p, 1, 100);
    if (rc < 0 && errno == EINTR)
      continue;
    if (rc < 0)
      return -1;
    if (!rc)
      continue;
    ssize_t n = read(STDIN_FILENO, input + count, 1);
    if (n != 1)
      return -1;
    if (input[count++] == '\n')
      return count == 3 && !memcmp(input, "GO\n", 3) ? 0 : -1;
  }
  return -1;
}
int qdc_watch(int fd, const char *token, int observe, int limit, char **argv) {
  if (fd < 0 || !token || strlen(token) > 96 ||
      strspn(
          token,
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
          strlen(token) ||
      observe < 0 || observe > 30 || limit < 1 || limit > 10 || !argv ||
      !argv[0] || argv[0][0] != '/')
    return 64;
  if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    return 65;
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("LOGGER_READY %s pid=%ld\n", token, (long)getpid());
  if (wait_go()) {
    puts("LOGGER_NOT_ARMED");
    return 66;
  }
  pid_t child = fork();
  if (child < 0) {
    perror("fork");
    return 67;
  }
  if (!child) {
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) {
      dup2(null, STDIN_FILENO);
      if (null != STDIN_FILENO)
        close(null);
    }
    execv(argv[0], argv);
    perror("execv");
    _exit(127);
  }
  printf("COMMAND_PID %ld\n", (long)child);
  double start = now(), finished = -1, last_sample = -1;
  long baseline = available();
  int status = 0, exited = 0, seen = 0;
  uint64_t seq = 0;
  size_t total = 0;
  for (;;) {
    char record[8193];
    int problem = 0;
    for (int i = 0; i < 128; i++) {
      ssize_t n = read(fd, record, sizeof(record) - 1);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        break;
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        printf("KMSG_READ_ERROR errno=%d\n", errno);
        problem = 1;
        break;
      }
      record[n] = 0;
      printf("KMSG %s", record);
      total += (size_t)n;
      if (qdc_log_problem(record, &seq, &seen) || total > 1024 * 1024) {
        puts("KMSG_UNSAFE_OR_GAP");
        problem = 1;
        break;
      }
    }
    if (problem) {
      if (!exited)
        kill(child, SIGSTOP);
      printf("UNSAFE_STOP child=%ld exited=%d; no driver cleanup attempted\n",
             (long)child, exited);
      return 86;
    }
    double t = now();
    if (t - last_sample >= 1) {
      long mem = available();
      printf("MEM available_kb=%ld watch_rss_kb=%ld child_rss_kb=%ld "
             "elapsed=%.3f\n",
             mem, rss(getpid()), exited ? 0 : rss(child), t - start);
      last_sample = t;
      if (mem < 0 || mem < 6144 || (baseline >= 0 && baseline - mem > 4096)) {
        if (!exited)
          kill(child, SIGTERM);
        puts("MEMORY_STOP; command may still be in kernel");
        return 87;
      }
    }
    if (!exited) {
      pid_t r = waitpid(child, &status, WNOHANG);
      if (r == child) {
        exited = 1;
        finished = t;
        printf("COMMAND_EXIT status=%d\n", status);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
          return WIFEXITED(status) ? WEXITSTATUS(status) : 88;
      } else if (r < 0) {
        perror("waitpid");
        return 89;
      } else if (t - start >= limit) {
        kill(child, SIGTERM);
        printf("COMMAND_TIMEOUT child=%ld; operation not rolled back\n",
               (long)child);
        return 124;
      }
    }
    if (exited && t - finished >= observe) {
      puts("LOGGER_COMPLETE");
      return 0;
    }
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 100) < 0 && errno != EINTR) {
      perror("poll");
      return 90;
    }
  }
}
