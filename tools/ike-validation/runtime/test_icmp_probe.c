#define _GNU_SOURCE
#include "icmp_probe.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Build normal parser tests with icmp_probe.c, or compile this file alone with
 * -DQDC_ICMP_TEST_IO to exercise the real loop using a deterministic fake OS.
 * Only system calls are replaced; packet matching and runtime logic are real.
 */
#ifdef QDC_ICMP_TEST_IO
static int fake_socket(int, int, int);
static int fake_fcntl(int, int, ...);
static int fake_bind(int, const struct sockaddr *, socklen_t);
static int fake_poll(struct pollfd *, nfds_t, int);
static ssize_t fake_sendto(int, const void *, size_t, int,
                           const struct sockaddr *, socklen_t);
static ssize_t fake_recvmsg(int, struct msghdr *, int);
static int fake_close(int);
static int fake_clock_gettime(clockid_t, struct timespec *);
static pid_t fake_getpid(void);
#define socket fake_socket
#define fcntl fake_fcntl
#define bind fake_bind
#define poll fake_poll
#define sendto fake_sendto
#define recvmsg fake_recvmsg
#define close fake_close
#define clock_gettime fake_clock_gettime
#define getpid fake_getpid
#include "icmp_probe.c"
#undef socket
#undef fcntl
#undef bind
#undef poll
#undef sendto
#undef recvmsg
#undef close
#undef clock_gettime
#undef getpid
#endif

#define PACKET_SIZE (60u + 8u + QDC_ICMP_PAYLOAD_SIZE)
static unsigned char packet[PACKET_SIZE];
static unsigned char payload[QDC_ICMP_PAYLOAD_SIZE];
static uint32_t source, target;

static uint16_t test_checksum(const unsigned char *p, size_t n) {
    unsigned long sum = 0;
    while (n > 1) {
        sum += ((unsigned)p[0] << 8) | p[1];
        p += 2; n -= 2;
    }
    if (n) sum += (unsigned)p[0] << 8;
    while (sum >> 16) sum = (sum & 65535u) + (sum >> 16);
    return (uint16_t)~sum;
}

static void test_put16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static size_t reply(size_t header) {
    size_t length = header + 8 + QDC_ICMP_PAYLOAD_SIZE;
    assert(header >= 20 && header <= 60 && !(header % 4));
    memset(packet, 0, sizeof(packet));
    packet[0] = (unsigned char)(0x40 | header / 4);
    test_put16(packet + 2, (uint16_t)length);
    packet[8] = 64; packet[9] = IPPROTO_ICMP;
    memcpy(packet + 12, &target, 4);
    memcpy(packet + 16, &source, 4);
    test_put16(packet + header + 4, 0x1234);
    test_put16(packet + header + 6, 0x5678);
    memcpy(packet + header + 8, payload, sizeof(payload));
    test_put16(packet + header + 2, test_checksum(packet + header, length - header));
    test_put16(packet + 10, test_checksum(packet, header));
    return length;
}

static int matches(size_t length) {
    return qdc_icmp_reply_matches(packet, length, source, target,
                                   0x1234, 0x5678, payload);
}

static void repair_ip(size_t header) {
    test_put16(packet + 10, 0);
    test_put16(packet + 10, test_checksum(packet, header));
}

static void repair_icmp(size_t header, size_t length) {
    test_put16(packet + header + 2, 0);
    test_put16(packet + header + 2, test_checksum(packet + header, length - header));
}

