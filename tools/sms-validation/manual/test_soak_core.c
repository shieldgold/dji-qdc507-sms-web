#include "manual_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef SMS_SOAK_RED
int manual_soak_control_parse(const char *s,const char *r,struct manual_soak_control *c){(void)s;(void)r;(void)c;return -1;}
int manual_esp_changed(const char *a,const char *b){(void)a;(void)b;return 0;}
#endif
static const char valid[]=
 "QDC_SMS_SOAK_V1\nRUN MANUAL01\nBOOT 01234567-89ab-cdef-0123-456789abcdef\nSUP 428 826\n"
 "SMS 3487 10182322\nSOURCE 16 4122 1005284\nBASELINE 9576\nHTTP 192.168.225.1\nSECONDS 3600\n";
int main(void) {
 struct manual_soak_control c;assert(!manual_soak_control_parse(valid,"MANUAL01",&c));
 assert(c.supervisor==428 && c.sms==3487 && c.source_ino==4122 && c.baseline==9576);
 assert(manual_soak_control_parse(valid,"OTHER",&c));
 char text[2048];snprintf(text,sizeof(text),"%sEXTRA\n",valid);assert(manual_soak_control_parse(text,"MANUAL01",&c));
 strcpy(text,valid);char *p=strstr(text,"HTTP ");strcpy(p,"HTTP 8.8.8.8\nSECONDS 3600\n");assert(manual_soak_control_parse(text,"MANUAL01",&c));
 strcpy(text,valid);p=strstr(text,"SUP 428");memcpy(p,"SUP 000",7);assert(manual_soak_control_parse(text,"MANUAL01",&c));
 strcpy(text,valid);p=strstr(text,"SMS 3487");memcpy(p,"SMS 0428",8);assert(manual_soak_control_parse(text,"MANUAL01",&c));
 strcpy(text,valid);p=strstr(text,"BASELINE 9576");memcpy(p,"BASELINE 8191",13);assert(manual_soak_control_parse(text,"MANUAL01",&c));
 const char *before="qdc-test: #1, ESTABLISHED, IKEv2\n  qdc-test: #1, INSTALLED, TUNNEL-in-UDP\n    in  abcdef01, 780 bytes\n    out 12345678, 780 bytes\n";
 const char *after="qdc-test: #1, ESTABLISHED, IKEv2\n  qdc-test: #2, INSTALLED, TUNNEL-in-UDP\n    in  abcdef02, 780 bytes\n    out 12345679, 780 bytes\n";
 assert(manual_esp_changed(before,after)==1);assert(manual_esp_changed(before,before)==0);
 assert(manual_esp_changed(before,"qdc-test: ESTABLISHED\nINSTALLED\n    in abcdef02, 0 bytes\n    out 12345678, 0 bytes\n")==0);
 assert(manual_esp_changed(before,"qdc-test: ESTABLISHED\nINSTALLED\n    in abcdef01, 0 bytes\n    out 12345679, 0 bytes\n")==0);
 assert(manual_esp_changed(before,"qdc-test: ESTABLISHED\nINSTALLED\n    in abcdef020, 0 bytes\n    out 12345679, 0 bytes\n")==0);
 assert(manual_esp_changed(before,"    in abcdef02, 0 bytes\n    out 12345679, 0 bytes\n")==0);
 puts("SMS_SOAK_CORE_TESTS_OK");return 0;
}
