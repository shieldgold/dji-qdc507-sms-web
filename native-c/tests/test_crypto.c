#include "qdc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef QDC_TEST_RED
int qdc_sha256(const void *p, size_t n, unsigned char out[32]) {
    (void)p;
    (void)n;
    (void)out;
    return -1;
}
int qdc_hmac256(const void *k, size_t kn, const void *p, size_t n, unsigned char out[32]) {
    (void)k;
    (void)kn;
    (void)p;
    (void)n;
    (void)out;
    return -1;
}
int qdc_pbkdf2(qdc_slice p, qdc_slice s, unsigned i, unsigned char out[32]) {
    (void)p;
    (void)s;
    (void)i;
    (void)out;
    return -1;
}
int qdc_random(void *p, size_t n, qdc_error *e) {
    (void)p;
    (void)n;
    (void)e;
    return -1;
}
int qdc_random_hex(char *p, size_t n, qdc_error *e) {
    (void)p;
    (void)n;
    (void)e;
    return -1;
}
bool qdc_constant_equal(const void *a, const void *b, size_t n) {
    (void)a;
    (void)b;
    (void)n;
    return false;
}
#endif
static void hex(const unsigned char *p, size_t n, char *s) {
    static const char t[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        s[i * 2] = t[p[i] >> 4];
        s[i * 2 + 1] = t[p[i] & 15];
    }
    s[n * 2] = 0;
}
int main(void) {
    unsigned char hash[32], a[64], b[64];
    char s[129];
    qdc_error e = {0};
    assert(qdc_sha256("abc", 3, hash) == 0);
    hex(hash, 32, s);
    assert(!strcmp(s, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    assert(qdc_hmac256("key", 3, "The quick brown fox jumps over the lazy dog", 43, hash) == 0);
    hex(hash, 32, s);
    assert(!strcmp(s, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"));
    assert(qdc_pbkdf2((qdc_slice){"password", 8}, (qdc_slice){"salt", 4}, 1, hash) == 0);
    hex(hash, 32, s);
    assert(!strcmp(s, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    assert(qdc_pbkdf2((qdc_slice){"password", 8}, (qdc_slice){"salt", 4}, 0, hash) == -1);
    assert(qdc_pbkdf2((qdc_slice){"pass\0word", 9}, (qdc_slice){"sa\0lt", 5}, 4096, hash) == 0);
    hex(hash, 32, s);
    assert(!strcmp(s, "89b69d0516f829893c696226650a86878c029ac13ee276509d5ae58b6466a724"));
    assert(qdc_random(a, sizeof(a), &e) == 0 && qdc_random(b, sizeof(b), &e) == 0);
    assert(!qdc_constant_equal(a, b, sizeof(a)));
    assert(qdc_constant_equal(a, a, sizeof(a)));
    assert(qdc_constant_equal(NULL, NULL, 0));
    assert(qdc_random_hex(s, 64, &e) == 0 && strlen(s) == 128);
    for (size_t i = 0; i < 128; i++)
        assert((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'));
    puts("crypto: PASS (published SHA/HMAC/PBKDF2 vectors, embedded NUL, entropy, constant "
         "comparison)");
    return 0;
}
