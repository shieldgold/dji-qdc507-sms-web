/* A separate migration profile. Reuse the reviewed short helper's private
 * identity/mount/clock implementation in the same translation unit, without
 * adding a long-duration option to the original command or control protocol. */
#include "manual_core.h"
static int manual_profile;
static int selected_control_parse(const char *text,const char *run,struct swap_control *c) {
 return manual_profile ? manual_control_parse(text,run,c):swap_control_parse(text,run,c);
}
#define swap_control_parse selected_control_parse
#define main qdc_short_swap_main
#include "../swap/swap.c"
#undef main
#undef swap_control_parse
#include "../../tun-validation/runtime/log_watch.h"

static int manual_kmsg_fd=-1,manual_log_seen;
static uint64_t manual_log_sequence;
static int manual_kernel_log(struct lease *l) {
 char text[8193];
 for(int i=0;i<16;i++) {
  ssize_t n=read(manual_kmsg_fd,text,sizeof(text)-1);
  if(n<0 && errno==EINTR)continue;
  if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return 0;
  if(n<=0)return -1;
  struct stat log;if(fstat(l->log,&log) || log.st_size<0 || (uint64_t)log.st_size+(uint64_t)n+16>60*1024)return -1;
  text[n]=0;record(l,"KMSG %s",text);
  if(qdc_log_problem(text,&manual_log_sequence,&manual_log_seen) || strstr(text,"Kernel panic") || strstr(text,"oom-killer") || strstr(text,"Killed process"))return -1;
 }
 return 0;
}

static int vpn_processes(struct lease *l) {
 DIR *dir=opendir("/proc");if(!dir)return -1;
 char root[160];snprintf(root,sizeof(root),"/run/%s/",l->clockname);
 struct dirent *entry;int active=0;
 while((entry=readdir(dir))) {
  uint64_t n;if(swap_uint(entry->d_name,&n) || !n || n>INT_MAX)continue;
  char name[64],exe[512];snprintf(name,sizeof(name),"/proc/%u/exe",(unsigned)n);
  ssize_t used=readlink(name,exe,sizeof(exe)-1);if(used<0)continue;exe[used]=0;
  if(strncmp(exe,root,strlen(root)))continue;
  const char *base=exe+strlen(root);
  if(!strcmp(base,"charon") || !strcmp(base,"swanctl") || !strcmp(base,"qdc-ike-soak") ||
     !strcmp(base,"charon (deleted)") || !strcmp(base,"swanctl (deleted)") || !strcmp(base,"qdc-ike-soak (deleted)")){active=1;break;}
 }
 closedir(dir);return active;
}
static int vpn_clean(struct lease *l) {
 struct stat s;
 return vpn_processes(l)==0 && lstat("/run/qdc-ike",&s)<0 && errno==ENOENT &&
   lstat("/sys/class/net/ipsec0",&s)<0 && errno==ENOENT;
}
/* A state record is written by the already-execed VPN guard itself.
 * 0 absent, 1 complete, 2 just-created empty file, -1 malformed. */
