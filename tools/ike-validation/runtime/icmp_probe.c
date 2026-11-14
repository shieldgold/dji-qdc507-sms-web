/* Bounded Linux raw ICMP echo; contains no child process or probe output. */
#define _GNU_SOURCE
#include "icmp_probe.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static uint16_t get16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint16_t checksum(const unsigned char *p, size_t length) {
    uint32_t sum = 0;
    while (length >= 2) {
        sum += get16(p);
        p += 2; length -= 2;
    }
    if (length) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 65535u) + (sum >> 16);
    return (uint16_t)~sum;
}

int qdc_icmp_reply_matches(const unsigned char *packet, size_t length,
                           uint32_t source, uint32_t target,
                           uint16_t identifier, uint16_t sequence,
                           const unsigned char payload[QDC_ICMP_PAYLOAD_SIZE]) {
    if (!packet || !payload || length < 20 || (packet[0] >> 4) != 4)
        return 0;
    size_t header = (size_t)(packet[0] & 15u) * 4;
    if (header < 20 || header > length ||
        length != header + 8 + QDC_ICMP_PAYLOAD_SIZE ||
        get16(packet + 2) != length || packet[9] != 1 ||
        (get16(packet + 6) & 0xbfffu) || checksum(packet, header))
        return 0;
    if (memcmp(packet + 12, &target, sizeof(target)) ||
        memcmp(packet + 16, &source, sizeof(source)))
        return 0;
    const unsigned char *icmp = packet + header;
    if (icmp[0] || icmp[1] || get16(icmp + 4) != identifier ||
        get16(icmp + 6) != sequence || checksum(icmp, length - header))
        return 0;
    return !memcmp(icmp + 8, payload, QDC_ICMP_PAYLOAD_SIZE);
}

static void put16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static int64_t monotonic_ms(struct timespec *sample) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) return -1;
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000 ||
        (uint64_t)value.tv_sec > (uint64_t)(INT64_MAX - 1000)/1000) {
        errno = EIO; return -1;
    }
    if (sample) *sample = value;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec/1000000;
}

static int64_t pump_remaining_ms(int (*pump)(void *), void *opaque,
                                  int64_t began, int64_t deadline) {
    if (pump(opaque)) { errno = ECANCELED; return -1; }
    int64_t current = monotonic_ms(NULL);
    if (current < 0) return -1;
    if (current < began || current >= deadline) { errno = ETIMEDOUT; return -1; }
    return deadline - current;
}

static int ipv4_literal(const char *text, struct in_addr *address) {
    char canonical[INET_ADDRSTRLEN];
    return text && inet_pton(AF_INET, text, address) == 1 &&
           inet_ntop(AF_INET, address, canonical, sizeof(canonical)) &&
           !strcmp(text, canonical);
}

int qdc_icmp_once(const char *source, const char *target,
                  int (*pump)(void *), void *opaque) {
    struct sockaddr_in local = {.sin_family = AF_INET};
    struct sockaddr_in remote = {.sin_family = AF_INET};
    if (!pump || !ipv4_literal(source, &local.sin_addr) ||
        !ipv4_literal(target, &remote.sin_addr)) {
        errno = EINVAL; return -1;
    }
    struct timespec start;
    int64_t began = monotonic_ms(&start);
    if (began < 0) return -1;
    int64_t deadline = began + 1000;
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) return -1;
    int result = -1;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) ||
        bind(fd, (const struct sockaddr *)&local, sizeof(local)))
        goto done;

    unsigned char request[8 + QDC_ICMP_PAYLOAD_SIZE] = {8, 0};
    uint16_t identifier = (uint16_t)getpid();
    uint16_t sequence = (uint16_t)((uint64_t)start.tv_nsec ^ (uint64_t)start.tv_sec);
    uint64_t nonce = ((uint64_t)start.tv_sec << 32) ^ (uint64_t)start.tv_nsec ^
                     ((uint64_t)identifier << 16);
    put16(request + 4, identifier);
    put16(request + 6, sequence);
    for (size_t i = 0; i < QDC_ICMP_PAYLOAD_SIZE; i++)
        request[8 + i] = (unsigned char)((nonce >> ((i % 8) * 8)) ^ i);
    put16(request + 2, checksum(request, sizeof(request)));
    int sent = 0;
    for (;;) {
        int64_t left = pump_remaining_ms(pump, opaque, began, deadline);
        if (left < 0) break;
        int milliseconds = left > 100 ? 100 : (int)left;
        short event = sent ? POLLIN : POLLOUT;
        struct pollfd ready = {fd, event, 0};
        int rc = poll(&ready, 1, milliseconds);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) break;
        if (pump_remaining_ms(pump, opaque, began, deadline) < 0) break;
        if (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EIO; break; }
        if (!rc || !(ready.revents & event)) continue;
        if (!sent) {
            ssize_t n = sendto(fd, request, sizeof(request), 0,
                               (const struct sockaddr *)&remote, sizeof(remote));
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (n != (ssize_t)sizeof(request)) { if (n >= 0) errno = EIO; break; }
            sent = 1;
        } else {
            unsigned char packet[60 + 8 + QDC_ICMP_PAYLOAD_SIZE];
            struct iovec vector = {packet, sizeof(packet)};
            struct msghdr message = {0};
            message.msg_iov = &vector; message.msg_iovlen = 1;
            ssize_t n = recvmsg(fd, &message, 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (n < 0) break;
            if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
                (size_t)n > sizeof(packet)) continue;
            if (qdc_icmp_reply_matches(packet, (size_t)n,
                                       local.sin_addr.s_addr, remote.sin_addr.s_addr,
                                       identifier, sequence, request + 8)) {
                if (pump_remaining_ms(pump, opaque, began, deadline) < 0) break;
                result = 0; break;
            }
        }
    }
done:;
    int saved_error = errno;
    close(fd);
    errno = saved_error;
    return result;
}
