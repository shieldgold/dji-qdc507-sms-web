/* New, bounded long-test entry point. The short guard and its CLI stay intact;
 * share its tested private child, VICI, network and cleanup primitives. */
#define main qdc_short_guard_main
#include "../../ike-validation/runtime/guard.c"
#undef main
#include "manual_core.h"

#ifndef SMS_SOAK_TEST_CLOCK_RATE
#define SMS_SOAK_TEST_CLOCK_RATE 1
#endif
#if SMS_SOAK_TEST_CLOCK_RATE != 1 && SMS_SOAK_TEST_CLOCK_RATE != 30
#error unsupported isolated-soak-test-clock-rate
#endif

static struct manual_soak_control soak_control;
static int soak_refused(const char *stage){printf("SOAK_REFUSED stage=%s\n",stage);return 65;}
static double soak_sampled;
static const char *soak_root;
static unsigned status_pairs,status_reconnects,status_rekeys;
static double status_began;
static int soak_status(const char *phase,int rc) {
 char temp[256],final[256];snprintf(temp,sizeof(temp),"%s/soak.status.tmp",soak_root);snprintf(final,sizeof(final),"%s/soak.status",soak_root);
 int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
 double elapsed=status_began>0?monotonic()-status_began:0;
 int failed=dprintf(fd,"SOAK_STATUS run=%s phase=%s rc=%d elapsed=%.3f pairs=%u reconnects=%u rekeys=%u available_kib=%ld min_available_kib=%ld sms_peak_kib=%ld charon_peak_kib=%ld\n",soak_control.run,phase,rc,elapsed,status_pairs,status_reconnects,status_rekeys,memory_available(),min_available==LONG_MAX?-1:min_available,sms_peak,daemon_peak)<0 || fsync(fd);
 if(close(fd))failed=1;
 if(failed || rename(temp,final))return -1;
 return 0;
}
static int native_baseline_proof(void) {
 char path[256],text[8192],expected[160];
 snprintf(path,sizeof(path),"/run/qdc-sms-%s/swap.log",soak_control.run);
 struct stat log;if(lstat(path,&log) || !S_ISREG(log.st_mode) || log.st_uid || (log.st_mode&0077) || read_complete(path,text,sizeof(text))<1)return -1;
 char *prep=strstr(text,"MANUAL_PREP_READY clock_delegated=1"),*base=strstr(text,"MANUAL_BASELINE baseline_kib="),*armed=strstr(text,"MANUAL_ARMED clock_before_cont=1");
 if(!prep || !base || !armed || prep>=base || base>=armed || strstr(base+1,"MANUAL_BASELINE baseline_kib="))return -1;
 long available=0;int consumed=0;
 if(sscanf(base,"MANUAL_BASELINE baseline_kib=%ld before_c_start=1%n",&available,&consumed)!=1 || !consumed || base[consumed]!='\n' || available!=soak_control.baseline)return -1;
 char *lease=strstr(text,"SWAP_LEASE run=");char run[49],boot[37];long pid=0,sup=0;unsigned long long start=0,supstart=0;
 consumed=0;
 if(!lease || sscanf(lease,"SWAP_LEASE run=%48s boot=%36s helper_pid=%ld helper_start=%llu supervisor=%ld supervisor_start=%llu seconds=5400 manual=1%n",run,boot,&pid,&start,&sup,&supstart,&consumed)!=6 || !consumed || lease[consumed]!='\n' ||
    strcmp(run,soak_control.run) || strcmp(boot,soak_control.boot) || pid<1 || pid>INT_MAX || sup!=soak_control.supervisor || supstart!=soak_control.sup_start)return -1;
 snprintf(expected,sizeof(expected),"/run/qdc-sms-%s/qdc-sms-swap",soak_control.run);
 return proc_matches((pid_t)pid,start,expected)?0:-1;
}
static int soak_identity(void) {
 char name[80];struct stat exec,installed;
 snprintf(name,sizeof(name),"/proc/%ld/exe",(long)soak_control.sms);
 return qdc_coexist_alive(&coexist) && !stat(name,&exec) && !lstat("/data/smsweb-c/smsweb",&installed) &&
  S_ISREG(installed.st_mode) && !installed.st_uid && (installed.st_mode&0777)==0500 &&
  (uint64_t)exec.st_dev==soak_control.source_dev && (uint64_t)exec.st_ino==soak_control.source_ino &&
  exec.st_size>=0 && (uint64_t)exec.st_size==soak_control.source_bytes &&
  installed.st_dev==exec.st_dev && installed.st_ino==exec.st_ino && installed.st_size==exec.st_size;
}
static int soak_tick(int wait_ms) {
 /* Normal long-run samples are every ten seconds. The shared CLI waits still
  * check memory/identity every tick and may sample more frequently. */
 sampled=monotonic();
 if(tick_wait(wait_ms) || !soak_identity()) {puts("STOP soak_sms_or_memory");return -1;}
 struct stat log;
 if(fstat(STDOUT_FILENO,&log) || log.st_size<0 || log.st_size>956*1024) {puts("STOP soak_log_budget");return -1;}
 double now=monotonic();
 if(now-soak_sampled>=10) {
  soak_sampled=now;
  long available=memory_available();if(available<min_available)min_available=available;
  if(!memory_safe(available,baseline))return -1;
  printf("MEM monotonic=%.3f available_kib=%ld baseline_kib=%ld\n",now,available,baseline);
  if(sample(getpid(),"guard",&(long){0}) || sample((pid_t)daemon_pid,"charon",&daemon_peak) || sample(coexist.sms.pid,"smsweb",&sms_peak))return -1;
  if(soak_status("running",-1))return -1;
 }
 if(daemon_pid>0) {int status;if(reap(&daemon_pid,&status,WNOHANG)>0){puts("STOP soak_charon_exit");return -1;}}
 return 0;
}
static int soak_pump(void *unused) {(void)unused;return soak_tick(0);}
static int soak_wait(double until) {while(monotonic()<until)if(soak_tick(100))return -1;return 0;}
static int soak_traffic(const char *target,const char *vip,unsigned index,double elapsed) {
 struct qdc_http_result http;
 if(!owned_target_route(target) || qdc_icmp_once(vip,target,soak_pump,NULL) || http_probe(soak_pump,&http))return -1;
 printf("SOAK_PAIR index=%u ping=1 http_status=200 elapsed=%.3f monotonic=%.3f\n",index,elapsed,monotonic());return 0;
}
static int soak_connected(const char *root,const char *target,const char *vip) {
 if(spawn(root,0,"--list-sas") || !strstr(cli_text,"ESTABLISHED") || !strstr(cli_text,"INSTALLED") ||
    has_vip(vip,1)!=1 || !owned_target_route(target))return -1;
 return qdc_icmp_once(vip,target,soak_pump,NULL);
}
static int soak_reconnect(const char *root,const char *target,const char *vip,unsigned index) {
 const char *term[]={"--terminate","--ike","qdc-test","--timeout","5",NULL};
 const char *init[]={"--initiate","--child","qdc-test","--timeout","15",NULL};
 if(spawnv(root,0,term,7) || spawn(root,0,"--list-sas") || strstr(cli_text,"ESTABLISHED") || strstr(cli_text,"INSTALLED") ||
    has_vip(vip,0)!=0 || !target_unrouted(target))return -1;
 printf("SOAK_DISCONNECTED index=%u sa_absent=1 vip_absent=1 target_route_absent=1\n",index);
 if(spawnv(root,0,init,17) || soak_connected(root,target,vip))return -1;
 printf("SOAK_RECONNECTED index=%u established=1 installed=1 protected_ping=1\n",index);return 0;
}
static int soak_rekey(const char *root,const char *target,const char *vip) {
 static char before[sizeof(cli_text)];
 const char *rekey[]={"--rekey","--child","qdc-test",NULL};
 if(spawn(root,0,"--list-sas"))return -1;
 if(cli_used>=sizeof(before))return -1;
 memcpy(before,cli_text,cli_used+1);
 if(spawnv(root,0,rekey,8))return -1;
 double until=monotonic()+8;
 while(monotonic()<until) {
  if(soak_wait(monotonic()+.25) || spawn(root,0,"--list-sas"))return -1;
  if(manual_esp_changed(before,cli_text) && has_vip(vip,1)==1 && owned_target_route(target) && !qdc_icmp_once(vip,target,soak_pump,NULL)) {
   puts("SOAK_REKEY_COMPLETE inbound_and_outbound_spi_changed=1 protected_ping=1");return 0;
  }
 }
 puts("STOP soak_rekey_spi_proof");return -1;
}
static int soak_result(const char *root,const char *run,double elapsed,unsigned pairs) {
 char file[256],final[256];
 snprintf(file,sizeof(file),"%s/soak.result.tmp",root);
 snprintf(final,sizeof(final),"%s/%s",root,SMS_SOAK_TEST_CLOCK_RATE==1?"soak.result":"soak.test-result");
 int fd=open(file,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
 struct stat owned;if(fstat(fd,&owned)){close(fd);return -1;}
 int rc=dprintf(fd,"QDC_SMS_SOAK_RESULT_V1\nRUN %s\nSECONDS %.3f\nPAIRS %u\nRECONNECTS 3\nREKEYS 1\nRC 0\n",run,elapsed,pairs)<0 || fsync(fd);
 if(close(fd))rc=1;
 struct stat st;
 if(rc || !lstat(final,&st) || errno!=ENOENT || rename(file,final))return -1;
 fd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
 rc=fd<0?-1:fsync(fd);if(fd>=0)close(fd);
 if(rc && !lstat(final,&st) && st.st_dev==owned.st_dev && st.st_ino==owned.st_ino)unlink(final);
 return rc;
}
static int soak_run(const char *root,const char *token,const char *target,const char *vip) {
 char path[256],text[2048],expected[128];struct stat st;struct rlimit limit;
 if(geteuid() || !trusted_root(root))return 65;
 soak_root=root;
 snprintf(path,sizeof(path),"%s/soak.log",root);
 int log=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(log<0)return 65;
 if(dup2(log,STDOUT_FILENO)<0 || dup2(log,STDERR_FILENO)<0){close(log);return 65;}close(log);
 printf("SOAK_BUILD production=%d clock_rate=%d\n",SMS_SOAK_TEST_CLOCK_RATE==1,SMS_SOAK_TEST_CLOCK_RATE);
 snprintf(path,sizeof(path),"%s/soak.control",root);
 if(!trusted_file(path,0) || read_complete(path,text,sizeof(text))<1 || manual_soak_control_parse(text,root+strlen("/run/qdc-tun-"),&soak_control))return 65;
 snprintf(expected,sizeof(expected),"%s_SOAK",soak_control.run);
 if(strcmp(token,expected))return 65;
 snprintf(expected,sizeof(expected),"%s\n",soak_control.boot);
 if(read_complete("/proc/sys/kernel/random/boot_id",text,sizeof(text))<1 || strcmp(text,expected))return 65;
 coexist.supervisor.pid=soak_control.supervisor;coexist.supervisor.start=soak_control.sup_start;
 coexist.sms.pid=soak_control.sms;coexist.sms.start=soak_control.sms_start;coexist.active=1;strcpy(coexist.http,soak_control.http);
 if(!soak_identity())return soak_refused("sms_identity");
 if(native_baseline_proof())return soak_refused("pre_c_baseline_proof");
 if(!target_unrouted(target) || has_vip(vip,0)!=0 || if_nametoindex("ipsec0") || !ike_ports_free())return soak_refused("network_prestate");
 for(unsigned i=0;i<7;i++) {
  const char *names[]={"charon","swanctl","strongswan.conf","swanctl.conf","client.key","client.pem","ca.pem"};
  snprintf(path,sizeof(path),"%s/%s",root,names[i]);
  if(!trusted_file(path,i<2) || (i==4 && (lstat(path,&st) || (st.st_mode&0077))))return 65;
 }
 if(!lstat("/run/qdc-ike",&st) || errno!=ENOENT || read_complete("/sys/class/misc/tun/dev",text,sizeof(text))<1 || strcmp(text,"10:200\n") ||
    lstat("/dev/net/tun",&st) || !S_ISCHR(st.st_mode) || st.st_uid || major(st.st_rdev)!=10 || minor(st.st_rdev)!=200)return 65;
 baseline=soak_control.baseline;
 if(memory_available()<8192 || !memory_safe(memory_available(),baseline)){puts("PREFLIGHT_MEMORY_REFUSED soak=1");return 87;}
 limit=(struct rlimit){0,0};if(setrlimit(RLIMIT_CORE,&limit))return 65;
 limit=(struct rlimit){64,64};if(setrlimit(RLIMIT_NOFILE,&limit))return 65;
 limit=(struct rlimit){512*1024,512*1024};if(setrlimit(RLIMIT_STACK,&limit))return 65;
 limit=(struct rlimit){64*1024*1024,64*1024*1024};if(setrlimit(RLIMIT_AS,&limit))return 65;
 limit=(struct rlimit){960*1024,960*1024};if(setrlimit(RLIMIT_FSIZE,&limit))return 65;
 signal(SIGTERM,signal_stop);signal(SIGINT,signal_stop);signal(SIGHUP,SIG_IGN);signal(SIGPIPE,SIG_IGN);signal(SIGXFSZ,signal_stop);
 signal(SIGALRM,hard_deadline);alarm(3720);
 kmsg_fd=open("/dev/kmsg",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
 if(kmsg_fd<0 || lseek(kmsg_fd,0,SEEK_END)<0)return 65;
 snprintf(path,sizeof(path),"%s/%s.state",root,token);
 state_fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);struct proc_id self;
 if(state_fd<0 || proc_identity(getpid(),&self))return 65;
 dprintf(state_fd,"guard %ld %llu %s\n",(long)getpid(),self.start,self.exe);fsync(state_fd);
 printf("SOAK_LOGGER_READY %s pid=%ld baseline_kib=%ld\n",token,(long)getpid(),baseline);
 int rc=1,created=0,creds=0;double began=0,elapsed=0;unsigned pairs=0,reconnects=0,rekeys=0;
 if(soak_status("waiting_go",-1))goto done;
 snprintf(path,sizeof(path),"%s/soak.go",root);double go_until=monotonic()+15;
 snprintf(expected,sizeof(expected),"SOAK_GO %s\n",soak_control.run);
 for(;;) {
  if(!lstat(path,&st)) {
   if(!trusted_file(path,0) || read_complete(path,text,sizeof(text))<1 || strcmp(text,expected)){puts("LOGGER_NOT_ARMED");goto done;}
   break;
  }
  if(errno!=ENOENT || monotonic()>=go_until || soak_tick(100)){puts("LOGGER_NOT_ARMED");goto done;}
 }
 puts("SOAK_GO");
 if(create_runtime(token))goto done;
 created=1;
 creds=1;if(credentials(root,1) || setenv("SWANCTL_DIR","/run/qdc-ike/swanctl",1))goto done;
 snprintf(path,sizeof(path),"%s/strongswan.conf",root);
 if(setenv("STRONGSWAN_CONF",path,1) || spawn(root,1,NULL))goto done;
 snprintf(path,sizeof(path),"/run/qdc-ike/charon.vici");double ready_until=monotonic()+8;
 while(lstat(path,&st) && monotonic()<ready_until)if(soak_tick(100))goto done;
 if(lstat(path,&st) || !S_ISSOCK(st.st_mode) || !if_nametoindex("ipsec0"))goto done;
 const char *load[]={"--load-all","--noprompt","--file","/run/qdc-ike/swanctl/swanctl.conf",NULL};
 const char *init[]={"--initiate","--child","qdc-test","--timeout","15",NULL};
 if(spawnv(root,0,load,6) || spawnv(root,0,init,17) || soak_connected(root,target,vip))goto done;
 began=monotonic();soak_sampled=began;status_began=began;
 printf("SOAK_READY seconds=3600 sms_pid=%ld sms_start=%llu real_monotonic=%.3f\n",(long)coexist.sms.pid,coexist.sms.start,began);
 for(;;) {
  elapsed=monotonic()-began;double window=elapsed*SMS_SOAK_TEST_CLOCK_RATE;
  if(window>=3600)break;
  if(soak_tick(100))goto done;
  if(reconnects<3 && window>=(reconnects+1)*60) {if(soak_status("reconnecting",-1) || soak_reconnect(root,target,vip,reconnects+1))goto done;reconnects++;status_reconnects=reconnects;continue;}
  if(reconnects==3 && !rekeys && window>=240) {if(soak_status("rekeying",-1) || soak_rekey(root,target,vip))goto done;rekeys=1;status_rekeys=rekeys;continue;}
  if(window>=pairs*10) {if(soak_traffic(target,vip,pairs+1,elapsed))goto done;pairs++;status_pairs=pairs;}
 }
 elapsed=monotonic()-began;
 if(pairs<300 || reconnects!=3 || rekeys!=1 || !soak_identity() || !owned_target_route(target) || has_vip(vip,1)!=1)goto done;
 printf("SOAK_TRAFFIC_COMPLETE real_seconds=%.3f window_seconds=%.3f ping_pairs=%u http_success=%u reconnects=%u rekeys=%u sms_peak_kib=%ld\n",elapsed,elapsed*SMS_SOAK_TEST_CLOCK_RATE,pairs,pairs,reconnects,rekeys,sms_peak);rc=0;
done:
 {int cli_stop=stop_child(&cli_pid),daemon_stop=stop_child(&daemon_pid);if(cli_stop || daemon_stop)rc=1;}
 if(cli_pid || daemon_pid){puts("RECOVERY_REQUIRED soak_children_not_reaped");return 1;}
 if(drain())rc=1;
 if(output_fd>=0){close(output_fd);output_fd=-1;}
 if(if_nametoindex("ipsec0") || has_vip(vip,0)!=0 || !target_unrouted(target)){puts("RECOVERY_REQUIRED soak_network_retained");rc=1;}
 if(creds && credentials(root,0)){puts("RECOVERY_REQUIRED soak_credentials_retained");rc=1;}
 if(created && cleanup_runtime(token)){puts("RECOVERY_REQUIRED soak_runtime_retained");rc=1;}
 struct qdc_http_result post;
 if(!soak_identity() || http_probe(post_pump,&post) || !soak_identity()){puts("STOP soak_post_sms_health");rc=1;}
 printf("MEMORY_SUMMARY charon_peak_kib=%ld cli_peak_kib=%ld sms_peak_kib=%ld min_available_kib=%ld smaps_read_to_eof=1\n",daemon_peak,cli_peak,sms_peak,min_available);
 if(ferror(stdout) || fflush(stdout) || fsync(STDOUT_FILENO))rc=1;
 if(soak_status("complete",rc))rc=1;
 printf("SOAK_COMPLETE rc=%d children_reaped=1 real_seconds=%.3f clock_rate=%d\n",rc,elapsed,SMS_SOAK_TEST_CLOCK_RATE);
 if(ferror(stdout) || fflush(stdout) || fsync(STDOUT_FILENO))rc=1;
 /* The success proof is the final operation before exit. Scaled lab builds
  * only write soak.test-result, which a production manual lease rejects. */
 if(!rc && soak_result(root,soak_control.run,elapsed,pairs))rc=1;
 alarm(0);if(state_fd>=0)close(state_fd);if(kmsg_fd>=0)close(kmsg_fd);return rc;
}
int main(int argc,char **argv) {
 setvbuf(stdout,NULL,_IONBF,0);umask(077);
 if(argc!=7 || strcmp(argv[1],"soak") || !identifier(argv[3]) || strcmp(argv[4],"3600") || !ipv4(argv[5]) || !ipv4(argv[6]) || !strcmp(argv[5],argv[6]))return 64;
 return soak_run(argv[2],argv[3],argv[5],argv[6]);
}
