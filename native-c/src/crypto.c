#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif
#include "qdc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

int qdc_random(void *dest, size_t len, qdc_error *error) {
    unsigned char *p = dest;
    size_t done = 0;
    if (!dest && len)
        return -1;
    while (done < len) {
        size_t part = len - done;
#if defined(__linux__)
        if (part > 256)
            part = 256;
        ssize_t n = getrandom(p + done, part, GRND_NONBLOCK);
        if (n < 0 && errno == EAGAIN) {
            /* Linux 3.18 can leave its secondary pool asleep after the input
             * pool has received entropy. This read only prompts the kernel
             * to reseed it. Discard every byte; return ONLY verified-ready
             * getrandom output, and never block a request waiting for boot. */
            unsigned char discard[32];
            int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd >= 0) {
                ssize_t ignored;
                do {
                    ignored = read(fd, discard, sizeof(discard));
                } while (ignored < 0 && errno == EINTR);
                close(fd);
            }
            memset(discard, 0, sizeof(discard));
            n = getrandom(p + done, part, GRND_NONBLOCK);
        }
#else
        if (part > 256)
            part = 256;
        ssize_t n = getentropy(p + done, part) == 0 ? (ssize_t)part : -1;
#endif
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            memset(dest, 0, len);
            qdc_error_set(error, "系统安全随机源尚未就绪或不可用，请稍后重试");
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int qdc_random_hex(char *dest, size_t bytes, qdc_error *error) {
    static const char digits[] = "0123456789abcdef";
    unsigned char block[64];
    if (!dest || bytes > (SIZE_MAX - 1) / 2)
        return -1;
    for (size_t offset = 0; offset < bytes;) {
        size_t n = bytes - offset;
        if (n > sizeof(block))
            n = sizeof(block);
        if (qdc_random(block, n, error) != 0) {
            memset(dest, 0, bytes * 2 + 1);
            return -1;
        }
        for (size_t i = 0; i < n; i++) {
            dest[(offset + i) * 2] = digits[block[i] >> 4];
            dest[(offset + i) * 2 + 1] = digits[block[i] & 15];
        }
        offset += n;
    }
    memset(block, 0, sizeof(block));
    dest[bytes * 2] = 0;
    return 0;
}

int qdc_sha256(const void *data, size_t len, unsigned char out[32]) {
    if ((!data && len) || !out)
        return -1;
    return mbedtls_sha256(data, len, out, 0) == 0 ? 0 : -1;
}

int qdc_hmac256(const void *key, size_t key_len, const void *data, size_t len,
                unsigned char out[32]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if ((!key && key_len) || (!data && len) || !out || !md)
        return -1;
    return mbedtls_md_hmac(md, key, key_len, data, len, out) == 0 ? 0 : -1;
}

int qdc_pbkdf2(qdc_slice password, qdc_slice salt, unsigned iterations, unsigned char out[32]) {
    if ((!password.ptr && password.len) || (!salt.ptr && salt.len) || !out || iterations == 0 ||
        iterations > 10000000)
        return -1;
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)password.ptr,
                                         password.len, (const unsigned char *)salt.ptr, salt.len,
                                         iterations, 32, out) == 0
               ? 0
               : -1;
}

bool qdc_constant_equal(const void *a, const void *b, size_t len) {
    const volatile unsigned char *x = a, *y = b;
    volatile unsigned char difference = 0;
    if ((!a || !b) && len)
        return false;
    for (size_t i = 0; i < len; i++)
        difference |= x[i] ^ y[i];
    return difference == 0;
}
