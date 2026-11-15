/* Persistent synthetic PTY across real smsweb supervisor handoff; VM only. */
#define _GNU_SOURCE
#include "../fake_modem.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
static volatile sig_atomic_t snapshot,stopping;
static void signal_handler(int sig){if(sig==SIGUSR1)snapshot=1;else stopping=1;}
static int write_all(int fd,const char *p,size_t n){while(n){ssize_t v=write(fd,p,n);if(v<0&&errno==EINTR)continue;if(v<=0)return -1;p+=v;n-=(size_t)v;}return 0;}
static void report(struct sms_fake *s){
    char line[256];int n=snprintf(line,sizeof line,"SMS_COMBO_FAKE commands=%u polls=%u submitted=%u cancelled=%u unlocks=%u\n",s->commands,s->polls,s->submitted,s->cancelled,s->unlocks);
    fputs(line,stdout);fflush(stdout);
    int fd=open("/run/sms-combo-counts",O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0600);
    if(fd>=0){write_all(fd,line,(size_t)n);close(fd);}
}
int main(void){
    char gate[4096];FILE *f=fopen("/proc/cmdline","r");
    if(!f||!fgets(gate,sizeof gate,f)||!strstr(gate,"qdc.sms.combo=1"))return 77;
    fclose(f);
    struct sigaction action={0};action.sa_handler=signal_handler;sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1,&action,NULL);sigaction(SIGTERM,&action,NULL);signal(SIGPIPE,SIG_IGN);
    int master=posix_openpt(O_RDWR|O_NOCTTY);if(master<0||grantpt(master)||unlockpt(master))return 1;
    char *name=ptsname(master);if(!name||strncmp(name,"/dev/pts/",9))return 1;
    int slave=open(name,O_RDWR|O_NOCTTY);if(slave<0)return 1;
    struct termios term;if(tcgetattr(slave,&term))return 1;cfmakeraw(&term);if(tcsetattr(slave,TCSANOW,&term))return 1;
    int fd=open("/run/sms-combo-pty",O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);if(fd<0)return 1;
    if(dprintf(fd,"%s\n",name)<0)return 1;
    close(fd);
    struct sms_fake s={.messages=1};char input[512],command[8192],out[4096];size_t used=0;
    puts("SMS_COMBO_MODEM_READY");fflush(stdout);
    while(!stopping){
        if(snapshot){snapshot=0;report(&s);}
        ssize_t got=read(master,input,sizeof input);if(got<0&&errno==EINTR)continue;if(got<=0)return 2;
        for(ssize_t i=0;i<got;i++){
            unsigned char c=(unsigned char)input[i];size_t n=0;
            if(s.awaiting_pdu){if(c==0x1a||c==0x1b){n=sms_fake_control(&s,c,out,sizeof out);used=0;}}
            else if(c=='\r'){command[used]=0;if(used)n=sms_fake_command(&s,command,out,sizeof out);used=0;}
            else if(c!='\n'){if(used+1>=sizeof command)return 3;command[used++]=(char)c;}
            if(n&&write_all(master,out,n))return 4;
        }
    }
    report(&s);close(slave);close(master);return 0;
}
