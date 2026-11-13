// Computer-only Linux ARM VM measurement. Never used on the QDC507 module.
#define _GNU_SOURCE
#include "lab_mem.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int qdc_parse_mem(const char *text, struct qdc_mem *m) {
  m->rss = m->hwm = m->vsize = m->threads = -1;
  while (text && *text) {
    if (sscanf(text, "VmRSS: %ld kB", &m->rss) == 1) {}
    else if (sscanf(text, "VmHWM: %ld kB", &m->hwm) == 1) {}
    else if (sscanf(text, "VmSize: %ld kB", &m->vsize) == 1) {}
    else (void)sscanf(text, "Threads: %ld", &m->threads);
    text = strchr(text, '\n');
    if (text) text++;
  }
  return m->rss < 0 || m->hwm < 0 || m->vsize < 0 || m->threads < 1 ? -1 : 0;
}
int qdc_parse_smaps(const char *text, struct qdc_smaps *m) {
  memset(m, 0, sizeof(*m)); int seen = 0; long n;
  while (text && *text) {
    if (sscanf(text, "Rss: %ld kB", &n) == 1) { m->rss += n; seen++; }
    else if (sscanf(text, "Pss: %ld kB", &n) == 1) m->pss += n;
    else if (sscanf(text, "Anonymous: %ld kB", &n) == 1) m->anonymous += n;
    else if (sscanf(text, "Private_Dirty: %ld kB", &n) == 1) m->dirty += n;
    else if (sscanf(text, "Private_Clean: %ld kB", &n) == 1) m->clean += n;
    text = strchr(text, '\n'); if (text) text++;
  }
  return seen ? 0 : -1;
}
#ifndef QDC_MEM_TEST
static long available(void) {
  FILE *f = fopen("/proc/meminfo", "r"); char s[256]; long n = -1;
  if (!f) return -1;
  while (fgets(s, sizeof(s), f)) if (sscanf(s, "MemAvailable: %ld kB", &n) == 1) break;
  fclose(f); return n;
}
static int smaps(pid_t pid, struct qdc_smaps *m) {
  char path[80]; static char text[131072];
  snprintf(path, sizeof(path), "/proc/%ld/smaps", (long)pid);
  int fd = open(path, O_RDONLY); if (fd < 0) return -1;
  size_t used = 0;
  for (;;) {
    ssize_t n = read(fd, text + used, sizeof(text)-1-used);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { close(fd); return -1; }
    if (!n) break;
    used += (size_t)n;
    if (used == sizeof(text)-1) { close(fd); return -1; }
  }
  close(fd); text[used] = 0;
  return used ? qdc_parse_smaps(text, m) : -1;
}
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  puts("MEASUREMENT_METHOD smaps_read_to_eof=1");
  if (argc >= 4 && !strcmp(argv[1], "run")) {
    pid_t pid = fork();
    if (pid < 0) return 2;
    if (!pid) { execv(argv[3], argv + 3); perror("execv"); _exit(127); }
    struct rusage usage; int status; long sampled = 0, anon_peak = 0;
    for (;;) {
      pid_t got = wait4(pid, &status, WNOHANG, &usage);
      if (got == pid) break;
      if (got < 0 && errno != EINTR) return 2;
      struct qdc_smaps sm;
      if (!smaps(pid, &sm)) {
        if (sm.rss > sampled) sampled = sm.rss;
        if (sm.anonymous > anon_peak) anon_peak = sm.anonymous;
      }
      struct timespec delay = {0, 10000000}; nanosleep(&delay, NULL);
    }
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    long peak = usage.ru_maxrss > sampled ? usage.ru_maxrss : sampled;
    printf("CLI_MEMORY label=%s maxrss_kib=%ld rusage_maxrss_kib=%ld sampled_smaps_rss_kib=%ld sampled_anonymous_kib=%ld rc=%d\n", argv[2], peak, usage.ru_maxrss, sampled, anon_peak, rc);
    return rc;
  }
  if (argc != 5 || strcmp(argv[1], "watch")) return 64;
  pid_t pid = (pid_t)strtol(argv[2], NULL, 10);
  long duration = strtol(argv[3], NULL, 10);
  if (pid <= 0 || duration < 100 || duration > 120000) return 64;
  char path[80]; snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
  long maxrss = 0, maxhwm = 0, minavail = 1L << 30;
  long smaps_rss = 0, smaps_pss = 0, smaps_anon = 0;
  for (long ms = 0; ms <= duration; ms += 20) {
    FILE *f = fopen(path, "r"); char text[8192]; struct qdc_mem m;
    if (!f) { printf("TARGET_GONE label=%s pid=%ld\n", argv[4], (long)pid); return 1; }
    size_t n = fread(text, 1, sizeof(text)-1, f); fclose(f); text[n] = 0;
    if (qdc_parse_mem(text, &m)) { printf("STATUS_INVALID label=%s\n", argv[4]); return 1; }
    long avail = available();
    if (m.rss > maxrss) maxrss = m.rss;
    if (m.hwm > maxhwm) maxhwm = m.hwm;
    if (avail >= 0 && avail < minavail) minavail = avail;
    if (!(ms % 100)) {
      struct qdc_smaps sm;
      if (smaps(pid, &sm)) { printf("SMAPS_READ_FAILED label=%s\n", argv[4]); return 1; }
      if (sm.rss > smaps_rss) smaps_rss = sm.rss;
      if (sm.pss > smaps_pss) smaps_pss = sm.pss;
      if (sm.anonymous > smaps_anon) smaps_anon = sm.anonymous;
      if (ms == duration)
        printf("SMAPS label=%s rss_kib=%ld pss_kib=%ld anonymous_kib=%ld private_dirty_kib=%ld private_clean_kib=%ld\n", argv[4], sm.rss, sm.pss, sm.anonymous, sm.dirty, sm.clean);
    }
    if (!(ms % 1000) || ms == duration)
      printf("MEMORY label=%s nominal_ms=%ld rss_kib=%ld hwm_kib=%ld vsize_kib=%ld threads=%ld available_kib=%ld\n", argv[4], ms, m.rss, m.hwm, m.vsize, m.threads, avail);
    struct timespec delay = {0, 20000000}; nanosleep(&delay, NULL);
  }
  printf("MEMORY_SUMMARY label=%s sampled_max_rss_kib=%ld lifetime_hwm_kib=%ld sampled_smaps_rss_kib=%ld sampled_pss_kib=%ld sampled_anonymous_kib=%ld min_available_kib=%ld\n", argv[4], maxrss, maxhwm, smaps_rss, smaps_pss, smaps_anon, minavail);
  return 0;
}
#endif
