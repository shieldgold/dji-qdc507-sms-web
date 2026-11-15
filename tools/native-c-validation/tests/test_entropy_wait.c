#include "../entropy_wait.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct fake {
    long now, available, later_available;
    unsigned draws, discards, success_after;
    int random_error, discard_error;
    ssize_t positive_short;
};
static long now_ms(void *p) { return ((struct fake *)p)->now; }
static long available_kib(void *p) {
    struct fake *f=p;
    return f->draws && f->later_available ? f->later_available : f->available;
}
static ssize_t random_read(void *p) {
    struct fake *f=p;f->draws++;
    if(f->positive_short) return f->positive_short;
    if(f->success_after && f->draws>=f->success_after) return 32;
    errno=f->random_error;return -1;
}
static int discard(void *p) {
    struct fake *f=p;f->discards++;
    if(f->discard_error) { errno=f->discard_error;return -1; }
    return 0;
}
static int sleep_ms(void *p,unsigned ms) { ((struct fake *)p)->now+=(long)ms;return 0; }
static struct qdc_entropy_ops ops(struct fake *f) {
    return (struct qdc_entropy_ops){now_ms,available_kib,random_read,discard,sleep_ms,f};
}
static struct fake unready(void) { return (struct fake){.available=30000,.random_error=EAGAIN}; }
static void gates(void) {
    assert(qdc_entropy_gate("console=x qdc.sms.lab=1 qdc.sms.case=ready\n")==1);
    assert(qdc_entropy_gate("qdc.coexist.lab=1 qdc.sms.combo=1")==2);
    const char *bad[]={"", "qdc.sms.lab=10", "prefixqdc.sms.lab=1", "qdc.sms.lab=1suffix",
        "qdc.coexist.lab=1", "qdc.sms.combo=1", "qdc.coexist.lab=1 qdc.sms.combo=10",
        "qdc.sms.lab=1 qdc.sms.lab=0", "qdc.sms.lab=1 qdc.sms.lab=1"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) assert(!qdc_entropy_gate(bad[i]));
}
int main(void) {
    gates();struct fake f=unready();struct qdc_entropy_result r;struct qdc_entropy_ops o;
    f.success_after=1;o=ops(&f);assert(!qdc_entropy_wait(&o,&r));
    assert(f.draws==1 && f.discards==0 && r.getrandom_return==32 && !r.error);
    f=unready();f.success_after=3;o=ops(&f);assert(!qdc_entropy_wait(&o,&r));
    assert(f.discards>0 && r.attempts==3 && r.elapsed_ms>0);
    f=unready();o=ops(&f);assert(qdc_entropy_wait(&o,&r));
    assert(!strcmp(r.reason,"timeout") && r.elapsed_ms==90000 && r.getrandom_return==-1 && r.error==EAGAIN);
    f=unready();f.random_error=ENOSYS;o=ops(&f);assert(qdc_entropy_wait(&o,&r));
    assert(!strcmp(r.reason,"getrandom") && f.discards==0);
    f=unready();f.positive_short=16;o=ops(&f);assert(qdc_entropy_wait(&o,&r));
    assert(!strcmp(r.reason,"getrandom-short") && f.discards==0);
    f=unready();f.discard_error=EIO;o=ops(&f);assert(qdc_entropy_wait(&o,&r));
    assert(!strcmp(r.reason,"urandom-discard") && r.error==EIO);
    f=unready();f.available=8191;o=ops(&f);assert(qdc_entropy_wait(&o,&r));
    assert(!strcmp(r.reason,"admission") && !f.draws);
    f=unready();f.later_available=6143;o=ops(&f);assert(qdc_entropy_wait(&o,&r));
    assert(!strcmp(r.reason,"memory-floor"));
    f=unready();f.available=10000;f.later_available=6800;o=ops(&f);assert(qdc_entropy_wait(&o,&r));
    assert(!strcmp(r.reason,"memory-drop"));
    f=unready();f.now=-1;o=ops(&f);assert(qdc_entropy_wait(&o,&r));
    assert(!strcmp(r.reason,"clock"));
    puts("ENTROPY_WAIT_TEST_PASS gates=11 core_cases=10");return 0;
}
