#include "lab.h"
#include <assert.h>

int main(void) {
    assert(lab_gate("console=ttyAMA0 qdc.sms.lab=1 qdc.tls.lab=1 maxcpus=1"));
    assert(lab_gate("\tqdc.tls.lab=1\nqdc.sms.lab=1 "));
    const char *bad[] = {NULL, "", "qdc.sms.lab=1", "qdc.tls.lab=1",
        "qdc.sms.lab=11 qdc.tls.lab=1", "qdc.sms.lab=1 qdc.tls.lab=0",
        "qdc.sms.lab=1 qdc.tls.lab=1 qdc.tls.lab=1",
        "qdc.sms.lab=1 qdc.tls.lab=1 qdc.sms.lab=1",
        "qdc.sms.lab=1 qdc.tls.lab=1 qdc.coexist.lab=1",
        "qdc.sms.lab=1 qdc.tls.lab=1 qdc.sms.combo=1",
        "xqdc.sms.lab=1 qdc.tls.lab=1", "qdc.sms.lab=1 xqdc.tls.lab=1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) assert(!lab_gate(bad[i]));
    assert(!lab_memory_reason(8192, 8192));
    assert(!strcmp(lab_memory_reason(8191, 8191), "admission"));
    assert(!strcmp(lab_memory_reason(10000, -1), "memory-read"));
    assert(!strcmp(lab_memory_reason(10000, 6143), "memory-floor"));
    assert(!strcmp(lab_memory_reason(10000, 6927), "memory-drop"));
    assert(!lab_memory_reason(10000, 6928));
    puts("ARM TLS guard unit: PASS (exact dual gate and unchanged 8/6/3 MiB boundaries)");
    return 0;
}
