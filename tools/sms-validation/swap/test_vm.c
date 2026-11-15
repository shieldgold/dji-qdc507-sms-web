/* Test-only ARM guest driver. Requires explicit qdc.sms.swap.lab=1 boot flag. */
#define _GNU_SOURCE
#include "../../ike-validation/runtime/proc.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char target[]="/data/smsweb/smsweb";
struct sample { char run[32],ram[128],data[128],clock[128],helper[160],log[160],guard[160]; pid_t sup,guardian,lease; unsigned long long supstart,guardstart; struct stat old,source; };
static void require(int ok,const char *why) { if (!ok) { printf("SMS_SWAP_VM_FAILED reason=%s errno=%d\n",why,errno); exit(1); } }
static void path(char *out,size_t cap,const char *root,const char *name) { require(snprintf(out,cap,"%s/%s",root,name)>0,"path"); }
static void copy(const char *from,const char *to,mode_t mode) {
    int in=open(from,O_RDONLY),out=open(to,O_WRONLY|O_CREAT|O_TRUNC,mode); require(in>=0 && out>=0,"fixture_copy_open");
    char buf[4096]; ssize_t n; while ((n=read(in,buf,sizeof(buf)))>0) require(write(out,buf,(size_t)n)==n,"fixture_copy_write");
    require(!n && !fchmod(out,mode),"fixture_copy_end"); close(in); close(out);
}
static int equal_files(const char *a,const char *b) {
    int x=open(a,O_RDONLY),y=open(b,O_RDONLY); if (x<0 || y<0) return 0;
    char p[4096],q[4096]; int same=1;
    for (;;) { ssize_t n=read(x,p,sizeof(p)),m=read(y,q,sizeof(q)); if (n<0 || m!=n || (n && memcmp(p,q,(size_t)n))) { same=0; break; } if (!n) break; }
    close(x); close(y); return same;
}
static void write_text(const char *name,const char *text,mode_t mode) {
    int fd=open(name,O_WRONLY|O_CREAT|O_TRUNC,mode); require(fd>=0,"fixture_text");
    require(write(fd,text,strlen(text))==(ssize_t)strlen(text) && !fchmod(fd,mode),"fixture_text_write"); close(fd);
}
static int contains(const char *name,const char *needle) { char text[8192]; return read_complete(name,text,sizeof(text))>=0 && strstr(text,needle)!=NULL; }
static void wait_text(const char *name,const char *needle) {
    for (int i=0;i<250;i++) { if (contains(name,needle)) return; usleep(20000); }
    char text[8192]; if (read_complete(name,text,sizeof(text))>=0) printf("FIXTURE_LOG %s\n%s",name,text);
    require(0,"fixture_text_deadline");
}
static int wait_code(pid_t pid) { int status; require(waitpid(pid,&status,0)==pid,"fixture_wait"); return WIFEXITED(status)?WEXITSTATUS(status):128; }
static pid_t spawn(const char *exe,char *const argv[],const char *input,const char *output,int failclock) {
    pid_t pid=fork(); require(pid>=0,"fixture_fork");
    if (!pid) {
        int in=open(input?input:"/dev/null",O_RDONLY),out=open(output,O_WRONLY|O_CREAT|O_TRUNC,0600);
        if (in<0 || out<0 || dup2(in,0)<0 || dup2(out,1)<0 || dup2(out,2)<0) _exit(126);
        close(in); close(out);
        if (failclock==1) setenv("SMS_SWAP_FIXTURE_CLOCK_FAIL","1",1);
        if (failclock==2) setenv("SMS_SWAP_FIXTURE_CLOCK_OVERLAY","1",1);
        execv(exe,argv); _exit(127);
    }
    return pid;
}
static int state(pid_t pid) {
    char name[64],text[4096]; snprintf(name,sizeof(name),"/proc/%ld/stat",(long)pid);
    if (read_complete(name,text,sizeof(text))<0) return 0;
    char *end=strrchr(text,')'); return end?end[2]:0;
}
static pid_t child_inode(pid_t parent,const struct stat *expected) {
    DIR *dir=opendir("/proc"); require(dir!=NULL,"fixture_proc"); struct dirent *entry; pid_t found=0;
    while ((entry=readdir(dir))) {
        char *end; long pid=strtol(entry->d_name,&end,10); if (*end || pid<1) continue;
        struct proc_id id; struct stat s; char name[64]; snprintf(name,sizeof(name),"/proc/%ld/exe",pid);
        if (!proc_identity((pid_t)pid,&id) && id.parent==parent && !stat(name,&s) && s.st_dev==expected->st_dev && s.st_ino==expected->st_ino) { found=(pid_t)pid; break; }
    }
    closedir(dir); return found;
}
static pid_t wait_child(pid_t parent,const struct stat *inode) {
    for (int i=0;i<250;i++) { pid_t pid=child_inode(parent,inode); if (pid) return pid; usleep(20000); }
    require(0,"fixture_child_deadline"); return 0;
}
static int command(struct sample *s,const char *verb,const char *bytes,const char *input) {
    char output[160]; path(output,sizeof(output),s->ram,"command.log");
    char *args[]={s->helper,(char *)verb,s->run,(char *)bytes,NULL}; if (!bytes) args[3]=NULL;
    return wait_code(spawn(s->helper,args,input,output,0));
}
static void prepare(struct sample *s,int index,int clockfail) {
    memset(s,0,sizeof(*s)); snprintf(s->run,sizeof(s->run),"VM%02d",index);
    snprintf(s->ram,sizeof(s->ram),"/run/qdc-sms-%s",s->run); snprintf(s->data,sizeof(s->data),"/data/qdc-sms-%s",s->run); snprintf(s->clock,sizeof(s->clock),"/run/qdc-tun-%s",s->run);
    require(!mkdir(s->ram,0700) && !mkdir(s->clock,0700),"fixture_private_dirs");
    path(s->helper,sizeof(s->helper),s->ram,"qdc-sms-swap"); path(s->log,sizeof(s->log),s->ram,"swap.log"); path(s->guard,sizeof(s->guard),s->clock,"qdc-ike-guard");
    copy("/fixtures/helper",s->helper,0500); copy("/fixtures/original",target,0700); copy("/fixtures/guardian",s->guard,0500);
    require(!stat(target,&s->old),"fixture_original_stat"); struct stat binary; require(!stat("/fixtures/optimized",&binary),"fixture_new_stat");
    char bytes[32]; snprintf(bytes,sizeof(bytes),"%lld",(long long)binary.st_size);
    require(!command(s,"copy",bytes,"/fixtures/optimized"),"copy_candidate");
    char source[160]; path(source,sizeof(source),s->data,"smsweb"); require(!stat(source,&s->source),"fixture_source_stat");
    require(command(s,"copy",bytes,"/fixtures/optimized")!=0 && equal_files(source,"/fixtures/optimized"),"refuse_copy_overwrite");
    char *sup_argv[]={"/bin/busybox","/bin/sh","/data/smsweb/start.sh","supervise",NULL};
    s->sup=spawn("/bin/busybox",sup_argv,NULL,"/run/supervisor.log",0); pid_t old=wait_child(s->sup,&s->old);
    require(!kill(s->sup,SIGSTOP),"fixture_stop_supervisor"); for (int i=0;i<100 && state(s->sup)!='T';i++) usleep(10000);
    require(state(s->sup)=='T' && !kill(old,SIGTERM),"fixture_original_term");
    for (int i=0;i<100 && state(old) && state(old)!='Z';i++) usleep(10000);
    require(!state(old) || state(old)=='Z',"fixture_original_exited"); struct proc_id id;
    require(!proc_identity(s->sup,&id) && supervisor_matches(id.pid,id.start),"fixture_exact_supervisor"); s->supstart=id.start;
    char pid[32],start[32],clocklog[160]; snprintf(pid,sizeof(pid),"%ld",(long)s->sup); snprintf(start,sizeof(start),"%llu",s->supstart); path(clocklog,sizeof(clocklog),s->clock,"sms-guard.log");
    char *guardargs[]={s->guard,"resume-clock",pid,start,"300",NULL}; s->guardian=spawn(s->guard,guardargs,NULL,clocklog,clockfail);
    wait_text(clocklog,"RESUME_GUARD_READY"); require(!proc_identity(s->guardian,&id),"fixture_guard_identity"); s->guardstart=id.start;
    char boot[64],control[1024],name[160]; require(read_complete("/proc/sys/kernel/random/boot_id",boot,sizeof(boot))>0,"fixture_boot"); boot[strcspn(boot,"\n")]=0;
    snprintf(control,sizeof(control),"QDC_SMS_SWAP_V1\nRUN %s\nBOOT %s\nSUP %ld %llu\nORIGINAL %llu %llu %llu\nSOURCE %llu %llu %llu\nCLOCK %ld %llu\nSECONDS 300\n",s->run,boot,(long)s->sup,s->supstart,(unsigned long long)s->old.st_dev,(unsigned long long)s->old.st_ino,(unsigned long long)s->old.st_size,(unsigned long long)s->source.st_dev,(unsigned long long)s->source.st_ino,(unsigned long long)s->source.st_size,(long)s->guardian,s->guardstart);
    path(name,sizeof(name),s->ram,"lease.control"); write_text(name,control,0600);
    int checked=command(s,"check",NULL,NULL);
    if (checked) { char diag[8192],output[160]; path(output,sizeof(output),s->ram,"command.log"); if (read_complete(output,diag,sizeof(diag))>=0) printf("CHECK_DIAGNOSTIC %s",diag); }
    require(!checked,"preflight_exact_identity");
}
static void start_lease(struct sample *s) {
    char output[160]; path(output,sizeof(output),s->ram,"helper.stdout"); char *args[]={s->helper,"hold",s->run,NULL};
    s->lease=spawn(s->helper,args,NULL,output,0); wait_text(s->log,"SWAP_READY");
    struct stat targetstat; require(!stat(target,&targetstat) && targetstat.st_dev==s->source.st_dev && targetstat.st_ino==s->source.st_ino,"bind_target_source_inode");
}
static void cleanup_fixture(struct sample *s) {
    kill(s->sup,SIGSTOP); pid_t pid;
    while ((pid=child_inode(s->sup,&s->old))) { kill(pid,SIGKILL); usleep(20000); }
    while ((pid=child_inode(s->sup,&s->source))) { kill(pid,SIGKILL); usleep(20000); }
    kill(s->sup,SIGKILL); waitpid(s->sup,NULL,0);
    if (proc_matches(s->guardian,s->guardstart,s->guard)) kill(s->guardian,SIGKILL);
    waitpid(s->guardian,NULL,0);
}
static void successful_restore(struct sample *s) {
    require(!kill(s->lease,SIGTERM) && !wait_code(s->lease),"successful_lease_rollback");
    struct stat restored; require(!stat(target,&restored) && restored.st_dev==s->old.st_dev && restored.st_ino==s->old.st_ino && equal_files(target,"/fixtures/original"),"original_bytes_and_inode");
    require(access(s->data,F_OK)<0 && errno==ENOENT,"private_copy_removed");
    require(contains(s->log,"SWAP_COMPLETE source_removed=1") && contains(s->log,"CLOCK_PROOF_OK"),"success_log");
    wait_child(s->sup,&s->old); require(state(s->sup)!='T',"original_supervisor_running");
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0); umask(077);
    char boot[4096]; require(read_complete("/proc/cmdline",boot,sizeof(boot))>0 && strstr(boot,"qdc.sms.swap.lab=1"),"lab_boot_flag");
    mkdir("/data/smsweb",0700);
    write_text("/data/smsweb/start.sh","#!/bin/sh\nwhile :; do /data/smsweb/smsweb; sleep 1; done\n",0700);
    struct sample s;
    prepare(&s,1,0); start_lease(&s); require(umount(target)<0 && errno==EBUSY,"lease_mount_id_pinned_by_fd"); puts("SMS_SWAP_VM_MOUNT_PINNED ordinary_external_umount=EBUSY"); successful_restore(&s); char proof[160]; path(proof,sizeof(proof),s.clock,"sms-guard.log"); require(contains(proof,"RESUME_DELEGATED verify_supervisor_before_cont=1") && !contains(proof,"RESUME_SENT"),"delegated_clock_restore_required"); cleanup_fixture(&s); puts("SMS_SWAP_VM_CASE early_clock_handoff PASS");
    prepare(&s,2,0); start_lease(&s); require(!kill(s.guardian,SIGTERM) && !wait_code(s.guardian),"fixture_clock_restore"); pid_t optimized=wait_child(s.sup,&s.source); successful_restore(&s); require(!state(optimized) || state(optimized)=='Z',"new_child_terminated"); cleanup_fixture(&s); puts("SMS_SWAP_VM_CASE running_new_to_original PASS");
    prepare(&s,3,1); start_lease(&s); require(!kill(s.lease,SIGTERM) && wait_code(s.lease)!=0,"clock_failure_refused"); require(state(s.sup)=='T' && equal_files(target,"/fixtures/original") && !contains(s.log,"SUP_RESUMED"),"clock_failure_no_helper_cont"); cleanup_fixture(&s); puts("SMS_SWAP_VM_CASE clock_proof_refusal PASS");
    prepare(&s,4,0); char source[160]; path(source,sizeof(source),s.data,"smsweb"); require(!unlink(source) && !symlink("/fixtures/optimized",source),"fixture_symlink"); require(command(&s,"check",NULL,NULL)!=0 && equal_files(target,"/fixtures/original"),"source_symlink_refused"); cleanup_fixture(&s); puts("SMS_SWAP_VM_CASE symlink_refusal PASS");
    prepare(&s,5,2); start_lease(&s); require(!kill(s.lease,SIGTERM) && wait_code(s.lease)!=0,"clock_window_target_changed"); require(state(s.sup)=='T' && equal_files(target,"/fixtures/foreign") && !contains(s.log,"SUP_RESUMED") && access(s.data,F_OK)==0,"clock_window_unknown_target_retained"); require(!umount(target) && equal_files(target,"/fixtures/original"),"fixture_owned_overlay_cleanup"); cleanup_fixture(&s); puts("SMS_SWAP_VM_CASE clock_window_target_refusal PASS");
    prepare(&s,6,0); start_lease(&s); require(!mount("/fixtures/foreign",target,NULL,MS_BIND,NULL),"fixture_foreign_overlay"); kill(s.lease,SIGTERM); require(wait_code(s.lease)!=0 && state(s.sup)=='T' && equal_files(target,"/fixtures/foreign") && access(s.data,F_OK)==0 && contains(s.log,"RECOVERY_REQUIRED"),"unknown_overlay_retained"); path(proof,sizeof(proof),s.clock,"sms-guard.log"); require(contains(proof,"RESUME_DELEGATED verify_supervisor_before_cont=1") && !contains(proof,"RESUME_SENT") && !wait_code(s.guardian),"unknown_overlay_retires_guardian_without_cont"); cleanup_fixture(&s); puts("SMS_SWAP_VM_CASE unknown_overlay_retained PASS");
    require(contains("/data/smsweb/start.sh","while :; do /data/smsweb/smsweb; sleep 1; done"),"startup_script_unchanged");
    puts("SMS_SWAP_VM_PASS cases=6 original_bytes_not_overwritten=1 clock_modified=0 device_executed=0"); return 0;
}