static void test_packets(void) {
    struct in_addr address;
    assert(inet_pton(AF_INET, "192.0.2.1", &address) == 1);
    source = address.s_addr;
    assert(inet_pton(AF_INET, "192.0.2.2", &address) == 1);
    target = address.s_addr;
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (unsigned char)i;
    size_t length = reply(20);
    assert(matches(length) == 1);
    for (size_t n = 0; n < length; n++) assert(!matches(n));
    assert(!matches(length + 1));
    assert(!matches((size_t)-1));
    assert(!qdc_icmp_reply_matches(NULL, length, source, target,
                                    0x1234, 0x5678, payload));
    assert(!qdc_icmp_reply_matches(packet, length, source, target,
                                    0x1234, 0x5678, NULL));
    packet[0] = 0x65; repair_ip(20); assert(!matches(length));
    reply(20); packet[0] = 0x44; repair_ip(20); assert(!matches(length));
    reply(20); packet[0] = 0x4f; assert(!matches(length));
    for (uint16_t n = 0; n <= 2; n++) {
        reply(20); test_put16(packet + 2, n == 0 ? 0 : (uint16_t)(length + n - 1));
        if (n == 1) test_put16(packet + 2, (uint16_t)(length - 1));
        repair_ip(20); assert(!matches(length));
    }
    reply(20); packet[9] = IPPROTO_UDP; repair_ip(20); assert(!matches(length));
    const uint16_t fragments[] = {0x8000, 0x2000, 1, 0x1fff, 0x6001};
    for (size_t i = 0; i < sizeof(fragments)/sizeof(fragments[0]); i++) {
        reply(20); test_put16(packet + 6, fragments[i]); repair_ip(20);
        assert(!matches(length));
    }
    reply(20); test_put16(packet + 6, 0x4000); repair_ip(20); assert(matches(length));
    reply(20); packet[8] ^= 1; assert(!matches(length));
    reply(20); packet[12] ^= 1; repair_ip(20); assert(!matches(length));
    reply(20); packet[16] ^= 1; repair_ip(20); assert(!matches(length));
    reply(20); packet[20] = 8; repair_icmp(20, length); assert(!matches(length));
    reply(20); packet[21] = 1; repair_icmp(20, length); assert(!matches(length));
    reply(20); packet[24] ^= 1; repair_icmp(20, length); assert(!matches(length));
    reply(20); packet[26] ^= 1; repair_icmp(20, length); assert(!matches(length));
    reply(20); packet[22] ^= 1; assert(!matches(length));
    for (size_t i = 0; i < sizeof(payload); i++) {
        reply(20); packet[28 + i] ^= 1; repair_icmp(20, length);
        assert(!matches(length));
    }
    for (size_t header = 20; header <= 60; header += 4) {
        length = reply(header); assert(matches(length));
        for (size_t n = 0; n < length; n++) assert(!matches(n));
        unsigned char unaligned[PACKET_SIZE + 1];
        memcpy(unaligned + 1, packet, length);
        assert(qdc_icmp_reply_matches(unaligned + 1, length, source, target,
                                       0x1234, 0x5678, payload));
    }
    length = reply(20);
    test_put16(packet + 24, 0); test_put16(packet + 26, 65535);
    repair_icmp(20, length);
    assert(qdc_icmp_reply_matches(packet, length, source, target, 0, 65535, payload));
    test_put16(packet + 24, 65535); test_put16(packet + 26, 0);
    repair_icmp(20, length);
    assert(qdc_icmp_reply_matches(packet, length, source, target, 65535, 0, payload));
}

static int count_pump(void *opaque) {
    ++*(int *)opaque;
    return 0;
}

static void test_addresses(void) {
    const char *bad[] = {NULL, "", "localhost", "127.1", "0x7f000001",
                         "127.0.0.01", "127.0.0.256", "::1", "127.0.0.1 ",
                         " 127.0.0.1", "127.0.0.1/32"};
    int calls = 0;
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        assert(qdc_icmp_once(bad[i], "127.0.0.1", count_pump, &calls) == -1);
        assert(qdc_icmp_once("127.0.0.1", bad[i], count_pump, &calls) == -1);
    }
    assert(qdc_icmp_once("127.0.0.1", "127.0.0.1", NULL, NULL) == -1);
    assert(calls == 0);
}

#ifdef QDC_ICMP_TEST_IO
#include <stdarg.h>

enum mode {SUCCESS, TIMEOUT, CANCEL, UNRELATED, FLOOD, INTERRUPT,
           POLL_OVERRUN, PUMP_OVERRUN, RECV_OVERRUN, MATCH_PUMP_OVERRUN, CANCEL_AFTER_POLL,
           CLOCK_FAILURE, SOCKET_FAILURE, FCNTL_FAILURE, BIND_FAILURE,
           POLL_FAILURE, POLL_ERROR, SEND_FAILURE, SEND_SHORT, RECV_FAILURE};
static struct {
    enum mode mode;
    long now_ms;
    int sockets, closes, polls, pumps, last_pump;
    int nonblock, cloexec, bound, sends, send_attempts, receives;
    unsigned char request[8 + QDC_ICMP_PAYLOAD_SIZE];
} sim;

static void reset(enum mode mode) {
    memset(&sim, 0, sizeof(sim)); sim.mode = mode;
}