static int vpn_state(struct lease *l,struct proc_id *id) {
 char name[96],text[1024],expected[192];
 snprintf(name,sizeof(name),"%s_SOAK.state",l->c.run);
 int fd=secret_file(l->clockdir,name);
 if(fd<0)return errno==ENOENT?0:-1;
 ssize_t n=read(fd,text,sizeof(text)-1);close(fd);
 if(n<0)return -1;
 if(!n)return 2;
 text[n]=0;
 char *end=strchr(text,'\n');if(!end)return -1;end[1]=0;
 long pid=0;unsigned long long start=0;char exe[512];int used=0;
 if(sscanf(text,"guard %ld %llu %511[^\n]\n%n",&pid,&start,exe,&used)!=3 || !used || text[used] || pid<1 || pid>INT_MAX || !start)return -1;
 snprintf(expected,sizeof(expected),"/run/%s/qdc-ike-soak",l->clockname);
 if(strcmp(exe,expected))return -1;
 memset(id,0,sizeof(*id));id->pid=(pid_t)pid;id->start=start;strcpy(id->exe,exe);return 1;
}
static int soak_succeeded(struct lease *l) {
 char text[1024];int fd=secret_file(l->clockdir,"soak.result");
 if(fd<0 || read_fd(fd,text,sizeof(text))<1){if(fd>=0)close(fd);return 0;}close(fd);
 return !manual_soak_proof(text,l->c.run);
}
static int same_mount_namespace(struct lease *l) {
 char path[64],self[128],sup[128];snprintf(path,sizeof(path),"/proc/%ld/ns/mnt",(long)l->c.supervisor);
 ssize_t a=readlink("/proc/self/ns/mnt",self,sizeof(self)),b=readlink(path,sup,sizeof(sup));
 return a>0 && a<(ssize_t)sizeof(self) && a==b && !memcmp(self,sup,(size_t)a);
}
static int retire_vpn(struct lease *l) {
 struct proc_id id;int present=vpn_state(l,&id);
 double initializing_until=monotonic()+1;
 while(present==2 && monotonic()<initializing_until){pause_short();present=vpn_state(l,&id);}
 if(present<0 || present==2)return -1;
 if(present==1) {
  if(proc_matches(id.pid,id.start,id.exe)) {
   if(!root_process(id.pid) || kill(id.pid,SIGTERM))return -1;
   record(l,"MANUAL_VPN_TERM pid=%ld start=%llu",(long)id.pid,id.start);
   double until=monotonic()+12;
   while(proc_matches(id.pid,id.start,id.exe) && monotonic()<until)pause_short();
   if(proc_matches(id.pid,id.start,id.exe))return -1;
  }
 }
 return vpn_clean(l)?0:-1;
}
static int release_clients(struct lease *l) {
 if(!vpn_clean(l))return -1;
 struct stat fd,path;
 if(fstat(l->clockdir,&fd) || fstatat(l->runbase,l->clockname,&path,AT_SYMLINK_NOFOLLOW) ||
    !S_ISDIR(path.st_mode) || path.st_uid || (path.st_mode&0777)!=0700 || fd.st_dev!=path.st_dev || fd.st_ino!=path.st_ino)return -1;
 const char *names[]={"charon","swanctl","tun.ko","strongswan.conf","swanctl.conf","client.key","client.pem","ca.pem","qdc-ike-soak","smsweb-c.gz"};
 const char *suffixes[]={"",".part000",".part001",".new"};
 for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
  for(unsigned j=0;j<sizeof(suffixes)/sizeof(suffixes[0]);j++) {
   if(j==2 && strcmp(names[i],"charon"))continue;
   char name[64];snprintf(name,sizeof(name),"%s%s",names[i],suffixes[j]);
   if(fstatat(l->clockdir,name,&path,AT_SYMLINK_NOFOLLOW)) {if(errno==ENOENT)continue;return -1;}
   if(!regular(&path,1) || unlinkat(l->clockdir,name,0))return -1;
  }
 }
 return fsync(l->clockdir);
}
static int manual_rollback(struct lease *l,const char *reason) {
 record(l,"MANUAL_STOP reason=%s",reason);
 if(retire_vpn(l))return recovery(l,"manual_vpn_identity_or_cleanup");
 if(release_clients(l))return recovery(l,"manual_client_release");
 int rc=rollback(l,reason);
 if(!rc)record(l,"MANUAL_ROLLBACK_COMPLETE source_retained=1 original_startup_unchanged=1");
 return rc;
}
static int current_c_alive(struct lease *l) {
 struct proc_id children[16];unsigned count;
 return supervisor(l,0) && !scan(l,0,children,&count) && count==1 && child_same(l,&children[0])==1;
}
static int committed_request(struct lease *l) {
 struct stat s;
 if(fstatat(l->ram,"commit.manual",&s,AT_SYMLINK_NOFOLLOW))return errno==ENOENT?0:-1;
 char text[1024];int fd=secret_file(l->ram,"commit.manual");
 if(fd<0 || read_fd(fd,text,sizeof(text))<1){if(fd>=0)close(fd);return -1;}close(fd);
 if(manual_commit_parse(text,l->c.run) || !vpn_clean(l) || !current_c_alive(l) || clock_proof(l,0))return -1;
 struct proc_id vpn;
 return vpn_state(l,&vpn)==1 && !proc_matches(vpn.pid,vpn.start,vpn.exe) && soak_succeeded(l)?1:-1;
}
static int arm_requested(struct lease *l) {
 struct stat s;
 if(fstatat(l->ram,"arm.manual",&s,AT_SYMLINK_NOFOLLOW))return errno==ENOENT?0:-1;
 char text[256],expected[256];int fd=secret_file(l->ram,"arm.manual");
 if(fd<0 || read_fd(fd,text,sizeof(text))<1){if(fd>=0)close(fd);return -1;}close(fd);
 snprintf(expected,sizeof(expected),"QDC_SMS_MANUAL_ARM_V1\nRUN %s\n",l->c.run);
 return strcmp(text,expected)?-1:1;
}
static int manual_hold(struct lease *l) {
 long baseline=0;
 if(memory_available()<8192){puts("MANUAL_REFUSED initial_memory_below_8192");return 87;}
 l->log=openat(l->ram,"swap.log",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(l->log<0)return 65;
 manual_kmsg_fd=open("/dev/kmsg",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
 if(manual_kmsg_fd<0 || lseek(manual_kmsg_fd,0,SEEK_END)<0){puts("MANUAL_REFUSED kmsg_unavailable");return 65;}
 struct proc_id self;if(proc_identity(getpid(),&self))return recovery(l,"manual_self_identity");
 record(l,"SWAP_LEASE run=%s boot=%s helper_pid=%ld helper_start=%llu supervisor=%ld supervisor_start=%" PRIu64 " seconds=5400 manual=1",l->c.run,l->c.boot,(long)getpid(),self.start,(long)l->c.supervisor,l->c.sup_start);
 uint64_t id=0;
 if(!supervisor(l,1) || guardian(l)!=1 || target_mount(&id)!=0 || !source_path(l) || !current_target(l,1))return recovery(l,"manual_pre_mount_changed");
 char sourcefd[64];snprintf(sourcefd,sizeof(sourcefd),"/proc/self/fd/%d",l->source);
 if(mount(sourcefd,target,NULL,MS_BIND,NULL))return recovery(l,"manual_bind_failed");
 l->bound=open(target,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
 if(l->bound<0 || target_mount(&l->mount_id)!=1 || !current_target(l,0) || !fd_same(l->bound,l->c.new_dev,l->c.new_ino,l->c.new_size))return recovery(l,"manual_bind_identity");
 record(l,"SWAP_READY mount_id=%" PRIu64 " old_dev=%" PRIu64 " old_ino=%" PRIu64 " new_dev=%" PRIu64 " new_ino=%" PRIu64,l->mount_id,l->c.old_dev,l->c.old_ino,l->c.new_dev,l->c.new_ino);
 record(l,"MANUAL_ORIGINAL_FD pid=%ld fd=%d dev=%" PRIu64 " ino=%" PRIu64 " bytes=%" PRIu64,(long)getpid(),l->original,l->c.old_dev,l->c.old_ino,l->c.old_size);
 if(restore_clock(l))return manual_rollback(l,"manual_preparation_clock_handoff");
 record(l,"MANUAL_PREP_READY clock_delegated=1 preparation_seconds=120");
 double began=monotonic(),sampled_at=began,initializing=-1,unregistered=-1;int committed=0,armed=0;const char *reason="signal";
 for(;;) {
  double now=monotonic();long available=memory_available();
  if(interrupted)break;
  if(manual_due(now-began,committed)){reason="manual_lease_deadline";break;}
  if(available<6144 || (armed && !memory_safe(available,baseline))){record(l,"MANUAL_MEMORY_STOP available_kib=%ld baseline_kib=%ld",available,baseline);reason="manual_memory";break;}
  if(manual_kernel_log(l)){reason="manual_kmsg_problem_or_budget";break;}
  struct stat log,logpath;
  if(fstat(l->log,&log) || fstatat(l->ram,"swap.log",&logpath,AT_SYMLINK_NOFOLLOW) || !regular(&logpath,1) ||
     log.st_dev!=logpath.st_dev || log.st_ino!=logpath.st_ino || log.st_size<0 || log.st_size>60*1024){reason="manual_log_budget_or_identity";break;}
  int alive=guardian(l);
  if(alive<0 || (!alive && clock_proof(l,0))){reason="manual_clock_proof";break;}
  if(!supervisor(l,-1) || !same_mount_namespace(l) || target_mount(&id)!=1 || id!=l->mount_id || !current_target(l,0) || !source_path(l)){reason="manual_identity_changed";break;}
  if(!armed) {
   int request=arm_requested(l);
   if(request<0){reason="manual_arm_refused";break;}
   if(request) {
    baseline=memory_available();
    if(baseline<8192 || !supervisor(l,1) || !same_mount_namespace(l) || clock_proof(l,1) || !current_target(l,0) || !source_path(l) ||
       !fd_same(l->original,l->c.old_dev,l->c.old_ino,l->c.old_size)){reason="manual_arm_identity_clock_or_memory";break;}
    record(l,"MANUAL_BASELINE baseline_kib=%ld before_c_start=1",baseline);
    if(kill(l->c.supervisor,SIGCONT)){reason="manual_arm_continue";break;}
    armed=1;record(l,"MANUAL_ARMED clock_before_cont=1 supervisor=%ld start=%" PRIu64,(long)l->c.supervisor,l->c.sup_start);
   } else if(now-began>=120){reason="manual_preparation_deadline";break;}
  }
  if(!committed) {
   struct proc_id vpn;int present=vpn_state(l,&vpn);
   struct stat vpnlog;
   if(armed && present==0 && !fstatat(l->clockdir,"soak.log",&vpnlog,AT_SYMLINK_NOFOLLOW)) {
    if(!regular(&vpnlog,1)){reason="manual_soak_log_identity";break;}
    if(unregistered<0)unregistered=now;
    else if(now-unregistered>5){reason="manual_soak_missing_state";break;}
   } else unregistered=-1;
   if(present==2){if(initializing<0)initializing=now;else if(now-initializing>1){reason="manual_soak_state_incomplete";break;}}
   else initializing=-1;
   if(present<0){reason="manual_soak_state_invalid";break;}
   if(present==1 && !proc_matches(vpn.pid,vpn.start,vpn.exe) && (!soak_succeeded(l) || !vpn_clean(l))){reason="manual_soak_exit_without_proof";break;}
   int requested=committed_request(l);
   if(requested<0){record(l,"MANUAL_COMMIT_REFUSED valid_soak_clock_c_and_cleanup_required=1");reason="manual_commit_proof";break;}
   if(requested){if(!armed){reason="manual_commit_before_arm";break;}committed=1;record(l,"MANUAL_COMMITTED boot_only=1 no_autostart=1");}
  }
  if(now-sampled_at>=60){record(l,"MANUAL_MEM available_kib=%ld baseline_kib=%ld elapsed=%.3f committed=%d",available,baseline,now-began,committed);sampled_at=now;}
  struct timespec delay={0,250000000};nanosleep(&delay,NULL);
 }
 return manual_rollback(l,reason);
}
int main(int argc,char **argv) {
 if(argc!=3 || (strcmp(argv[1],"manual-check") && strcmp(argv[1],"manual-hold")))return qdc_short_swap_main(argc,argv);
 setvbuf(stdout,NULL,_IONBF,0);umask(077);manual_profile=1;
 signal(SIGTERM,stop_signal);signal(SIGINT,stop_signal);signal(SIGHUP,SIG_IGN);signal(SIGPIPE,SIG_IGN);
 struct lease l;if(setup(&l,argv[2])){puts("MANUAL_REFUSED private_paths_or_self_identity");return 65;}
 strcpy(l.dataname,"smsweb-c");strcpy(l.sourcepath,"/data/smsweb-c/smsweb");l.retain_source=1;
 if(preflight(&l,argv[2]) || !same_mount_namespace(&l)){puts("MANUAL_REFUSED exact_install_supervisor_clock_and_mount_required");return 65;}
 if(!strcmp(argv[1],"manual-check")){puts("MANUAL_CHECK_OK install_retained_on_rollback=1 startup_unchanged=1");return 0;}
 return manual_hold(&l);
}
