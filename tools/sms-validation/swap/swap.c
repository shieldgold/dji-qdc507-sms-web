/* Narrow identity-bound SMS file lease. No generic commands or device probing. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "swap_core.h"
#include "../../ike-validation/runtime/proc.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static const char target[]="/data/smsweb/smsweb";
static volatile sig_atomic_t interrupted;
struct lease {
    struct swap_control c;
    int runbase, database, ram, data, clockdir, original, source, bound, clocklog, log;
    struct stat ramstat, datastat;
    uint64_t mount_id;
    char ramname[80], dataname[80], clockname[80], rampath[128], sourcepath[160], guardpath[160];
};
static void stop_signal(int sig) { (void)sig; interrupted=1; }
static double monotonic(void) {
    struct timespec t; if (clock_gettime(CLOCK_MONOTONIC,&t)) return -1;
    return (double)t.tv_sec+t.tv_nsec/1e9;
}
static void pause_short(void) { struct timespec t={0,20000000}; nanosleep(&t,NULL); }
static int regular(const struct stat *s, int secret) {
    return S_ISREG(s->st_mode) && !s->st_uid && s->st_nlink==1 && !(s->st_mode&06022) && (!secret || !(s->st_mode&0077));
}
static int same(const struct stat *s, uint64_t dev, uint64_t ino, uint64_t bytes) {
    return regular(s,0) && (uint64_t)s->st_dev==dev && (uint64_t)s->st_ino==ino && s->st_size>=0 && (uint64_t)s->st_size==bytes;
}
static int fd_same(int fd, uint64_t dev, uint64_t ino, uint64_t bytes) {
    struct stat s; return !fstat(fd,&s) && same(&s,dev,ino,bytes);
}
static int root_dir(int parent, const char *name, int private) {
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat s;
    if (fd<0) return -1;
    if (fstat(fd,&s) || !S_ISDIR(s.st_mode) || s.st_uid || (s.st_mode&0022) || (private && (s.st_mode&0777)!=0700)) { close(fd); return -1; }
    return fd;
}
static ssize_t read_fd(int fd, char *text, size_t cap) {
    if (lseek(fd,0,SEEK_SET)<0) return -1;
    size_t used=0;
    for (;;) {
        ssize_t n=read(fd,text+used,cap-1-used);
        if (n<0 && errno==EINTR) continue;
        if (n<0 || (n>0 && memchr(text+used,0,(size_t)n))) return -1;
        if (!n) { text[used]=0; return (ssize_t)used; }
        used+=(size_t)n; if (used==cap-1) return -1;
    }
}
static int secret_file(int dir, const char *name) {
    int fd=openat(dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW); struct stat s;
    if (fd<0) return -1;
    if (fstat(fd,&s) || !regular(&s,1)) { close(fd); return -1; }
    return fd;
}
static void record(struct lease *l, const char *format, ...) {
    if (l->log<0) return;
    va_list args; va_start(args,format); vdprintf(l->log,format,args); va_end(args);
    dprintf(l->log,"\n"); fsync(l->log);
}
static int root_process(pid_t pid) {
    char path[64]; struct stat s; snprintf(path,sizeof(path),"/proc/%ld",(long)pid);
    return !stat(path,&s) && !s.st_uid;
}
static int supervisor(struct lease *l, int stopped) {
    struct proc_id p;
    return root_process(l->c.supervisor) && supervisor_matches(l->c.supervisor,l->c.sup_start) &&
        !proc_identity(l->c.supervisor,&p) && (stopped<0 || ((p.state=='T' || p.state=='t')==stopped));
}
static int stop_supervisor(struct lease *l) {
    if (!supervisor(l,-1) || kill(l->c.supervisor,SIGSTOP)) return -1;
    double until=monotonic()+1;
    while (monotonic()<until) { if (supervisor(l,1)) return 0; pause_short(); }
    return -1;
}
/* stat-only identity also recognizes zombies, whose /proc/PID/exe is absent. */
static int basic_process(pid_t pid, struct proc_id *id) {
    char path[64], text[4096]; snprintf(path,sizeof(path),"/proc/%ld/stat",(long)pid);
    if (read_complete(path,text,sizeof(text))<1) return -1;
    char *end=strrchr(text,')'), *save=NULL; if (!end || end[1]!=' ') return -1;
    memset(id,0,sizeof(*id)); id->pid=pid;
    int field=3;
    for (char *p=strtok_r(end+2," \n",&save); p; p=strtok_r(NULL," \n",&save),field++) {
        uint64_t n;
        if (field==3) id->state=*p;
        else if (field==4) { if (swap_uint(p,&n) || n>INT_MAX) return -1; id->parent=(pid_t)n; }
        /* PID 1/kernel tasks may have startticks=0; scanning them does not
         * grant signal authority. Control-card identities remain nonzero. */
        else if (field==22) { if (swap_uint(p,&n)) return -1; id->start=n; return 0; }
    }
    return -1;
}
static int executable_stat(pid_t pid, struct stat *s) {
    char path[64]; snprintf(path,sizeof(path),"/proc/%ld/exe",(long)pid); return stat(path,s);
}
static double guardian_age(struct lease *l) {
    long hz=sysconf(_SC_CLK_TCK); double now=monotonic();
    return hz>0 && now>=0 ? now-l->c.guard_start/(double)hz : -1;
}
/* 1: exact live guardian; 0: that identity exited; -1: live identity changed. */
static int guardian(struct lease *l) {
    struct proc_id p;
    if (basic_process(l->c.guardian,&p)) return errno==ENOENT || errno==ESRCH ? 0:-1;
    if (p.start!=l->c.guard_start || p.state=='Z') return 0;
    if (!root_process(p.pid) || !proc_matches(p.pid,p.start,l->guardpath) || p.state=='T' || p.state=='t') return -1;
    char path[64], actual[512], expected[512]; snprintf(path,sizeof(path),"/proc/%ld/cmdline",(long)p.pid);
    ssize_t n=read_complete(path,actual,sizeof(actual));
    int need=snprintf(expected,sizeof(expected),"%s%cresume-clock%c%ld%c%" PRIu64 "%c300%c",l->guardpath,0,0,(long)l->c.supervisor,0,l->c.sup_start,0,0);
    return need>0 && (size_t)need<sizeof(expected) && n==need && !memcmp(actual,expected,(size_t)need) ? 1:-1;
}
static int clock_text(struct lease *l, char *text, size_t cap) {
    struct stat fd,path;
    return fstat(l->clocklog,&fd) || fstatat(l->clockdir,"sms-guard.log",&path,AT_SYMLINK_NOFOLLOW) ||
        !regular(&path,1) || fd.st_dev!=path.st_dev || fd.st_ino!=path.st_ino || read_fd(l->clocklog,text,cap)<1 ? -1:0;
}
static int clock_proof(struct lease *l, int delegated_only) {
    char text[4096]; return clock_text(l,text,sizeof(text)) || swap_clock_proof(text,&l->c,delegated_only) ? -1:0;
}
static int target_mount(uint64_t *id) {
    FILE *in=fopen("/proc/self/mountinfo","r"); if (!in) return -1;
    char line[4096]; int count=0;
    while (fgets(line,sizeof(line),in)) {
        uint64_t found; int rc=swap_mount_record(line,target,&found);
        if (rc<0) { fclose(in); return -1; }
        if (rc) { *id=found; count++; }
    }
    int failed=ferror(in); fclose(in); return failed || count>1 ? -1:count;
}
static int current_target(struct lease *l, int original) {
    int dir=root_dir(l->database,"smsweb",1); if (dir<0) return 0;
    int fd=openat(dir,"smsweb",O_RDONLY|O_CLOEXEC|O_NOFOLLOW); close(dir);
    if (fd<0) return 0;
    int ok=original ? fd_same(fd,l->c.old_dev,l->c.old_ino,l->c.old_size) : fd_same(fd,l->c.new_dev,l->c.new_ino,l->c.new_size);
    close(fd); return ok;
}
static int source_path(struct lease *l) {
    struct stat dir,file;
    return !fstatat(l->database,l->dataname,&dir,AT_SYMLINK_NOFOLLOW) && S_ISDIR(dir.st_mode) && !dir.st_uid &&
        (dir.st_mode&0777)==0700 && dir.st_dev==l->datastat.st_dev && dir.st_ino==l->datastat.st_ino &&
        !fstatat(l->data,"smsweb",&file,AT_SYMLINK_NOFOLLOW) && same(&file,l->c.new_dev,l->c.new_ino,l->c.new_size) && (file.st_mode&0777)==0500;
}
/* Refuse unknown direct children and source-inode users owned by another parent. */
static int scan(struct lease *l, int preflight, struct proc_id *children, unsigned *count) {
    DIR *dir=opendir("/proc"); if (!dir) return -1; struct dirent *entry; int rc=0; *count=0;
    while ((entry=readdir(dir))) {
        uint64_t n; if (swap_uint(entry->d_name,&n) || !n || n>INT_MAX) continue;
        struct proc_id p; struct stat exe;
        if (basic_process((pid_t)n,&p)) {
            if (errno!=ENOENT && errno!=ESRCH) { printf("SWAP_SCAN_REFUSED pid=%" PRIu64 " errno=%d\n",n,errno); rc=-1; }
            if (rc) break;
            continue;
        }
        if (p.state=='Z') continue;
        if (executable_stat(p.pid,&exe)) { if (p.parent==l->c.supervisor) { rc=-1; break; } continue; }
        int old=(uint64_t)exe.st_dev==l->c.old_dev && (uint64_t)exe.st_ino==l->c.old_ino;
        int fresh=(uint64_t)exe.st_dev==l->c.new_dev && (uint64_t)exe.st_ino==l->c.new_ino;
        if (old || (fresh && p.parent!=l->c.supervisor)) { printf("SWAP_SCAN_REFUSED pid=%ld old=%d source_other_parent=%d\n",(long)p.pid,old,fresh); rc=-1; break; }
        if (p.parent==l->c.supervisor) {
            if (preflight || !fresh || !root_process(p.pid) || *count>=16) { printf("SWAP_SCAN_REFUSED direct_child=%ld source=%d\n",(long)p.pid,fresh); rc=-1; break; }
            children[(*count)++]=p;
        }
    }
    closedir(dir); return rc;
}
static int child_same(struct lease *l, const struct proc_id *child) {
    struct proc_id now; struct stat exe;
    if (basic_process(child->pid,&now)) return errno==ENOENT || errno==ESRCH ? 0:-1;
    if (now.start!=child->start || now.state=='Z') return 0;
    return now.parent==l->c.supervisor && root_process(now.pid) && !executable_stat(now.pid,&exe) &&
        (uint64_t)exe.st_dev==l->c.new_dev && (uint64_t)exe.st_ino==l->c.new_ino ? 1:-1;
}
static int end_children(struct lease *l) {
    struct proc_id child[16]; unsigned count;
    for (int pass=0; pass<3; pass++) {
        if (stop_supervisor(l) || scan(l,0,child,&count)) return -1;
        for (unsigned i=0; i<count; i++) {
            int rc=child_same(l,&child[i]); if (rc<0 || (rc && kill(child[i].pid,SIGTERM))) return -1;
            record(l,"SMS_TERM pid=%ld start=%llu parent=%ld source_inode=1",(long)child[i].pid,child[i].start,(long)l->c.supervisor);
        }
        double until=monotonic()+2;
        for (;;) {
            int alive=0;
            for (unsigned i=0; i<count; i++) { int rc=child_same(l,&child[i]); if (rc<0) return -1; alive+=rc; }
            if (!alive || monotonic()>=until) break;
            pause_short();
        }
        for (unsigned i=0; i<count; i++) {
            int rc=child_same(l,&child[i]); if (rc<0 || (rc && kill(child[i].pid,SIGKILL))) return -1;
        }
        until=monotonic()+1;
        for (;;) {
            int alive=0;
            for (unsigned i=0; i<count; i++) { int rc=child_same(l,&child[i]); if (rc<0) return -1; alive+=rc; }
            if (!alive) break;
            if (monotonic()>=until) return -1;
            pause_short();
        }
        if (supervisor(l,1) && !scan(l,0,child,&count) && !count) return 0;
    }
    return -1;
}
static int restore_clock(struct lease *l);
static int recovery(struct lease *l, const char *reason) {
    (void)stop_supervisor(l);
    /* Retire only the exact guardian through restore-only, even if the mount
     * cannot safely be removed. Never leave its normal TTL armed knowingly. */
    int clock_ok=restore_clock(l)==0, retired=guardian(l)==0;
    int stopped=stop_supervisor(l)==0;
    record(l,"RECOVERY_REQUIRED reason=%s supervisor_stopped=%d clock_proven=%d guardian_retired=%d unknown_resources_retained=1",reason,stopped,clock_ok,retired);
    return 1;
}
static int restore_clock(struct lease *l) {
    int alive=guardian(l); if (alive<0) return -1;
    int requested=alive;
    if (alive) {
        if (guardian(l)!=1 || kill(l->c.guardian,SIGUSR1)) return -1;
        record(l,"CLOCK_RESTORE_REQUEST pid=%ld start=%" PRIu64 " signal=SIGUSR1 restore_only=1",(long)l->c.guardian,l->c.guard_start);
        double until=monotonic()+5;
        while ((alive=guardian(l))==1 && monotonic()<until) pause_short();
        if (alive!=0) return -1;
    }
    return clock_proof(l,requested);
}
static int cleanup_source(struct lease *l) {
    if (!source_path(l)) return -1;
    if (unlinkat(l->data,"smsweb",0) || fsync(l->data) || unlinkat(l->database,l->dataname,AT_REMOVEDIR) || fsync(l->database)) return -1;
    return 0;
}
static int rollback(struct lease *l, const char *reason) {
    record(l,"SWAP_ROLLBACK reason=%s",reason);
    if (stop_supervisor(l) || end_children(l)) return recovery(l,"child_identity_or_exit");
    uint64_t id=0;
    if (target_mount(&id)!=1 || id!=l->mount_id || !current_target(l,0) || !fd_same(l->bound,l->c.new_dev,l->c.new_ino,l->c.new_size) || !source_path(l) ||
        !fd_same(l->original,l->c.old_dev,l->c.old_ino,l->c.old_size) || !supervisor(l,1)) return recovery(l,"mount_or_file_identity");
    /* Hold the mount reference throughout the lease so a detached mount ID
     * cannot be recycled. Release it only after the final ownership checks. */
    if (close(l->bound)) return recovery(l,"owned_mount_fd_close");
    l->bound=-1;
    if (umount(target)) return recovery(l,"ordinary_umount_failed");
    if (target_mount(&id)!=0 || !current_target(l,1) || !fd_same(l->original,l->c.old_dev,l->c.old_ino,l->c.old_size)) return recovery(l,"original_not_restored");
    record(l,"ORIGINAL_RESTORED dev=%" PRIu64 " ino=%" PRIu64 " bytes=%" PRIu64,l->c.old_dev,l->c.old_ino,l->c.old_size);
    if (restore_clock(l)) return recovery(l,"clock_proof");
    /* Clock handoff may wait for another process. Recheck the recovery path
     * immediately before granting our own CONT, not only before that wait. */
    if (target_mount(&id)!=0 || !current_target(l,1) || !fd_same(l->original,l->c.old_dev,l->c.old_ino,l->c.old_size) || !source_path(l)) return recovery(l,"post_clock_original_identity");
    record(l,"CLOCK_PROOF_OK restored_before_resume=1");
    if (!supervisor(l,-1) || kill(l->c.supervisor,SIGCONT)) return recovery(l,"supervisor_resume_identity");
    record(l,"SUP_RESUMED pid=%ld start=%" PRIu64,(long)l->c.supervisor,l->c.sup_start);
    if (cleanup_source(l)) { record(l,"RECOVERY_REQUIRED reason=private_copy_cleanup original_restored=1 clock_proven=1"); return 1; }
    record(l,"SWAP_COMPLETE source_removed=1 verify_original_sms_health_separately=1"); return 0;
}
static int setup(struct lease *l, const char *run) {
    memset(l,0,sizeof(*l));
    l->runbase=l->database=l->ram=l->data=l->clockdir=l->original=l->source=l->bound=l->clocklog=l->log=-1;
    if (geteuid() || !swap_run_valid(run)) return -1;
    snprintf(l->ramname,sizeof(l->ramname),"qdc-sms-%s",run); snprintf(l->dataname,sizeof(l->dataname),"qdc-sms-%s",run);
    snprintf(l->clockname,sizeof(l->clockname),"qdc-tun-%s",run); snprintf(l->rampath,sizeof(l->rampath),"/run/%s",l->ramname);
    snprintf(l->sourcepath,sizeof(l->sourcepath),"/data/%s/smsweb",l->dataname); snprintf(l->guardpath,sizeof(l->guardpath),"/run/%s/qdc-ike-guard",l->clockname);
    l->runbase=root_dir(AT_FDCWD,"/run",0); l->database=root_dir(AT_FDCWD,"/data",0);
    if (l->runbase<0 || l->database<0 || (l->ram=root_dir(l->runbase,l->ramname,1))<0 || fstat(l->ram,&l->ramstat)) return -1;
    struct stat self,actual; int fd=openat(l->ram,"qdc-sms-swap",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0 || fstat(fd,&self) || !regular(&self,1) || stat("/proc/self/exe",&actual) || self.st_dev!=actual.st_dev || self.st_ino!=actual.st_ino) { if (fd>=0) close(fd); return -1; }
    close(fd); return 0;
}
static int refused(const char *stage) { printf("SWAP_REFUSED stage=%s\n",stage); return -1; }
static int preflight(struct lease *l, const char *run) {
    char text[4096], expected[192]; int control=secret_file(l->ram,"lease.control");
    if (control<0 || read_fd(control,text,1024)<1 || swap_control_parse(text,run,&l->c)) { if (control>=0) close(control); return refused("control"); }
    close(control);
    snprintf(expected,sizeof(expected),"%s\n",l->c.boot);
    if (read_complete("/proc/sys/kernel/random/boot_id",text,sizeof(text))<1 || strcmp(text,expected) || !supervisor(l,1)) return refused("boot_or_supervisor");
    int originaldir=root_dir(l->database,"smsweb",1); if (originaldir<0) return refused("original_directory");
    l->original=openat(originaldir,"smsweb",O_RDONLY|O_CLOEXEC|O_NOFOLLOW); close(originaldir);
    l->data=root_dir(l->database,l->dataname,1); l->clockdir=root_dir(l->runbase,l->clockname,1);
    if (l->original<0 || l->data<0 || l->clockdir<0 || fstat(l->data,&l->datastat)) return refused("private_directories");
    l->source=openat(l->data,"smsweb",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    l->clocklog=secret_file(l->clockdir,"sms-guard.log");
    int guardfd=openat(l->clockdir,"qdc-ike-guard",O_RDONLY|O_CLOEXEC|O_NOFOLLOW); struct stat gs;
    if (guardfd<0 || fstat(guardfd,&gs) || !regular(&gs,1)) { if (guardfd>=0) close(guardfd); return refused("guardian_file"); }
    close(guardfd);
    if (l->source<0 || l->clocklog<0 || !fd_same(l->original,l->c.old_dev,l->c.old_ino,l->c.old_size) ||
        !fd_same(l->source,l->c.new_dev,l->c.new_ino,l->c.new_size) || !source_path(l) || guardian(l)!=1) return refused("files_or_guardian");
    double age=guardian_age(l); uint64_t id=0; struct proc_id child[16]; unsigned count;
    if (age<0 || age>180) return refused("guardian_age");
    if (target_mount(&id)!=0) return refused("preexisting_or_invalid_mount");
    if (scan(l,1,child,&count)) return refused("live_original_or_unknown_child");
    if (clock_text(l,text,sizeof(text))) return refused("clock_log_identity");
    snprintf(expected,sizeof(expected),"RESUME_GUARD_READY pid=%ld supervisor=%ld start=%" PRIu64 " seconds=300 restore_clock=1\n",(long)l->c.guardian,(long)l->c.supervisor,l->c.sup_start);
    return strcmp(text,expected) ? refused("clock_ready_log"):0;
}
static int hold(struct lease *l) {
    l->log=openat(l->ram,"swap.log",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if (l->log<0) return 65;
    struct proc_id self; if (proc_identity(getpid(),&self)) return recovery(l,"self_identity");
    record(l,"SWAP_LEASE run=%s boot=%s helper_pid=%ld helper_start=%llu supervisor=%ld supervisor_start=%" PRIu64 " seconds=300 clock_margin=30",l->c.run,l->c.boot,(long)getpid(),self.start,(long)l->c.supervisor,l->c.sup_start);
    /* O_EXCL log plus the no-existing-target-mount check reject a second lease. */
    uint64_t id=0;
    if (!supervisor(l,1) || guardian(l)!=1 || target_mount(&id)!=0 || !source_path(l) || !current_target(l,1)) return recovery(l,"pre_mount_changed");
    char sourcefd[64]; snprintf(sourcefd,sizeof(sourcefd),"/proc/self/fd/%d",l->source);
    if (mount(sourcefd,target,NULL,MS_BIND,NULL)) return recovery(l,"bind_mount_failed");
    l->bound=open(target,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (l->bound<0 || target_mount(&l->mount_id)!=1 || !current_target(l,0) || !fd_same(l->bound,l->c.new_dev,l->c.new_ino,l->c.new_size)) return recovery(l,"bind_mount_identity");
    record(l,"SWAP_READY mount_id=%" PRIu64 " old_dev=%" PRIu64 " old_ino=%" PRIu64 " new_dev=%" PRIu64 " new_ino=%" PRIu64,l->mount_id,l->c.old_dev,l->c.old_ino,l->c.new_dev,l->c.new_ino);
    double began=monotonic(); const char *reason="signal";
    for (;;) {
        double now=monotonic(), age=guardian_age(l);
        if (now<0 || age<0) { reason="monotonic_identity"; break; }
        if (interrupted) break;
        if (swap_lease_due(now-began,age)) { reason="lease_deadline"; break; }
        int alive=guardian(l);
        if (alive<0 || (!alive && clock_proof(l,0))) { reason="clock_guardian_proof"; break; }
        if (!supervisor(l,-1) || target_mount(&id)!=1 || id!=l->mount_id || !current_target(l,0) || !source_path(l)) { reason="lease_identity_changed"; break; }
        pause_short();
    }
    return rollback(l,reason);
}
static int copy_memory_safe(void) {
    long available=memory_available();
    if (available>=6144) return 1;
    printf("COPY_REFUSED memory_floor_kib=6144 measured_available_kib=%ld\n",available); return 0;
}
static int copy_candidate(struct lease *l, uint64_t expected) {
    if (!expected || expected>QDC_SMS_SWAP_MAX_BYTES) return 64;
    struct statvfs fs;
    if (fstatvfs(l->database,&fs) || !fs.f_frsize || (uint64_t)fs.f_bavail>UINT64_MAX/(uint64_t)fs.f_frsize ||
        (uint64_t)fs.f_bavail*(uint64_t)fs.f_frsize<expected+1024U*1024U) {
        puts("COPY_REFUSED storage_requires_full_copy_plus_1MiB=1 existing_resources_unchanged=1"); return 65;
    }
    if (!copy_memory_safe()) return 65;
    if (mkdirat(l->database,l->dataname,0700)) return 65;
    l->data=root_dir(l->database,l->dataname,1);
    if (l->data<0 || fstat(l->data,&l->datastat)) return 65;
    int fd=openat(l->data,"smsweb",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if (fd<0) return 65;
    struct stat owned; if (fstat(fd,&owned)) { close(fd); return 65; }
    char buffer[16384]; uint64_t used=0, advised=0; int rc=1;
    double began=monotonic(); if (began<0) goto out;
    for (;;) {
        double now=monotonic();
        if (interrupted || now<0 || now-began>=90) { puts("COPY_REFUSED monotonic_deadline_seconds=90 interrupted_or_expired=1"); goto out; }
        struct pollfd input={STDIN_FILENO,POLLIN,0};
        int ready=poll(&input,1,100);
        if (ready<0 && errno==EINTR) continue;
        if (ready<0 || (input.revents&(POLLERR|POLLNVAL))) goto out;
        if (!ready || !(input.revents&(POLLIN|POLLHUP))) continue;
        if (interrupted) goto out;
        ssize_t n=read(STDIN_FILENO,buffer,sizeof(buffer));
        if (n<0 && errno==EINTR && !interrupted) continue;
        if (n<0 || interrupted) goto out;
        if (!n) break;
        if ((uint64_t)n>expected-used) goto out;
        ssize_t at=0;
        while (at<n) { ssize_t w=write(fd,buffer+at,(size_t)(n-at)); if (w<0 && errno==EINTR) continue; if (w<=0) goto out; at+=w; }
        used+=(uint64_t)n;
        if (used-advised>=65536) {
            if (!copy_memory_safe() || fdatasync(fd) || posix_fadvise(fd,(off_t)advised,(off_t)(used-advised),POSIX_FADV_DONTNEED) || !copy_memory_safe()) goto out;
            advised=used;
        }
    }
    if (used!=expected || !copy_memory_safe() || fchmod(fd,0500) || fsync(fd) || posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED) || fsync(l->data) || fsync(l->database) || fstat(fd,&owned) || !copy_memory_safe()) goto out;
    printf("COPY_READY source_dev=%" PRIu64 " source_ino=%" PRIu64 " bytes=%" PRIu64 " fsync=1 private_fadvise=1\n",(uint64_t)owned.st_dev,(uint64_t)owned.st_ino,used);
    rc=0;
out:
    close(fd);
    if (rc) {
        struct stat current, directory;
        if (!fstatat(l->database,l->dataname,&directory,AT_SYMLINK_NOFOLLOW) && S_ISDIR(directory.st_mode) && !directory.st_uid &&
            (directory.st_mode&0777)==0700 && directory.st_dev==l->datastat.st_dev && directory.st_ino==l->datastat.st_ino &&
            !fstatat(l->data,"smsweb",&current,AT_SYMLINK_NOFOLLOW) && regular(&current,1) && current.st_dev==owned.st_dev && current.st_ino==owned.st_ino) {
            unlinkat(l->data,"smsweb",0); fsync(l->data); unlinkat(l->database,l->dataname,AT_REMOVEDIR); fsync(l->database);
        } else puts("RECOVERY_REQUIRED private_copy_directory_or_file_identity_changed resources_retained=1");
        puts("COPY_REFUSED partial_private_copy_only=1");
    }
    return rc;
}
int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IONBF,0); umask(077);
    if (argc<3 || argc>4 || !swap_run_valid(argv[2])) return 64;
    signal(SIGTERM,stop_signal); signal(SIGINT,stop_signal); signal(SIGHUP,SIG_IGN); signal(SIGPIPE,SIG_IGN);
    struct lease l;
    if (setup(&l,argv[2])) { puts("SWAP_REFUSED root_private_paths_or_self_identity"); return 65; }
    if (argc==4 && !strcmp(argv[1],"copy")) { uint64_t bytes; return swap_uint(argv[3],&bytes) ? 64:copy_candidate(&l,bytes); }
    if (argc!=3 || (strcmp(argv[1],"check") && strcmp(argv[1],"hold"))) return 64;
    if (preflight(&l,argv[2])) { puts("SWAP_REFUSED original_supervisor_source_clock_or_mount_identity"); return 65; }
    if (!strcmp(argv[1],"check")) { puts("SWAP_CHECK_OK supervisor_stopped=1 original_live_processes=0"); return 0; }
    return hold(&l);
}