static int fake_socket(int family, int type, int protocol) {
    assert(family == AF_INET && type == SOCK_RAW && protocol == IPPROTO_ICMP);
    sim.sockets++;
    if (sim.mode == SOCKET_FAILURE) { errno = EPERM; return -1; }
    return 17;
}

static int fake_fcntl(int fd, int command, ...) {
    assert(fd == 17);
    if (sim.mode == FCNTL_FAILURE) { errno = EIO; return -1; }
    if (command == F_GETFL || command == F_GETFD) return 0;
    va_list args; va_start(args, command);
    int flags = va_arg(args, int); va_end(args);
    if (command == F_SETFL) { assert(flags & O_NONBLOCK); sim.nonblock = 1; }
    else { assert(command == F_SETFD && (flags & FD_CLOEXEC)); sim.cloexec = 1; }
    return 0;
}

static int fake_bind(int fd, const struct sockaddr *address, socklen_t n) {
    const struct sockaddr_in *ip = (const struct sockaddr_in *)address;
    assert(fd == 17 && n == sizeof(*ip));
    assert(ip->sin_family == AF_INET && ip->sin_addr.s_addr == source);
    assert(ip->sin_port == 0 && sim.nonblock && sim.cloexec);
    sim.bound = 1;
    if (sim.mode == BIND_FAILURE) { errno = EADDRNOTAVAIL; return -1; }
    return 0;
}

static int fake_poll(struct pollfd *p, nfds_t n, int timeout) {
    assert(n == 1 && p->fd == 17 && sim.bound);
    assert(timeout > 0 && timeout <= 100);
    assert(sim.pumps > sim.last_pump); sim.last_pump = sim.pumps;
    sim.polls++;
    if (sim.mode == INTERRUPT && sim.polls == 1) { errno = EINTR; return -1; }
    if (sim.mode == POLL_FAILURE) { errno = EIO; return -1; }
    if (sim.mode == POLL_ERROR) { p->revents = POLLERR; return 1; }
    if (sim.sends && (sim.mode == TIMEOUT || sim.mode == CANCEL)) {
        assert(p->events & POLLIN);
        sim.now_ms += timeout; p->revents = 0; return 0;
    }
    if (sim.mode == POLL_OVERRUN && sim.sends) sim.now_ms = 1001;
    else sim.now_ms++;
    assert(p->events & (sim.sends ? POLLIN : POLLOUT));
    p->revents = sim.sends ? POLLIN : POLLOUT;
    return 1;
}

static ssize_t fake_sendto(int fd, const void *data, size_t n, int flags,
                           const struct sockaddr *address, socklen_t length) {
    const struct sockaddr_in *ip = (const struct sockaddr_in *)address;
    assert(fd == 17 && !flags && n == sizeof(sim.request));
    assert(length == sizeof(*ip) && ip->sin_family == AF_INET);
    assert(ip->sin_addr.s_addr == target && ip->sin_port == 0);
    sim.send_attempts++;
    if (sim.mode == INTERRUPT && sim.send_attempts == 1) {
        errno = EAGAIN; return -1;
    }
    if (sim.mode == SEND_FAILURE) { errno = EIO; return -1; }
    if (sim.mode == SEND_SHORT) return (ssize_t)n - 1;
    const unsigned char *icmp = data;
    assert(icmp[0] == 8 && icmp[1] == 0 && !test_checksum(icmp, n));
    memcpy(sim.request, data, n); sim.sends++;
    assert(sim.sends == 1);
    return (ssize_t)n;
}

static ssize_t fake_recvmsg(int fd, struct msghdr *msg, int flags) {
    assert(fd == 17 && !flags && sim.sends == 1);
    assert(msg->msg_iovlen == 1);
    sim.receives++;
    if (sim.mode == RECV_FAILURE) { errno = EIO; return -1; }
    if (sim.mode == INTERRUPT && sim.receives == 1) { errno = EINTR; return -1; }
    size_t length = reply(20);
    memcpy(packet + 20, sim.request, sizeof(sim.request));
    packet[20] = 0; repair_icmp(20, length);
    if (sim.mode == FLOOD || (sim.mode == UNRELATED && sim.receives == 1)) {
        packet[12] ^= 1; repair_ip(20);
    } else if (sim.mode == UNRELATED && sim.receives == 2) {
        msg->msg_flags |= MSG_TRUNC;
    } else if (sim.mode == UNRELATED && sim.receives == 3) {
        packet[22] ^= 1;
    } else if (sim.mode == UNRELATED && sim.receives == 4) {
        packet[28] ^= 1; repair_icmp(20, length);
    } else if (sim.mode == UNRELATED && sim.receives == 5) {
        packet[0] = 0x44;
    }
    assert(msg->msg_iov[0].iov_len >= length);
    memcpy(msg->msg_iov[0].iov_base, packet, length);
    if (sim.mode == RECV_OVERRUN) sim.now_ms = 1001;
    return (ssize_t)length;
}

