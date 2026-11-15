/* Disposable VM preflight. Never consumes urandom output as a secret. */
#define _GNU_SOURCE
#include "entropy_wait.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int qdc_entropy_gate(const char *line) {
    static const char *keys[]={"qdc.sms.lab=","qdc.coexist.lab=","qdc.sms.combo="};
    unsigned seen[3]={0};
    if(!line) return 0;
    while(*line) {
        while(isspace((unsigned char)*line)) line++;
        const char *end=line;
        while(*end && !isspace((unsigned char)*end)) end++;
        size_t length=(size_t)(end-line);
        for(unsigned i=0;i<3;i++) {
            size_t key_length=strlen(keys[i]);
            if(length>=key_length && !memcmp(line,keys[i],key_length)) {
                if(seen[i]++ || length!=key_length+1 || line[key_length]!='1') return 0;
            }
        }
        line=end;
    }
    if(seen[1] && seen[2]) return 2;
    return seen[0] ? 1 : 0;
}

static int guard(const struct qdc_entropy_ops *ops,struct qdc_entropy_result *r,
                 long started,long baseline) {
    long now=ops->now_ms(ops->context);
    if(now<started) { r->reason="clock";return -1; }
    r->elapsed_ms=now-started;
    long available=ops->available_kib(ops->context);
    if(available<0) { r->reason="memory-read";return -1; }
    if(available<r->min_available_kib) r->min_available_kib=available;
    if(available<QDC_ENTROPY_FLOOR_KIB) { r->reason="memory-floor";return -1; }
    if(baseline-available>QDC_ENTROPY_DROP_KIB) { r->reason="memory-drop";return -1; }
    if(r->elapsed_ms>=QDC_ENTROPY_TIMEOUT_MS) { r->reason="timeout";return -1; }
    return 0;
}

int qdc_entropy_wait(const struct qdc_entropy_ops *ops,struct qdc_entropy_result *r) {
    *r=(struct qdc_entropy_result){.reason="clock",.getrandom_return=-1};
    long started=ops->now_ms(ops->context);
    if(started<0) return -1;
    long baseline=ops->available_kib(ops->context);
    r->min_available_kib=baseline;
    if(baseline<QDC_ENTROPY_ADMISSION_KIB) { r->reason="admission";return -1; }
    for(;;) {
        if(guard(ops,r,started,baseline)) return -1;
        errno=0;
        r->getrandom_return=ops->getrandom_nonblock(ops->context);
        r->error=errno;
        r->attempts++;
        if(guard(ops,r,started,baseline)) return -1;
        if(r->getrandom_return==32) { r->error=0;r->reason="ready";return 0; }
        if(r->getrandom_return>=0) { r->reason="getrandom-short";return -1; }
        if(r->error!=EAGAIN) { r->reason="getrandom";return -1; }
        /* On 3.18 this may trigger a credited input-pool transfer. Discarded
         * bytes are never accepted as evidence that getrandom is ready. */
        if(ops->discard_urandom(ops->context)) {
            r->error=errno;r->reason="urandom-discard";return -1;
        }
        if(guard(ops,r,started,baseline)) return -1;
        long remaining=QDC_ENTROPY_TIMEOUT_MS-r->elapsed_ms;
        unsigned delay=(unsigned)(remaining<100?remaining:100);
        if(ops->sleep_ms(ops->context,delay)) {
            r->error=errno;r->reason="sleep";return -1;
        }
    }
}

#ifndef QDC_ENTROPY_NO_MAIN
#include <fcntl.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void wipe(unsigned char *p,size_t size) {
    volatile unsigned char *v=p;
    while(size--) *v++=0;
}
static long monotonic_ms(void *context) {
    (void)context;struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now) || now.tv_sec<0 || now.tv_sec>2147483) return -1;
    return (long)now.tv_sec*1000L+now.tv_nsec/1000000L;
}
static long available_kib(void *context) {
    (void)context;FILE *file=fopen("/proc/meminfo","r");
    if(!file) return -1;
    char line[256];long available=-1;
    while(fgets(line,sizeof(line),file)) {
        if(sscanf(line,"MemAvailable: %ld kB",&available)==1) break;
    }
    fclose(file);return available;
}
static ssize_t random_nonblock(void *context) {
    (void)context;unsigned char buffer[32];
    ssize_t result=getrandom(buffer,sizeof(buffer),GRND_NONBLOCK);
    int saved=errno;wipe(buffer,sizeof(buffer));errno=saved;return result;
}
static int discard_urandom(void *context) {
    (void)context;unsigned char buffer[32];
    int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);
    if(fd<0) return -1;
    struct stat state;
    if(fstat(fd,&state) || !S_ISCHR(state.st_mode)) {
        close(fd);errno=ENODEV;return -1;
    }
    ssize_t count=read(fd,buffer,sizeof(buffer));int saved=errno;
    close(fd);wipe(buffer,sizeof(buffer));
    if(count!=(ssize_t)sizeof(buffer)) { errno=count<0?saved:EIO;return -1; }
    return 0;
}
static int sleep_ms(void *context,unsigned milliseconds) {
    (void)context;struct timespec delay={milliseconds/1000,(long)(milliseconds%1000)*1000000L};
    while(nanosleep(&delay,&delay)) if(errno!=EINTR) return -1;
    return 0;
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    int release_mounts=argc==2 && !strcmp(argv[1],"--release-lab-mounts");
    char commandline[4096];FILE *file=fopen("/proc/cmdline","r");
    if((argc!=1 && !release_mounts) || !file) { puts("QDC_ENTROPY_FAILED reason=lab-gate");return 64; }
    size_t length=fread(commandline,1,sizeof(commandline)-1,file);
    int extra=fgetc(file),bad=ferror(file);fclose(file);commandline[length]=0;
    int gate=bad || extra!=EOF || memchr(commandline,0,length)?0:qdc_entropy_gate(commandline);
    if(!gate) { puts("QDC_ENTROPY_FAILED reason=lab-gate");return 64; }
    printf("QDC_ENTROPY_PREFLIGHT gate=%s timeout_ms=90000 admission_kib=8192 absolute_floor_kib=6144 max_drop_kib=3072\n",gate==1?"sms":"coexist");
    const struct qdc_entropy_ops ops={monotonic_ms,available_kib,random_nonblock,discard_urandom,sleep_ms,NULL};
    struct qdc_entropy_result result;
    int rc=qdc_entropy_wait(&ops,&result);
    if(!rc && release_mounts && (umount("/dev") || umount("/proc"))) {
        result.reason="release-lab-mounts";result.error=errno;rc=-1;
    }
    printf("QDC_ENTROPY_%s elapsed_ms=%ld getrandom_return=%ld errno=%d attempts=%u min_available_kib=%ld reason=%s\n",
        rc?"FAILED":"READY",result.elapsed_ms,(long)result.getrandom_return,result.error,
        result.attempts,result.min_available_kib,result.reason);
    return rc?1:0;
}
#endif
