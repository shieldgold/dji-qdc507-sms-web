#define _GNU_SOURCE
#include "log_watch.h"
#include "packet.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void interrupted(int sig) {
  (void)sig;
  stop = 1;
}
static int identifier(const char *s) {
  return s && *s && strlen(s) < 80 &&
         strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456"
                   "789_-") == strlen(s);
}
static int limits(void) {
  struct rlimit core = {0, 0}, fds = {32, 32}, stack = {256 * 1024, 256 * 1024},
                as = {8 * 1024 * 1024, 8 * 1024 * 1024};
  return setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_NOFILE, &fds) ||
         setrlimit(RLIMIT_STACK, &stack) || setrlimit(RLIMIT_AS, &as);
}
static int open_tun(const char *path) {
  struct stat s;
  if (lstat(path, &s) || !S_ISCHR(s.st_mode) || major(s.st_rdev) != 10 ||
      minor(s.st_rdev) != 200 || (s.st_mode & 0777) != 0600 || s.st_uid != 0) {
    fprintf(stderr, "test node identity/permissions rejected\n");
    return -1;
  }
  return open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
}
static int private_namespace(void) {
  char a[80], b[80];
  ssize_t n = readlink("/proc/self/ns/net", a, sizeof(a) - 1);
  if (n < 0)
    return -1;
  a[n] = 0;
  if (unshare(CLONE_NEWNET))
    return -1;
  n = readlink("/proc/self/ns/net", b, sizeof(b) - 1);
  if (n < 0)
    return -1;
  b[n] = 0;
  if (!strcmp(a, b)) {
    errno = EINVAL;
    return -1;
  }
  printf("NAMESPACE pid=%ld before=%s after=%s\n", (long)getpid(), a, b);
  return 0;
}
static int create_tun(int fd, int ctl) {
  if (if_nametoindex("qdcvt0")) {
    errno = EEXIST;
    return -1;
  }
  struct ifreq req;
  memset(&req, 0, sizeof(req));
  strcpy(req.ifr_name, "qdcvt0");
  req.ifr_flags = (short)(IFF_TUN | IFF_NO_PI | IFF_TUN_EXCL);
  if (ioctl(fd, TUNSETIFF, &req))
    return -1;
  unsigned flags = 0;
  struct ifreq actual;
  memset(&actual, 0, sizeof(actual));
  if (ioctl(fd, TUNGETIFF, &actual))
    return -1;
  flags = (unsigned short)actual.ifr_flags;
  if (!(flags & IFF_TUN) || !(flags & IFF_NO_PI) || (flags & IFF_PERSIST)) {
    errno = EINVAL;
    return -1;
  }
  memset(&req, 0, sizeof(req));
  strcpy(req.ifr_name, "qdcvt0");
  if (ioctl(ctl, SIOCGIFMTU, &req))
    return -1;
  printf("TUN_CREATED name=qdcvt0 ifindex=%u mtu=%d flags=%x\n",
         if_nametoindex("qdcvt0"), req.ifr_mtu, flags);
  return req.ifr_mtu == 1500 ? 0 : -1;
}
static int address(int ctl, int cmd, const char *value) {
  struct ifreq q;
  memset(&q, 0, sizeof(q));
  strcpy(q.ifr_name, "qdcvt0");
  struct sockaddr_in *a = (void *)&q.ifr_addr;
  a->sin_family = AF_INET;
  if (inet_pton(AF_INET, value, &a->sin_addr) != 1) {
    errno = EINVAL;
    return -1;
  }
  return ioctl(ctl, cmd, &q);
}
static int configure(int fd, int ctl) {
  if (ioctl(fd, TUNSETOFFLOAD, 0) || address(ctl, SIOCSIFADDR, "192.0.2.1") ||
      address(ctl, SIOCSIFDSTADDR, "192.0.2.2") ||
      address(ctl, SIOCSIFNETMASK, "255.255.255.252"))
    return -1;
  struct ifreq q;
  memset(&q, 0, sizeof(q));
  strcpy(q.ifr_name, "qdcvt0");
  q.ifr_mtu = 1500;
  if (ioctl(ctl, SIOCSIFMTU, &q) || ioctl(ctl, SIOCGIFFLAGS, &q))
    return -1;
  q.ifr_flags |= IFF_UP;
  return ioctl(ctl, SIOCSIFFLAGS, &q);
}
static void dump_file(const char *path, const char *label) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char b[256];
  printf("SNAPSHOT %s\n", label);
  while (fgets(b, sizeof(b), f))
    fputs(b, stdout);
  fclose(f);
}
static int ready(int fd) {
  struct pollfd p = {fd, POLLIN, 0};
  int n;
  do
    n = poll(&p, 1, 2000);
  while (n < 0 && errno == EINTR && !stop);
  return n == 1 && (p.revents & POLLIN) ? 0 : -1;
}
static int traffic(int tun, const char *run) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(39000)},
                     peer = {.sin_family = AF_INET, .sin_port = htons(39001)};
  inet_pton(AF_INET, "192.0.2.1", &local.sin_addr);
  inet_pton(AF_INET, "192.0.2.2", &peer.sin_addr);
  int result = -1;
  unsigned other = 0;
  size_t observed = 0;
  if (bind(fd, (void *)&local, sizeof(local)))
    goto done;
  for (int seq = 0; seq < 10 && !stop; seq++) {
    unsigned char out[2048], back[2048], tx[256], rx[256];
    int n = snprintf((char *)tx, sizeof(tx), "%s:kernel-to-fd:%d", run, seq),
        m = snprintf((char *)rx, sizeof(rx), "%s:fd-to-kernel:%d", run, seq);
    if (n < 0 || m < 0 || (size_t)n >= sizeof(tx) || (size_t)m >= sizeof(rx))
      goto done;
    if (sendto(fd, tx, (size_t)n, 0, (void *)&peer, sizeof(peer)) != n)
      goto done;
    for (;;) {
      if (stop || ready(tun))
        goto done;
      ssize_t got = read(tun, out, sizeof(out));
      if (got <= 0)
        goto done;
      observed += (size_t)got;
      if (observed > 65536)
        goto done;
      if (qdc_match(out, (size_t)got, local.sin_addr.s_addr,
                    peer.sin_addr.s_addr, 39000, 39001, tx, (size_t)n))
        break;
      if (++other > 32)
        goto done;
    }
    size_t length =
        qdc_packet(back, sizeof(back), peer.sin_addr.s_addr,
                   local.sin_addr.s_addr, 39001, 39000, rx, (size_t)m);
    if (!length || write(tun, back, length) != (ssize_t)length || ready(fd))
      goto done;
    struct sockaddr_in from;
    socklen_t size = sizeof(from);
    ssize_t got = recvfrom(fd, out, sizeof(out), 0, (void *)&from, &size);
    if (got != m || memcmp(out, rx, (size_t)m) ||
        from.sin_addr.s_addr != peer.sin_addr.s_addr ||
        from.sin_port != peer.sin_port)
      goto done;
    printf("PACKET_PAIR_OK seq=%d out_bytes=%d in_bytes=%d\n", seq, n, m);
    struct timespec delay = {0, 100000000};
    nanosleep(&delay, NULL);
  }
  if (!stop) {
    printf("TRAFFIC_OK pairs=10 observed_bytes=%zu unrelated=%u\n", observed,
           other);
    result = 0;
  }