static int fake_close(int fd) { assert(fd == 17); sim.closes++; return 0; }
static pid_t fake_getpid(void) { return 12345; }
static int fake_clock_gettime(clockid_t clock_id, struct timespec *value) {
    assert(clock_id == CLOCK_MONOTONIC);
    if (sim.mode == CLOCK_FAILURE) { errno = EIO; return -1; }
    value->tv_sec = sim.now_ms / 1000;
    value->tv_nsec = (sim.now_ms % 1000) * 1000000;
    return 0;
}
static int monitored(void *opaque) {
    assert(opaque == &sim); sim.pumps++;
    if ((sim.mode == PUMP_OVERRUN && sim.polls == 2 && !sim.receives) ||
        (sim.mode == MATCH_PUMP_OVERRUN && sim.receives)) sim.now_ms = 1001;
    if (sim.mode == CANCEL_AFTER_POLL && sim.polls == 1 && !sim.sends) return 1;
    return sim.mode == CANCEL && sim.pumps >= 3 ? -1 : 0;
}
static int probe(void) {
    return qdc_icmp_once("192.0.2.1", "192.0.2.2", monitored, &sim);
}
static void test_loop(void) {
    reset(SUCCESS); assert(probe() == 0);
    assert(sim.sends == 1 && sim.closes == 1 && sim.receives == 1 && sim.pumps >= 2);
    const enum mode late[] = {POLL_OVERRUN, PUMP_OVERRUN, RECV_OVERRUN, MATCH_PUMP_OVERRUN};
    for (size_t i = 0; i < sizeof(late)/sizeof(late[0]); i++) {
        reset(late[i]); assert(probe() == -1);
        assert(sim.closes == 1);
    }
    reset(CANCEL_AFTER_POLL); assert(probe() == -1);
    assert(sim.polls == 1 && !sim.sends && !sim.receives && sim.closes == 1);
    reset(UNRELATED); assert(probe() == 0);
    assert(sim.receives == 6 && sim.sends == 1 && sim.closes == 1);
    reset(TIMEOUT); assert(probe() == -1);
    assert(sim.now_ms == 1000 && sim.polls >= 10 && sim.pumps >= 10 && sim.closes == 1);
    reset(CANCEL); assert(probe() == -1);
    assert(sim.pumps == 3 && sim.polls == 1 && sim.closes == 1);
    reset(FLOOD); assert(probe() == -1);
    assert(sim.now_ms == 1000 && sim.receives < 1000 && sim.closes == 1);
    reset(INTERRUPT); assert(probe() == 0);
    assert(sim.sends == 1 && sim.send_attempts == 2 && sim.receives == 2 && sim.closes == 1);
    const enum mode failure[] = {CLOCK_FAILURE, SOCKET_FAILURE, FCNTL_FAILURE,
                                 BIND_FAILURE, POLL_FAILURE, POLL_ERROR,
                                 SEND_FAILURE, SEND_SHORT, RECV_FAILURE};
    for (size_t i = 0; i < sizeof(failure)/sizeof(failure[0]); i++) {
        reset(failure[i]); assert(probe() == -1);
        assert(sim.closes == (sim.sockets && sim.mode != SOCKET_FAILURE ? 1 : 0));
    }
    reset(SUCCESS); test_addresses(); assert(!sim.sockets);
}
#endif

int main(int argc, char **argv) {
#ifndef QDC_ICMP_TEST_IO
    if (argc == 2 && !strcmp(argv[1], "--loopback")) {
        int calls = 0;
        int result = qdc_icmp_once("127.0.0.1", "127.0.0.1", count_pump, &calls);
        if (result) { perror("ICMP_LOOPBACK"); return 1; }
        puts("ICMP_LOOPBACK_OK"); return 0;
    }
#else
    (void)argc; (void)argv;
#endif
    test_packets();
    test_addresses();
#ifdef QDC_ICMP_TEST_IO
    test_loop();
#endif
    puts("ICMP_PROBE_TESTS_OK");
    return 0;
}
