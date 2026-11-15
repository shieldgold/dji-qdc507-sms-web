#include "swap_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef SMS_SWAP_RED
/* Deliberately absent implementation for the initial test-first RED run. */
int swap_run_valid(const char *s) { (void)s; return 0; }
int swap_uint(const char *s, uint64_t *n) { (void)s; (void)n; return -1; }
int swap_control_parse(const char *s, const char *run, struct swap_control *c) { (void)s; (void)run; (void)c; return -1; }
int swap_mount_record(const char *s, const char *target, uint64_t *id) { (void)s; (void)target; (void)id; return -1; }
int swap_clock_proof(const char *s, const struct swap_control *c, int only) { (void)s; (void)c; (void)only; return -1; }
int swap_lease_due(double elapsed, double age) { (void)elapsed; (void)age; return 0; }
#endif

static const char valid[] =
    "QDC_SMS_SWAP_V1\nRUN LAB01\nBOOT 01234567-89ab-cdef-0123-456789abcdef\n"
    "SUP 431 813\nORIGINAL 11 22 6000000\nSOURCE 33 44 6000001\nCLOCK 2001 12346\nSECONDS 300\n";
static void replace(char *out, const char *from, const char *to) {
    const char *at = strstr(valid, from); assert(at);
    snprintf(out, 1024, "%.*s%s%s", (int)(at-valid), valid, to, at+strlen(from));
}
int main(void) {
    struct swap_control c; uint64_t id;
    assert(!swap_control_parse(valid, "LAB01", &c));
    assert(c.supervisor == 431 && c.sup_start == 813 && c.guardian == 2001 && c.new_ino == 44);
    assert(swap_control_parse(valid, "OTHER", &c));
    const char *from[] = {"RUN LAB01", "SUP 431", "SUP 431 813", "CLOCK 2001", "CLOCK 2001 12346", "SOURCE 33 44", "SOURCE 33 44 6000001", "SECONDS 300", "01234567-89ab"};
    const char *to[] = {"RUN ../bad", "SUP 0", "SUP 431 18446744073709551616", "CLOCK 431", "CLOCK 2001 0", "SOURCE 11 22", "SOURCE 33 44 16777217", "SECONDS 301", "01234567-zzab"};
    for (unsigned i=0; i<sizeof(from)/sizeof(from[0]); i++) {
        char text[1024]; replace(text, from[i], to[i]); assert(swap_control_parse(text, "LAB01", &c));
    }
    char text[1024]; snprintf(text, sizeof(text), "%sEXTRA 1\n", valid); assert(swap_control_parse(text, "LAB01", &c));
    strcpy(text, valid); text[strlen(text)-1]=0; assert(swap_control_parse(text, "LAB01", &c));
    assert(!swap_control_parse(valid, "LAB01", &c));
    assert(swap_mount_record("42 1 8:1 /source /data/smsweb/smsweb rw - ext4 /dev/mmcblk0 rw\n", "/data/smsweb/smsweb", &id)==1 && id==42);
    assert(swap_mount_record("43 1 8:1 /source /data/smsweb/smsweb-extra rw - ext4 /dev/mmcblk0 rw\n", "/data/smsweb/smsweb", &id)==0);
    /* QDC507 3.18.44 exposes its root mount with parent ID zero. */
    assert(swap_mount_record("10 0 0:11 / / ro,relatime - ubifs ubi0:rootfs ro,bulk_read,chk_data_crc\n", "/data/smsweb/smsweb", &id)==0);
    assert(swap_mount_record("0 0 0:11 / / ro - ubifs ubi0:rootfs ro\n", "/data/smsweb/smsweb", &id)<0);
    assert(swap_mount_record("42 0 8:1 /source /data/smsweb/smsweb rw - ext4 /dev/a rw\n", "/data/smsweb/smsweb", &id)<0);
    assert(swap_mount_record("42 0 8:1 /source /other rw - ext4 /dev/a rw\n", "/data/smsweb/smsweb", &id)<0);
    assert(swap_mount_record("10 18446744073709551616 0:11 / / ro - ubifs ubi0:rootfs ro\n", "/data/smsweb/smsweb", &id)<0);
    assert(swap_mount_record("18446744073709551616 1 8:1 /source /data/smsweb/smsweb rw - ext4 /dev/a rw\n", "/data/smsweb/smsweb", &id)<0);
    assert(swap_mount_record("42 1 8:1 /source /data/smsweb/smsweb rw", "/data/smsweb/smsweb", &id)<0);
    const char *ready="RESUME_GUARD_READY pid=2001 supervisor=431 start=813 seconds=300 restore_clock=1\n";
    snprintf(text, sizeof(text), "%sCLOCK_RESTORED epoch=1794700800 before_supervisor_cont=1\nRESUME_SENT verify_sms_health_separately=1\n", ready);
    assert(!swap_clock_proof(text, &c, 0));
    assert(swap_clock_proof(text, &c, 1));
    snprintf(text, sizeof(text), "%sCLOCK_RESTORED epoch=1794700800 before_supervisor_cont=1\nRESUME_DELEGATED verify_supervisor_before_cont=1\n", ready);
    assert(!swap_clock_proof(text, &c, 0));
    assert(!swap_clock_proof(text, &c, 1));
    const char *bad[] = {
        "RESUME_SENT verify_sms_health_separately=1\nCLOCK_RESTORED epoch=1794700800 before_supervisor_cont=1\n",
        "CLOCK_RESTORED epoch=1 before_supervisor_cont=0\nRESUME_SENT verify_sms_health_separately=1\n",
        "CLOCK_RESTORED epoch=1 before_supervisor_cont=1\nRECOVERY_REQUIRED clock_restore\nRESUME_SENT verify_sms_health_separately=1\n",
        "CLOCK_RESTORED epoch=1 before_supervisor_cont=1\nRESUME_SENT verify_sms_health_separately=1\nEXTRA\n",
        "CLOCK_RESTORED epoch=1 before_supervisor_cont=1\n",
        NULL};
    for (int i=0; bad[i]; i++) { snprintf(text, sizeof(text), "%s%s", ready,bad[i]); assert(swap_clock_proof(text,&c,0)); }
    assert(!swap_lease_due(299.9,0)); assert(swap_lease_due(300,0));
    assert(!swap_lease_due(1,269.9)); assert(swap_lease_due(1,270));
    assert(swap_lease_due(1,299));
    assert(!swap_uint("18446744073709551615", &id) && id==UINT64_MAX);
    assert(swap_uint("18446744073709551616", &id)); assert(swap_uint("-1", &id)); assert(swap_uint("01", &id));
    puts("SMS_SWAP_CORE_TESTS_OK"); return 0;
}
