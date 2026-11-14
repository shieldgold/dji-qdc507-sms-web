/* Bounded read-only SMS page probe. This file has no credential or API access. */
#define _GNU_SOURCE
#include "http_probe.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HEADER_LIMIT 8192
#define BODY_LIMIT 1048576
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int qdc_http_local_address(const char *address) {
    struct in_addr ip;
    if (inet_pton(AF_INET, address, &ip) != 1) return 0;
    unsigned long n = ntohl(ip.s_addr);
    if ((n >> 24) != 10 && (n >> 24) != 127 &&
        (n >> 20) != 0xac1 && (n >> 16) != 0xc0a8) return 0;
    struct ifaddrs *list;
    if (getifaddrs(&list)) return 0;
    int found = 0;
    for (struct ifaddrs *p = list; p; p = p->ifa_next) {
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && (p->ifa_flags & IFF_UP) &&
            ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr == ip.s_addr) found = 1;
    }
    freeifaddrs(list);
    return found;
}

static double now(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) return -1;
    return value.tv_sec + value.tv_nsec/1e9;
}

static int ready(int fd, short events, double deadline) {
    for (;;) {
        double left = deadline - now();
        if (left <= 0 || left > 2.1) return -1;
        int ms = left > 0.1 ? 100 : (int)(left*1000) + 1;
        struct pollfd p = {fd, events, 0};
        int rc = poll(&p, 1, ms);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 || (p.revents & POLLNVAL)) return -1;
        if (rc > 0 && (p.revents & (events | POLLERR | POLLHUP))) return 0;
    }
}

static int header_name(const char *p, size_t n) {
    if (!n) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c))) return 0;
    }
    return 1;
}

/* header is NUL terminated at the end of the last CRLF, before the empty line. */
static int headers(char *header, int *status, long *length) {
    char *end = strstr(header, "\r\n");
    if (!end) return -1;
    *end = 0;
    size_t n = strlen(header);
    if (n < 13 || (strncmp(header, "HTTP/1.0 ", 9) && strncmp(header, "HTTP/1.1 ", 9)) ||
        header[9] < '1' || header[9] > '5' || header[10] < '0' || header[10] > '9' ||
        header[11] < '0' || header[11] > '9' || header[12] != ' ') return -1;
    for (size_t i = 13; i < n; i++) if ((unsigned char)header[i] < 32 || (unsigned char)header[i] > 126) return -1;
    *status = (header[9]-'0')*100 + (header[10]-'0')*10 + header[11]-'0';
    char *p = end + 2;
    while (*p) {
        end = strstr(p, "\r\n");
        if (!end) return -1;
        *end = 0;
        char *colon = strchr(p, ':');
        if (!colon || !header_name(p, (size_t)(colon-p))) return -1;
        *colon = 0;
        char *value = colon+1;
        for (char *q = value; *q; q++) {
            unsigned char c = (unsigned char)*q;
            if ((c < 32 && c != '\t') || c == 127) return -1;
        }
        while (*value == ' ' || *value == '\t') value++;
        char *tail = value+strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (!strcasecmp(p, "Transfer-Encoding")) return -1; /* No chunked decoder. */
        if (!strcasecmp(p, "Content-Length")) {
            if (*length >= 0 || !*value) return -1;
            long result = 0;
            for (char *q = value; *q; q++) {
                if (*q < '0' || *q > '9' || result > (BODY_LIMIT-(*q-'0'))/10) return -1;
                result = result*10 + *q-'0';
            }
            *length = result;
        }
        p = end+2;
    }
    return 0;
}

int qdc_http_once(const char *address) {
    int fd = -1, status = 0, transport = 0, success = 0;
    size_t body = 0;
    if (!qdc_http_local_address(address)) goto done;
    double deadline = now();
    if (deadline < 0) goto done;
    deadline += 2;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK)) goto done;
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe))) goto done;
#endif
    struct sockaddr_in endpoint = {.sin_family = AF_INET, .sin_port = htons(8787)};
    if (inet_pton(AF_INET, address, &endpoint.sin_addr) != 1) goto done;
    int rc = connect(fd, (struct sockaddr *)&endpoint, sizeof(endpoint));
    if (rc && errno != EINPROGRESS) goto done;
    if (rc) {
        if (ready(fd, POLLOUT, deadline)) goto done;
        int error = 0; socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) goto done;
    }
    transport = 1;
    char request[160];
    int request_len = snprintf(request, sizeof(request),
        "GET / HTTP/1.0\r\nHost: %s:8787\r\nConnection: close\r\n\r\n", address);
    if (request_len < 1 || request_len >= (int)sizeof(request)) goto done;
    size_t sent = 0;
    while (sent < (size_t)request_len) {
        if (ready(fd, POLLOUT, deadline)) goto done;
        ssize_t n = send(fd, request+sent, (size_t)request_len-sent, MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) goto done;
        sent += (size_t)n;
    }
    char buffer[HEADER_LIMIT+1]; size_t used = 0; int parsed = 0; long length = -1;
    for (;;) {
        if (ready(fd, POLLIN, deadline)) goto done;
        ssize_t n = recv(fd, buffer + (parsed ? 0 : used), parsed ? HEADER_LIMIT : HEADER_LIMIT-used, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n < 0) goto done;
        if (!n) {
            success = parsed && status == 200 && (length < 0 || body == (size_t)length);
            break;
        }
        if (parsed) body += (size_t)n;
        else {
            used += (size_t)n; buffer[used] = 0;
            char *end = strstr(buffer, "\r\n\r\n");
            if (end) {
                size_t header_bytes = (size_t)(end-buffer)+4;
                body = used-header_bytes;
                end[2] = 0;
                if (headers(buffer, &status, &length)) goto done;
                parsed = 1;
                if (status != 200) goto done;
            } else if (memchr(buffer, 0, used) || used == HEADER_LIMIT) goto done;
        }
        if (body > BODY_LIMIT || (length >= 0 && body > (size_t)length)) goto done;
    }
done:
    if (fd >= 0) close(fd);
    printf("HTTP_PROBE status=%d transport=%d bytes=%zu\n", status, transport, body);
    return success ? 0 : 1;
}
