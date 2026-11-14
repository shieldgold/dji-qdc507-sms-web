#define _GNU_SOURCE
#include "proc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Exercise the public reader with deterministic short reads and errors, without
 * opening a device or relying on a host /proc filesystem. */
static int smaps_open(const char *, int, ...);
static ssize_t smaps_read(int, void *, size_t);
static int smaps_close(int);
#define open smaps_open
#define read smaps_read
#define close smaps_close
#include "proc.c"
#undef open
#undef read
#undef close

static struct {
    const char *text;
    size_t length, offset, max_request, chunk, error_after, interrupt_call, reads;
    int opens, closes, eof, open_error;
} source;

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #test); return 1; \
} } while (0)

static void input(const char *text, size_t length, size_t chunk) {
    memset(&source, 0, sizeof(source));
    source.text = text; source.length = length; source.chunk = chunk;
    source.error_after = (size_t)-1;
}
static int smaps_open(const char *path, int flags, ...) {
    if (strcmp(path, "/proc/42/smaps") ||
        flags != (O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) abort();
    source.opens++;
    if (source.open_error) { errno = source.open_error; return -1; }
    return 42;
}
static ssize_t smaps_read(int fd, void *out, size_t cap) {
    if (fd != 42 || !cap) abort();
    source.reads++;
    if (cap > source.max_request) source.max_request = cap;
    if (source.reads == source.interrupt_call) { errno = EINTR; return -1; }
    if (source.offset >= source.error_after) { errno = EIO; return -1; }
    size_t n = source.length - source.offset;
    if (n > cap) n = cap;
    if (source.chunk && n > source.chunk) n = source.chunk;
    if (n > source.error_after - source.offset) n = source.error_after - source.offset;
    if (!n) { source.eof++; return 0; }
    memcpy(out, source.text + source.offset, n); source.offset += n;
    return (ssize_t)n;
}
static int smaps_close(int fd) {
    if (fd != 42) abort();
    source.closes++; return 0;
}
static int same(const struct qdc_smaps *a, const struct qdc_smaps *b) {
    return a->rss == b->rss && a->pss == b->pss && a->anonymous == b->anonymous &&
        a->dirty == b->dirty && a->clean == b->clean;
}

static int bounded_reads(void) {
    const char text[] = "Rss: 9 kB\nPss: 6 kB\nAnonymous: 2 kB\nPrivate_Dirty: 3 kB\nPrivate_Clean: 4 kB\n";
    struct qdc_smaps actual, expected = {9, 6, 2, 3, 4};
    input(text, sizeof(text)-1, 3);
    CHECK(!proc_smaps(42, &actual));
    CHECK(same(&actual, &expected));
    CHECK(source.max_request <= 512);
    CHECK(source.offset == source.length && source.eof == 1);
    CHECK(source.opens == 1 && source.closes == 1);
    return 0;
}
static int parser_parity(void) {
    static const char *const cases[] = {
        ("001000-002000 rw-p 0 00:00 0 /first\nRss: 10 kB\nPss: 8 kB\nAnonymous: 7 kB\nPrivate_Dirty: 6 kB\nPrivate_Clean: 5 kB\n"
         "002000-003000 r--p 0 00:00 0 /last\nRss: 4 kB\nPss: 3 kB\nAnonymous: 2 kB\nPrivate_Dirty: 1 kB\nPrivate_Clean: 2 kB\n"),
        "Rss: +12 suffix\nPss: -2\nAnonymous:\t3ignored\nPrivate_Dirty: 4.5\nPrivate_Clean: 5x",
        " Rss: 99 kB\nRss: broken\nRssX: 99 kB\nRss: -\nRss: 0 kB\nPss: +\nVmRSS: 50 kB\n",
        "Rss:\n \t\r\v\f7 kB\nPss:\n3 kB\nPrivate_Clean:\nAnonymous: 4 kB\n",
        "Rss: 7 kB\nPrivate_Dirty: ",
        "Rss: 7",
        "Pss: 4 kB\nVmRSS: 99 kB\n",
        "Rss: -",
        "Rss:",
        ""
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        struct qdc_smaps expected;
        int want = qdc_parse_smaps(cases[i], &expected);
        for (size_t chunk = 1; chunk <= 64; chunk++) {
            struct qdc_smaps actual;
            input(cases[i], strlen(cases[i]), chunk);
            CHECK(proc_smaps(42, &actual) == want);
            if (!want) CHECK(same(&actual, &expected));
            CHECK(source.eof == 1 && source.closes == 1);
        }
    }
    return 0;
}
static int all_vmas_and_long_lines(void) {
    size_t length = 90000;
    char *text = malloc(length+1); CHECK(text);
    memset(text, 'x', length);
    const char first[] = "Rss: 1 kB\nPss: 2 kB\n";
    const char last[] = "\nRss: 11 kB\nPss: 13 kB\nAnonymous: 17 kB\nPrivate_Dirty: 19 kB\nPrivate_Clean: 23 kB\n";
    memcpy(text, first, sizeof(first)-1);
    memcpy(text+length-(sizeof(last)-1), last, sizeof(last)-1); text[length] = 0;
    struct qdc_smaps actual, expected;
    CHECK(!qdc_parse_smaps(text, &expected));
    input(text, length, 509);
    CHECK(!proc_smaps(42, &actual));
    CHECK(same(&actual, &expected));
    CHECK(actual.rss == 12 && actual.pss == 15 && actual.clean == 23);
    CHECK(source.offset == length && source.eof == 1);
    free(text); return 0;
}
static int input_limit(void) {
    char *text = malloc(131074); CHECK(text);
    memset(text, 'x', 131074);
    memcpy(text, "Rss: 1 kB\n", 10);
    struct qdc_smaps actual;
    input(text, 131070, 0);
    CHECK(!proc_smaps(42, &actual));
    CHECK(actual.rss == 1 && source.eof == 1);
    for (size_t length = 131071; length <= 131073; length++) {
        struct qdc_smaps before = {31, 32, 33, 34, 35}; actual = before;
        input(text, length, 0); errno = 0;
        CHECK(proc_smaps(42, &actual) == -1);
        CHECK(errno == EOVERFLOW);
        CHECK(same(&actual, &before));
        CHECK(source.closes == 1);
    }
    free(text); return 0;
}
static int read_errors(void) {
    const char text[] = "Rss: 5 kB\nPss: 3 kB\n";
    struct qdc_smaps before = {31, 32, 33, 34, 35}, actual;
    input(text, sizeof(text)-1, 1); source.open_error = EACCES; actual = before;
    CHECK(proc_smaps(42, &actual) == -1 && errno == EACCES);
    CHECK(source.reads == 0 && source.closes == 0 && same(&actual, &before));
    for (size_t after = 0; after < sizeof(text)-1; after++) {
        input(text, sizeof(text)-1, 1); source.error_after = after; actual = before;
        CHECK(proc_smaps(42, &actual) == -1 && errno == EIO);
        CHECK(source.closes == 1 && same(&actual, &before));
    }
    for (size_t interrupt = 1; interrupt <= sizeof(text); interrupt++) {
        input(text, sizeof(text)-1, 1); source.interrupt_call = interrupt;
        CHECK(!proc_smaps(42, &actual));
        CHECK(actual.rss == 5 && actual.pss == 3 && source.eof == 1);
    }
    return 0;
}
static int number_overflow(void) {
    static const char *const fields[] = {"Rss", "Pss", "Anonymous", "Private_Dirty", "Private_Clean"};
    char text[256]; struct qdc_smaps actual;
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
        snprintf(text, sizeof(text), "Rss: 0 kB\n%s: %lu kB\n", fields[i], (unsigned long)LONG_MAX+1UL);
        input(text, strlen(text), 1); errno = 0;
        CHECK(proc_smaps(42, &actual) == -1 && errno == EOVERFLOW);
        snprintf(text, sizeof(text), "Rss: 0 kB\n%s: -%lu kB\n", fields[i], (unsigned long)LONG_MAX+2UL);
        input(text, strlen(text), 1); errno = 0;
        CHECK(proc_smaps(42, &actual) == -1 && errno == EOVERFLOW);
    }
    const char huge[] = "Rss: 999999999999999999999999999999999999999999 kB\n";
    input(huge, sizeof(huge)-1, 1);
    CHECK(proc_smaps(42, &actual) == -1);
    return 0;
}
static int total_overflow_and_limits(void) {
    static const char *const fields[] = {"Rss", "Pss", "Anonymous", "Private_Dirty", "Private_Clean"};
    char text[256]; struct qdc_smaps actual, expected;
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
        snprintf(text, sizeof(text), "Rss: 0 kB\n%s: %ld kB\n%s: 1 kB\n", fields[i], LONG_MAX, fields[i]);
        input(text, strlen(text), 7); errno = 0;
        CHECK(proc_smaps(42, &actual) == -1 && errno == EOVERFLOW);
        snprintf(text, sizeof(text), "Rss: 0 kB\n%s: %ld kB\n%s: -1 kB\n", fields[i], LONG_MIN, fields[i]);
        input(text, strlen(text), 7); errno = 0;
        CHECK(proc_smaps(42, &actual) == -1 && errno == EOVERFLOW);
        snprintf(text, sizeof(text), "Rss: 0 kB\n%s: %ld kB\n%s: %ld kB\n", fields[i], LONG_MAX, fields[i], LONG_MIN);
        CHECK(!qdc_parse_smaps(text, &expected)); input(text, strlen(text), 1);
        CHECK(!proc_smaps(42, &actual) && same(&actual, &expected));
    }
    return 0;
}
static int embedded_nul(void) {
    const char text[] = "Rss: 7 kB\0\nRss: 99 kB\n";
    struct qdc_smaps actual, expected;
    CHECK(!qdc_parse_smaps(text, &expected));
    input(text, sizeof(text)-1, 1);
    CHECK(!proc_smaps(42, &actual) && same(&actual, &expected));
    CHECK(source.offset == source.length && source.eof == 1);
    return 0;
}
int main(void) {
    static const struct { const char *name; int (*run)(void); } tests[] = {
        {"bounded_reads", bounded_reads}, {"parser_parity", parser_parity},
        {"all_vmas_and_long_lines", all_vmas_and_long_lines}, {"input_limit", input_limit},
        {"read_errors", read_errors}, {"number_overflow", number_overflow},
        {"total_overflow_and_limits", total_overflow_and_limits}, {"embedded_nul", embedded_nul}
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
        int result = tests[i].run(); failed += result;
        printf("%s %s\n", result ? "FAIL" : "PASS", tests[i].name);
    }
    if (failed) return 1;
    puts("PROC_SMAPS_TESTS_OK"); return 0;
}
