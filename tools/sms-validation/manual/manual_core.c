#define _GNU_SOURCE
#include "manual_core.h"
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int manual_control_parse(const char *text,const char *run,struct swap_control *c) {
 const char prefix[]="QDC_SMS_MANUAL_V1\n", suffix[]="SECONDS 5400\n";
 size_t n=strlen(text),head=sizeof(prefix)-1,tail=sizeof(suffix)-1;
 if(n>=1024 || n<head+tail || strncmp(text,prefix,head) || strcmp(text+n-tail,suffix))return -1;
 char normalized[1024];
 int used=snprintf(normalized,sizeof(normalized),"QDC_SMS_SWAP_V1\n%.*sSECONDS 300\n",(int)(n-head-tail),text+head);
 if(used<0 || (size_t)used>=sizeof(normalized))return -1;
 /* Identity syntax remains the same; only this new protocol grants a long
  * manual lease. The existing short parser never accepts its header. */
 return swap_control_parse(normalized,run,c);
}
int manual_due(double elapsed,int committed) {
 return !isfinite(elapsed) || elapsed<0 || (committed!=0 && committed!=1) || (!committed && elapsed>=QDC_SMS_MANUAL_SECONDS);
}
int manual_commit_parse(const char *text,const char *run) {
 if(!swap_run_valid(run))return -1;
 char expected[256];
 int n=snprintf(expected,sizeof(expected),"QDC_SMS_MANUAL_COMMIT_V1\nRUN %s\nC_VERIFIED 1\nVPN_CLEANED 1\n",run);
 return n<0 || (size_t)n>=sizeof(expected) || strcmp(text,expected) ? -1:0;
}
int manual_soak_proof(const char *text,const char *run) {
 char id[49],seconds[64],pairs[32],reconnects[32],rekeys[32],canonical[512];int used=0;
 if(!swap_run_valid(run) || strlen(text)>=sizeof(canonical) ||
    sscanf(text,"QDC_SMS_SOAK_RESULT_V1\nRUN %48[A-Za-z0-9_-]\nSECONDS %63[0-9.]\nPAIRS %31[0-9]\nRECONNECTS %31[0-9]\nREKEYS %31[0-9]\nRC 0\n%n",id,seconds,pairs,reconnects,rekeys,&used)!=5 ||
    !used || text[used] || strcmp(id,run))return -1;
 int n=snprintf(canonical,sizeof(canonical),"QDC_SMS_SOAK_RESULT_V1\nRUN %s\nSECONDS %s\nPAIRS %s\nRECONNECTS %s\nREKEYS %s\nRC 0\n",id,seconds,pairs,reconnects,rekeys);
 if(n<0 || (size_t)n>=sizeof(canonical) || strcmp(text,canonical))return -1;
 if(seconds[0]=='0' || seconds[0]=='.' || seconds[strlen(seconds)-1]=='.')return -1;
 char *end;errno=0;double duration=strtod(seconds,&end);
 uint64_t p,r,k;
 return errno || *end || !isfinite(duration) || duration<QDC_SMS_SOAK_SECONDS || duration>QDC_SMS_SOAK_SECONDS+60 ||
   swap_uint(pairs,&p) || p<300 || swap_uint(reconnects,&r) || r!=3 || swap_uint(rekeys,&k) || k!=1 ? -1:0;
}

int manual_soak_control_parse(const char *text,const char *run,struct manual_soak_control *c) {
 char id[49],boot[37],http[16],q[8][32],canonical[1024];int used=0;
 if(!swap_run_valid(run) || strlen(run)>43 || strlen(text)>=sizeof(canonical) ||
    sscanf(text,"QDC_SMS_SOAK_V1\nRUN %48[A-Za-z0-9_-]\nBOOT %36[0-9a-f-]\nSUP %31[0-9] %31[0-9]\nSMS %31[0-9] %31[0-9]\nSOURCE %31[0-9] %31[0-9] %31[0-9]\nBASELINE %31[0-9]\nHTTP %15[0-9.]\nSECONDS 3600\n%n",id,boot,q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],http,&used)!=11 ||
    !used || text[used] || strcmp(id,run))return -1;
 int n=snprintf(canonical,sizeof(canonical),"QDC_SMS_SOAK_V1\nRUN %s\nBOOT %s\nSUP %s %s\nSMS %s %s\nSOURCE %s %s %s\nBASELINE %s\nHTTP %s\nSECONDS 3600\n",id,boot,q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],http);
 if(n<0 || (size_t)n>=sizeof(canonical) || strcmp(text,canonical) || strlen(boot)!=36)return -1;
 for(int j=0;j<36;j++) {
  if(j==8 || j==13 || j==18 || j==23){if(boot[j]!='-')return -1;}
  else if(!strchr("0123456789abcdef",boot[j]))return -1;
 }
 uint64_t v[8];for(unsigned i=0;i<8;i++)if(swap_uint(q[i],&v[i]))return -1;
 if(!v[0] || v[0]>INT_MAX || !v[1] || !v[2] || v[2]>INT_MAX || v[0]==v[2] || !v[3] || !v[5] || !v[6] || v[6]>QDC_SMS_SWAP_MAX_BYTES ||
    v[7]<8192 || v[7]>(uint64_t)LONG_MAX || (strcmp(http,"192.168.225.1") && strcmp(http,"127.0.0.1")))return -1;
 memset(c,0,sizeof(*c));strcpy(c->run,id);strcpy(c->boot,boot);strcpy(c->http,http);
 c->supervisor=(pid_t)v[0];c->sup_start=v[1];c->sms=(pid_t)v[2];c->sms_start=v[3];
 c->source_dev=v[4];c->source_ino=v[5];c->source_bytes=v[6];c->baseline=(long)v[7];return 0;
}
static int esp_pair(const char *text,const char *old_in,const char *old_out,char found_in[9],char found_out[9]) {
 char pending[9]="";
 for(const char *p=text;*p;) {
  const char *end=strchr(p,'\n');size_t n=end?(size_t)(end-p):strlen(p);
  if(n<512) {
   char line[512],spi[9];int used=0;memcpy(line,p,n);line[n]=0;
   if(strstr(line,"INSTALLED"))pending[0]=0;
   if(sscanf(line," in %8[0123456789abcdefABCDEF],%n",spi,&used)==1 && used>0 && strlen(spi)==8) {
    for(int j=0;j<8;j++){pending[j]=(char)tolower((unsigned char)spi[j]);}
    pending[8]=0;
   } else {
    used=0;
    if(pending[0] && sscanf(line," out %8[0123456789abcdefABCDEF],%n",spi,&used)==1 && used>0 && strlen(spi)==8) {
     for(int j=0;j<8;j++)spi[j]=(char)tolower((unsigned char)spi[j]);
     if(!old_in || (strcmp(pending,old_in) && strcmp(spi,old_out))) {strcpy(found_in,pending);strcpy(found_out,spi);return 1;}
     pending[0]=0;
    }
   }
  }
  if(!end)break;
  p=end+1;
 }
 return 0;
}
int manual_esp_changed(const char *before,const char *after) {
 if(!strstr(before,"ESTABLISHED") || !strstr(before,"INSTALLED") || !strstr(after,"ESTABLISHED") || !strstr(after,"INSTALLED"))return 0;
 char in[9],out[9],new_in[9],new_out[9];
 return esp_pair(before,NULL,NULL,in,out) && esp_pair(after,in,out,new_in,new_out);
}
