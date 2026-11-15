#define _GNU_SOURCE
#include "swap_core.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int swap_run_valid(const char *s) {
    size_t n = strlen(s);
    return n && n<=48 && strspn(s,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==n;
}
int swap_uint(const char *s, uint64_t *n) {
    if (!*s || (*s=='0' && s[1]) || strspn(s,"0123456789")!=strlen(s)) return -1;
    char *end; errno=0; unsigned long long value=strtoull(s,&end,10);
    if (errno || *end) return -1;
    *n=(uint64_t)value; return 0;
}
static int numbers(char *s, uint64_t *out, unsigned count) {
    for (unsigned i=0; i<count; i++) {
        char *space=strchr(s,' ');
        if ((i+1<count)!=!!space) return -1;
        if (space) *space=0;
        if (swap_uint(s,&out[i])) return -1;
        if (space) s=space+1;
    }
    return 0;
}
int swap_control_parse(const char *text, const char *run, struct swap_control *c) {
    if (!swap_run_valid(run) || strlen(text)>=1024) return -1;
    char copy[1024]; strcpy(copy,text); memset(c,0,sizeof(*c));
    static const char *keys[]={"QDC_SMS_SWAP_V1","RUN ","BOOT ","SUP ","ORIGINAL ","SOURCE ","CLOCK ","SECONDS "};
    char *p=copy;
    for (unsigned i=0; i<sizeof(keys)/sizeof(keys[0]); i++) {
        char *end=strchr(p,'\n'); if (!end) return -1; *end=0;
        size_t n=strlen(keys[i]);
        if (strncmp(p,keys[i],n) || (!i && p[n])) return -1;
        char *value=p+n; uint64_t v[3];
        if (i==1) { if (strcmp(value,run)) return -1; strcpy(c->run,value); }
        else if (i==2) {
            if (strlen(value)!=36) return -1;
            for (int j=0; j<36; j++) {
                if (j==8 || j==13 || j==18 || j==23) { if (value[j]!='-') return -1; }
                else if (!strchr("0123456789abcdef",value[j])) return -1;
            }
            strcpy(c->boot,value);
        } else if (i==3 || i==6) {
            if (numbers(value,v,2) || !v[0] || v[0]>INT_MAX || !v[1]) return -1;
            if (i==3) { c->supervisor=(pid_t)v[0]; c->sup_start=v[1]; }
            else { c->guardian=(pid_t)v[0]; c->guard_start=v[1]; }
        } else if (i==4 || i==5) {
            if (numbers(value,v,3) || !v[1] || !v[2] || v[2]>QDC_SMS_SWAP_MAX_BYTES) return -1;
            if (i==4) { c->old_dev=v[0]; c->old_ino=v[1]; c->old_size=v[2]; }
            else { c->new_dev=v[0]; c->new_ino=v[1]; c->new_size=v[2]; }
        } else if (i==7 && strcmp(value,"300")) return -1;
        p=end+1;
    }
    return *p || c->supervisor==c->guardian || (c->old_dev==c->new_dev && c->old_ino==c->new_ino) ? -1:0;
}
int swap_mount_record(const char *line, const char *target, uint64_t *id) {
    size_t n=strlen(line);
    if (!n || n>=4096 || line[n-1]!='\n' || !strstr(line," - ")) return -1;
    char copy[4096]; strcpy(copy,line); char *save=NULL, *fields[6];
    for (int i=0; i<6; i++) { fields[i]=strtok_r(i?NULL:copy," \n",&save); if (!fields[i]) return -1; }
    uint64_t mount, parent;
    if (swap_uint(fields[0],&mount) || !mount || mount>INT_MAX || swap_uint(fields[1],&parent) || parent>INT_MAX) return -1;
    /* The device's 3.18 kernel reports parent=0 for the root mount only.
     * Keep rejecting a zero parent for the target or any other mountpoint. */
    if (!parent && strcmp(fields[4],"/")) return -1;
    if (strcmp(fields[4],target)) return 0;
    *id=mount; return 1;
}
int swap_clock_proof(const char *text, const struct swap_control *c, int delegated_only) {
    char ready[192];
    int n=snprintf(ready,sizeof(ready),"RESUME_GUARD_READY pid=%ld supervisor=%ld start=%" PRIu64 " seconds=300 restore_clock=1\n",(long)c->guardian,(long)c->supervisor,c->sup_start);
    if (n<0 || (size_t)n>=sizeof(ready) || strncmp(text,ready,(size_t)n)) return -1;
    const char *p=text+n, *prefix="CLOCK_RESTORED epoch=";
    if (strncmp(p,prefix,strlen(prefix))) return -1;
    p+=strlen(prefix); errno=0; char *end; (void)strtoll(p,&end,10);
    if (errno || end==p || (*p!='-' && (*p<'0' || *p>'9'))) return -1;
    if (!strcmp(end," before_supervisor_cont=1\nRESUME_DELEGATED verify_supervisor_before_cont=1\n")) return 0;
    return !delegated_only && !strcmp(end," before_supervisor_cont=1\nRESUME_SENT verify_sms_health_separately=1\n") ? 0:-1;
}
int swap_lease_due(double elapsed, double guardian_age) {
    return elapsed>=QDC_SMS_SWAP_SECONDS || guardian_age>=QDC_SMS_SWAP_SECONDS-30;
}
