#define _GNU_SOURCE
#include "coexist.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char valid[] =
    "QDC_COEXIST_V1\nRUN LAB01\nBOOT 01234567-89ab-cdef-0123-456789abcdef\n"
    "TOKEN LAB01_COEX\nSUP 431 813\nSMS 2000 12345\nRESUME 2001 12346\nHTTP 192.168.225.1\nSECONDS 30\n";

int main(void) {
    struct qdc_coexist c;
    assert(!qdc_coexist_parse(valid, &c));
    assert(!strcmp(c.run, "LAB01") && !strcmp(c.token, "LAB01_COEX"));
    assert(c.supervisor.pid == 431 && c.supervisor.start == 813);
    assert(c.original_sms.pid == 2000 && c.guardian.pid == 2001 && c.seconds == 30);
    const char *bad[] = {"", "QDC_COEXIST_V2\n", "QDC_COEXIST_V1\nRUN ../bad\n", NULL};
    for (int i = 0; bad[i]; i++) assert(qdc_coexist_parse(bad[i], &c));
    char text[2048];
    const char *from[] = {"SECONDS 30", "SUP 431", "RESUME 2001", "12346", "192.168.225.1", "01234567-89ab", "TOKEN LAB01_COEX"};
    const char *to[] = {"SECONDS 60", "SUP 0", "RESUME 431", "18446744073709551616", "8.8.8.8", "01234567-zzab", "TOKEN OTHER"};
    for (unsigned i = 0; i < sizeof(from)/sizeof(from[0]); i++) {
        const char *at = strstr(valid, from[i]); assert(at);
        snprintf(text, sizeof(text), "%.*s%s%s", (int)(at-valid), valid, to[i], at+strlen(from[i]));
        assert(qdc_coexist_parse(text, &c));
    }
    snprintf(text, sizeof(text), "%sUNKNOWN 1\n", valid);
    assert(qdc_coexist_parse(text, &c));
    strcpy(text, valid); text[strlen(text)-1] = 0;
    assert(qdc_coexist_parse(text, &c));
    puts("COEXIST_PARSE_TESTS_OK");
    return 0;
}
