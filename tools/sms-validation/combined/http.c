/* Synthetic account/SMS exercise during the real guarded VPN window. */
#define _GNU_SOURCE
#include "../http_metadata.h"
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
static char response[1024*1024],cookie[512];
static int request(const char *method,const char *path,const char *body){
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
    struct timeval timeout={20,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(8787),.sin_addr={htonl(INADDR_LOOPBACK)}};
    if(connect(fd,(struct sockaddr *)&address,sizeof address)){close(fd);return -1;}
    char wire[4096];int n=snprintf(wire,sizeof wire,"%s %s HTTP/1.0\r\nHost: 127.0.0.1:8787\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%s%s%s\r\n%s",method,path,body?strlen(body):0,*cookie?"Cookie: ":"",cookie,*cookie?"\r\n":"",body?body:"");
    if(n<=0||n>=(int)sizeof wire){close(fd);return -1;}
    size_t sent=0,used=0;while(sent<(size_t)n){ssize_t v=write(fd,wire+sent,(size_t)n-sent);if(v<0&&errno==EINTR)continue;if(v<=0){close(fd);return -1;}sent+=(size_t)v;}
    for(;;){ssize_t v=read(fd,response+used,sizeof response-used-1);if(v<0&&errno==EINTR)continue;if(v<0){close(fd);return -1;}if(!v)break;used+=(size_t)v;if(used+1>=sizeof response){close(fd);return -1;}}
    close(fd);response[used]=0;int status=sms_http_status(response,used);if(status<0)return -1;
    char *p=strstr(response,"\r\nSet-Cookie: ");if(p){p+=14;char *end=strpbrk(p,";\r\n");if(end&&end>p&&(size_t)(end-p)<sizeof cookie){memcpy(cookie,p,(size_t)(end-p));cookie[end-p]=0;}}
    return status;
}
static void check(int ok,const char *step){if(!ok){printf("SMS_COMBO_HTTP_FAILED step=%s\n",step);exit(1);}}
int main(int argc,char **argv){
    signal(SIGPIPE,SIG_IGN);setvbuf(stdout,NULL,_IOLBF,0);
    char gate[4096];FILE *f=fopen("/proc/cmdline","r");
    if(!f||!fgets(gate,sizeof gate,f)||!strstr(gate,"qdc.sms.combo=1"))return 77;
    fclose(f);
    check(argc==2&&(!strcmp(argv[1],"setup")||!strcmp(argv[1],"exercise")),"arguments");
    int ready=0;for(int i=0;i<100;i++){if(request("GET","/",NULL)==200){ready=1;break;}usleep(100000);}check(ready,"startup");
    check(request("GET","/api/state",NULL)==401,"auth-required");
    const char *path=!strcmp(argv[1],"setup")?"/api/auth/setup":"/api/auth/login";
    check(request("POST",path,"{\"password\":\"lab-only-password\",\"confirm\":\"lab-only-password\"}")==200&&*cookie,"synthetic-login");
    for(int i=0;i<100;i++){
        check(request("GET","/api/state",NULL)==200,"state");if(strstr(response,"654321"))break;
        check(i<99,"inbound-pdu");usleep(100000);
    }
    if(!strcmp(argv[1],"setup")){puts("SMS_COMBO_SETUP_PASS");return 0;}
    const char *body="{\"request_id\":\"combo-sms-0001\",\"number\":\"+15550000001\",\"text\":\"synthetic combo lab\"}";
    check(request("POST","/api/send",body)==202,"send");
    for(int i=0;i<100;i++){
        check(request("GET","/api/send/status?request_id=combo-sms-0001",NULL)==200,"send-status");
        if(strstr(response,"\"submitted\""))break;
        check(i<99,"send-confirm");usleep(100000);
    }
    check(request("POST","/api/send",body)==202,"send-dedup");
    const char *assets[]={"/","/style.css","/model.js","/app.js"};
    for(int i=0;i<16;i++)check(request("GET",assets[i%4],NULL)==200,"asset");
    puts("SMS_COMBO_FUNCTIONAL_PASS");return 0;
}
