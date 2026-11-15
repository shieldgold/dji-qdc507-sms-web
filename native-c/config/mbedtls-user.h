#ifndef QDC_MBEDTLS_USER_H
#define QDC_MBEDTLS_USER_H
/* Keep upstream algorithms and TLS 1.2/1.3. The hook verifies X.509 with the
 * authenticated application clock. TLS 1.3 PSA state needs pthread locking. */
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
#endif