done:
  if (result)
    fprintf(stderr, "TRAFFIC_FAILED errno=%d stop=%d\n", errno, stop);
  close(fd);
  return result;
}
static int worker(const char *mode, const char *node, const char *run) {
  int tun = -1, ctl = -1, rc = 1;
  if (strcmp(mode, "features") && private_namespace()) {
    perror("unshare");
    return 1;
  }
  tun = open_tun(node);
  if (tun < 0) {
    perror("open tun");
    return 1;
  }
  if (!strcmp(mode, "features")) {
    unsigned features = 0;
    if (ioctl(tun, TUNGETFEATURES, &features)) {
      perror("TUNGETFEATURES");
      goto done;
    }
    printf("FEATURES_OK features=%x pid=%ld\n", features, (long)getpid());
    rc = 0;
    goto done;
  }
  ctl = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (ctl < 0 || create_tun(tun, ctl)) {
    perror("TUNSETIFF");
    goto done;
  }
  dump_file("/proc/self/net/dev", "private-dev-before");
  if (!strcmp(mode, "traffic")) {
    if (configure(tun, ctl)) {
      perror("configure private TUN");
      goto done;
    }
    dump_file("/proc/self/net/route", "private-routes");
    if (traffic(tun, run))
      goto done;
    dump_file("/proc/self/net/dev", "private-dev-after");
  } else {
    struct timespec delay = {2, 0};
    nanosleep(&delay, NULL);
  }
  rc = stop ? 1 : 0;
done:
  if (tun >= 0) {
    if (close(tun)) {
      perror("close tun");
      rc = 1;
    }
    tun = -1;
  }
  if (strcmp(mode, "features")) {
    if (if_nametoindex("qdcvt0")) {
      fputs("INTERFACE_LEAK\n", stderr);
      rc = 1;
    } else
      puts("INTERFACE_REMOVED qdcvt0");
    dump_file("/proc/self/net/dev", "private-dev-cleanup");
  }
  if (ctl >= 0)
    close(ctl);
  printf("WORKER_END mode=%s rc=%d\n", mode, rc);
  return rc;
}
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc == 2 && !strcmp(argv[1], "selftest")) {
    unsigned char p[128], x[] = {1, 2, 3};
    const unsigned char data[] = "runtime-probe";
    uint32_t a = 0x010200c0, b = 0x020200c0;
    if (qdc_checksum(x, 3) != 0xfbfd)
      return 1;
    size_t n = qdc_packet(p, sizeof(p), a, b, 1, 2, data, sizeof(data));
    if (!qdc_match(p, n, a, b, 1, 2, data, sizeof(data)))
      return 1;
    p[n - 1] ^= 1;
    if (qdc_match(p, n, a, b, 1, 2, data, sizeof(data)))
      return 1;
    uint64_t seq = 0;
    int seen = 0;
    if (qdc_log_problem("6,1,0,-;test", &seq, &seen))
      return 1;
    puts("SELFTEST_OK");
    return 0;
  }
  if (argc >= 6 && !strcmp(argv[1], "watch")) {
    if (!identifier(argv[2]) || limits())
      return 64;
    int observe = atoi(argv[3]), limit = atoi(argv[4]);
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      perror("open /dev/kmsg");
      return 65;
    }
    if (lseek(fd, 0, SEEK_END) < 0) {
      perror("seek /dev/kmsg");
      close(fd);
      return 65;
    }
    int rc = qdc_watch(fd, argv[2], observe, limit, argv + 5);
    close(fd);
    return rc;
  }
  if (argc < 3 || argc > 4 ||
      (!strcmp(argv[1], "features")
           ? argc != 3
           : (strcmp(argv[1], "create") && strcmp(argv[1], "traffic"))) ||
      strncmp(argv[2], "/dev/qdc-tun-", 13) || strchr(argv[2] + 5, '/') ||
      (strcmp(argv[1], "features") && (argc != 4 || !identifier(argv[3]))))
    return 64;
  if (limits()) {
    return 64;
  }
  signal(SIGTERM, interrupted);
  signal(SIGINT, interrupted);
  signal(SIGALRM, interrupted);
  alarm(30);
  return worker(argv[1], argv[2], argc == 4 ? argv[3] : "");
}
