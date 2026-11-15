/* Disposable VM only. No serial-device, USB, WAN or real SIM access. */
#define _GNU_SOURCE
#include "fake_modem.h"
#include "http_metadata.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static pid_t app_pid=-1, modem_pid=-1;
static char response[1024*1024], cookie[512];
static struct sms_fake *fake;
static const char *scenario;
static unsigned peak_rss;

static void cleanup(void) {
    if (app_pid>0) { kill(app_pid,SIGTERM); waitpid(app_pid,NULL,0); app_pid=-1; }
    if (modem_pid>0) { kill(modem_pid,SIGTERM); waitpid(modem_pid,NULL,0); modem_pid=-1; }
}
static void fail(const char *step) {
    printf("SMS_LAB_FAILED step=%s errno=%d\n",step,errno);
    cleanup(); exit(1);
}
static void require(int ok,const char *step) { if (!ok) fail(step); }
static void pause_ms(unsigned ms) {
    struct timespec ts={ms/1000,(long)(ms%1000)*1000000};
    while (nanosleep(&ts,&ts) && errno==EINTR) {}
}
static int all_write(int fd,const char *p,size_t n) {
    while(n) { ssize_t v=write(fd,p,n); if(v<0 && errno==EINTR)continue;
        if(v<=0)return -1;
        p+=v;n-=(size_t)v; }
    return 0;
}
static void modem_loop(int fd) {
    char command[8192], out[4096], input[512]; size_t used=0;
    for(;;) {
        ssize_t n=read(fd,input,sizeof input);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)_exit(2);
        for(ssize_t i=0;i<n;i++) {
            unsigned char c=(unsigned char)input[i]; size_t len=0;
            if(fake->awaiting_pdu) {
                if(c==0x1a || c==0x1b) { len=sms_fake_control(fake,c,out,sizeof out);used=0; }
            } else if(c=='\r') {
                command[used]=0;
                if(used)len=sms_fake_command(fake,command,out,sizeof out);
                used=0;
            } else if(c!='\n') {
                if(used+1>=sizeof command)_exit(3);
                command[used++]=(char)c;
            }
            if(len && all_write(fd,out,len))_exit(4);
        }
    }
}
static unsigned rss(void) {
    char path[96],line[512];unsigned value=0;
    snprintf(path,sizeof path,"/proc/%d/status",app_pid);
    FILE *f=fopen(path,"r");if(!f)return 0;
    while(fgets(line,sizeof line,f))if(sscanf(line,"VmRSS: %u kB",&value)==1)break;
    fclose(f);if(value>peak_rss)peak_rss=value;return value;
}
static unsigned available(void) {
    char line[256];unsigned value=0;FILE *f=fopen("/proc/meminfo","r");
    if(!f)return 0;
    while(fgets(line,sizeof line,f))if(sscanf(line,"MemAvailable: %u kB",&value)==1)break;
    fclose(f);return value;
}
static void sample(const char *phase) {
    char path[96],line[512],key[64];unsigned value;
    unsigned maps=0,r=0,pss=0,anon=0,private_dirty=0,private_clean=0,shared_dirty=0,shared_clean=0;
    snprintf(path,sizeof path,"/proc/%d/smaps",app_pid);
    FILE *f=fopen(path,"r");require(f!=NULL,"smaps-open");
    printf("SMS_LAB_SMAPS_BEGIN phase=%s\n",phase);
    while(fgets(line,sizeof line,f)) {
        fputs(line,stdout);
        unsigned long a,b;
        if(sscanf(line,"%lx-%lx",&a,&b)==2){maps++;continue;}
        if(sscanf(line,"%63[^:]: %u kB",key,&value)!=2)continue;
        if(!strcmp(key,"Rss"))r+=value;
        else if(!strcmp(key,"Pss"))pss+=value;
        else if(!strcmp(key,"Anonymous"))anon+=value;
        else if(!strcmp(key,"Private_Dirty"))private_dirty+=value;
        else if(!strcmp(key,"Private_Clean"))private_clean+=value;
        else if(!strcmp(key,"Shared_Dirty"))shared_dirty+=value;
        else if(!strcmp(key,"Shared_Clean"))shared_clean+=value;
    }
    fclose(f);printf("SMS_LAB_SMAPS_END phase=%s\n",phase);
    require(maps && r && pss,"smaps-fields");rss();if(r>peak_rss)peak_rss=r;
    printf("SMS_LAB_SAMPLE phase=%s rss_kib=%u pss_kib=%u anonymous_kib=%u file_rss_kib=%u maps=%u mem_available_kib=%u private_dirty_kib=%u private_clean_kib=%u shared_dirty_kib=%u shared_clean_kib=%u peak_observed_rss_kib=%u\n",
        phase,r,pss,anon,r>anon?r-anon:0,maps,available(),private_dirty,private_clean,shared_dirty,shared_clean,peak_rss);
    fflush(stdout);
}
static int http(const char *method,const char *path,const char *body) {
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
    struct timeval timeout={30,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    struct sockaddr_in target={.sin_family=AF_INET,.sin_port=htons(8787)};
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(fd,(struct sockaddr *)&target,sizeof target)) {close(fd);return -1;}
    char request[4096];size_t length=body?strlen(body):0;
    int n=snprintf(request,sizeof request,"%s %s HTTP/1.0\r\nHost: 127.0.0.1:8787\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%s%s%s\r\n%s",method,path,length,*cookie?"Cookie: ":"",cookie,*cookie?"\r\n":"",body?body:"");
    if(n<0 || n>=(int)sizeof request || all_write(fd,request,(size_t)n)){close(fd);return -1;}
    size_t used=0;
    for(;;) {
        ssize_t got=read(fd,response+used,sizeof response-used-1);
        if(got<0 && errno==EINTR)continue;
        if(got<0){close(fd);return -1;}
        if(!got)break;
        used+=(size_t)got;
        if(used+1>=sizeof response){close(fd);return -1;}
    }
    close(fd);response[used]=0;
    int status=sms_http_status(response,used);if(status<0)return -1;
    char *set=strstr(response,"\r\nSet-Cookie: ");
    if(set) {
        set+=14;char *end=strpbrk(set,";\r\n");
        if(end && end>set && (size_t)(end-set)<sizeof cookie) {
            memcpy(cookie,set,(size_t)(end-set));cookie[end-set]=0;
        }
    }
    rss();return status;
}
static void wait_http(void) {
    for(int i=0;i<100;i++) { if(http("GET","/",NULL)==200)return;pause_ms(100); }
    fail("http-startup");
}
static void functional(void) {
    require(http("GET","/api/state",NULL)==401,"auth-required");
    require(http("GET","/api/sim/binding",NULL)==200,"synthetic-binding");
    require(http("POST","/api/auth/setup","{\"password\":\"lab-only-password\",\"confirm\":\"lab-only-password\"}")==200,"synthetic-register");
    require(*cookie,"synthetic-cookie");
    if(!strcmp(scenario,"pin")) {
        require(http("GET","/api/sim",NULL)==200 && strstr(response,"SIM PIN"),"pin-locked");
        require(http("POST","/api/sim/unlock","{\"pin\":\"1234\",\"request_id\":\"lab-pin-0001\"}")==200,"synthetic-pin-unlock");
        require(strstr(response,"READY")!=NULL,"pin-ready");
    }
    for(int i=0;i<80;i++) {
        require(http("GET","/api/state",NULL)==200,"poll-state");
        if(strstr(response,"654321"))break;
        require(i<79,"synthetic-inbound");pause_ms(100);
    }
    require(http("POST","/api/send","{\"request_id\":\"lab-sms-0001\",\"number\":\"+15550000001\",\"text\":\"synthetic lab\"}")==202,"synthetic-send");
    for(int i=0;i<100;i++) {
        require(http("GET","/api/send/status?request_id=lab-sms-0001",NULL)==200,"synthetic-send-status");
        if(strstr(response,"\"submitted\""))break;
        require(i<99,"synthetic-submit-confirm");pause_ms(100);
    }
    require(http("POST","/api/send","{\"request_id\":\"lab-sms-0001\",\"number\":\"+15550000001\",\"text\":\"synthetic lab\"}")==202,"synthetic-send-dedup");
    require(fake->submitted==1,"no-duplicate-submission");
    puts("SMS_LAB_FUNCTIONAL_PASS");
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);signal(SIGPIPE,SIG_IGN);
    require(argc==3 && (!strcmp(argv[2],"ready") || !strcmp(argv[2],"pin")),"arguments");scenario=argv[2];
    char commandline[4096];FILE *cmd=fopen("/proc/cmdline","r");
    require(cmd && fgets(commandline,sizeof commandline,cmd) && strstr(commandline,"qdc.sms.lab=1"),"vm-only-gate");fclose(cmd);
    struct utsname kernel;require(!uname(&kernel),"uname");printf("SMS_LAB_KERNEL release=%s\n",kernel.release);
    require(!strcmp(kernel.release,"3.18.44"),"kernel-version");
    fake=mmap(NULL,sizeof *fake,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    require(fake!=MAP_FAILED,"shared-state");fake->messages=1;fake->pin_locked=!strcmp(scenario,"pin");
    int master=posix_openpt(O_RDWR|O_NOCTTY);require(master>=0,"ptmx-open");
    require(!grantpt(master) && !unlockpt(master),"ptmx-unlock");
    char *slave=ptsname(master);require(slave && !strncmp(slave,"/dev/pts/",9),"pty-slave");
    int slave_fd=open(slave,O_RDWR|O_NOCTTY);require(slave_fd>=0,"pty-open");
    struct termios term;require(!tcgetattr(slave_fd,&term),"pty-termios");cfmakeraw(&term);
    require(!tcsetattr(slave_fd,TCSANOW,&term),"pty-raw");puts("SMS_LAB_PTY_OK");
    modem_pid=fork();require(modem_pid>=0,"fork-modem");
    if(!modem_pid){close(slave_fd);modem_loop(master);_exit(1);}
    require(!mkdir("/run/smsdata",0700),"data-dir");
    app_pid=fork();require(app_pid>=0,"fork-app");
    if(!app_pid) {
        close(master);close(slave_fd);
        int err=open("/run/sms-stderr.log",O_CREAT|O_WRONLY|O_TRUNC,0600);
        if(err<0)_exit(1);
        dup2(err,STDOUT_FILENO);dup2(err,STDERR_FILENO);close(err);
        setenv("GODEBUG","gctrace=1,gcpacertrace=1",1);setenv("GOGC","20",1);setenv("GOMEMLIMIT","12MiB",1);
        execl(argv[1],argv[1],"-at",slave,"-listen","127.0.0.1","-port","8787","-data","/run/smsdata","-poll","3s",(char *)NULL);_exit(1);
    }
    /* Hold a slave descriptor until smsweb has opened its own. Otherwise the
       master can receive EIO in the fork/exec gap and lose the fake modem. */
    close(master);atexit(cleanup);
    wait_http();close(slave_fd);sample("startup");functional();
    pause_ms(10000);sample("poll");
    const char *assets[]={"/","/style.css","/model.js","/app.js"};
    for(unsigned i=0;i<64;i++)require(http("GET",assets[i%4],NULL)==200,"asset-http");
    sample("http");
    unsigned before=available();require(before>=8192,"pressure-admission-8mib");
    size_t pressure_bytes=2*1024*1024;volatile unsigned char *pressure=malloc(pressure_bytes);
    require(pressure!=NULL,"synthetic-pressure");
    for(size_t i=0;i<pressure_bytes;i+=4096)pressure[i]=1;
    unsigned after=available();
    require(after>=6144 && (after>=before || before-after<=3072),"pressure-floor-or-drop");
    require(http("GET","/api/state",NULL)==200,"pressure-http");
    for(int i=0;i<30;i++) {
        pause_ms(100);after=available();
        require(after>=6144 && (after>=before || before-after<=3072),"pressure-floor-or-drop");
    }
    sample("pressure");
    free((void *)pressure);pause_ms(3000);sample("released");
    require(fake->submitted==1 && fake->polls>=3,"fake-counters");
    printf("SMS_LAB_FAKE commands=%u polls=%u submitted=%u cancelled=%u unlocks=%u\n",fake->commands,fake->polls,fake->submitted,fake->cancelled,fake->unlocks);
    FILE *gc=fopen("/run/sms-stderr.log","r");require(gc!=NULL,"gc-metadata");
    char line[2048];puts("SMS_LAB_GC_BEGIN");
    while(fgets(line,sizeof line,gc))if(!strncmp(line,"gc ",3) || !strncmp(line,"pacer: ",7))fputs(line,stdout);
    fclose(gc);puts("SMS_LAB_GC_END");
    cleanup();printf("SMS_LAB_PASS scenario=%s\n",scenario);return 0;
}
