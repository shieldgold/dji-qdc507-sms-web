#include "manual_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef SMS_MANUAL_RED
int manual_control_parse(const char *s,const char *r,struct swap_control *c) {(void)s;(void)r;(void)c;return -1;}
int manual_due(double s,int c) {(void)s;(void)c;return 1;}
int manual_commit_parse(const char *s,const char *r) {(void)s;(void)r;return -1;}
int manual_soak_proof(const char *s,const char *r) {(void)s;(void)r;return -1;}
#endif
static const char valid[]=
 "QDC_SMS_MANUAL_V1\nRUN MANUAL01\nBOOT 01234567-89ab-cdef-0123-456789abcdef\n"
 "SUP 428 826\nORIGINAL 16 898 6226072\nSOURCE 16 4122 1005284\nCLOCK 3154 10166479\nSECONDS 5400\n";
int main(void) {
 struct swap_control c;
 assert(!manual_control_parse(valid,"MANUAL01",&c));
 assert(c.supervisor==428 && c.guardian==3154 && c.new_ino==4122);
 assert(manual_control_parse(valid,"OTHER",&c));
 assert(swap_control_parse(valid,"MANUAL01",&c));
 char text[2048];
 snprintf(text,sizeof(text),"%sEXTRA\n",valid);assert(manual_control_parse(text,"MANUAL01",&c));
 strcpy(text,valid);text[strlen(text)-1]=0;assert(manual_control_parse(text,"MANUAL01",&c));
 strcpy(text,valid);char *p=strstr(text,"SECONDS 5400");strcpy(p,"SECONDS 300\n");assert(manual_control_parse(text,"MANUAL01",&c));
 strcpy(text,valid);p=strstr(text,"SUP 428");memcpy(p,"SUP 000",7);assert(manual_control_parse(text,"MANUAL01",&c));
 assert(!manual_due(5399.999,0));assert(manual_due(5400,0));
 assert(!manual_due(999999,1));assert(manual_due(-1,0));assert(manual_due(NAN,1));assert(manual_due(1,2));
 const char *commit="QDC_SMS_MANUAL_COMMIT_V1\nRUN MANUAL01\nC_VERIFIED 1\nVPN_CLEANED 1\n";
 assert(!manual_commit_parse(commit,"MANUAL01"));assert(manual_commit_parse(commit,"OTHER"));
 snprintf(text,sizeof(text),"%sEXTRA\n",commit);assert(manual_commit_parse(text,"MANUAL01"));
 const char *proof="QDC_SMS_SOAK_RESULT_V1\nRUN MANUAL01\nSECONDS 3600.105\nPAIRS 350\nRECONNECTS 3\nREKEYS 1\nRC 0\n";
 assert(!manual_soak_proof(proof,"MANUAL01"));assert(manual_soak_proof(proof,"OTHER"));
 const char *bad[]={
 "QDC_SMS_SOAK_RESULT_V1\nRUN MANUAL01\nSECONDS 3599.9\nPAIRS 350\nRECONNECTS 3\nREKEYS 1\nRC 0\n",
 "QDC_SMS_SOAK_RESULT_V1\nRUN MANUAL01\nSECONDS nan\nPAIRS 350\nRECONNECTS 3\nREKEYS 1\nRC 0\n",
 "QDC_SMS_SOAK_RESULT_V1\nRUN MANUAL01\nSECONDS 3600.1\nPAIRS 299\nRECONNECTS 3\nREKEYS 1\nRC 0\n",
 "QDC_SMS_SOAK_RESULT_V1\nRUN MANUAL01\nSECONDS 3600.1\nPAIRS 350\nRECONNECTS 2\nREKEYS 1\nRC 0\n",
 "QDC_SMS_SOAK_RESULT_V1\nRUN MANUAL01\nSECONDS 3600.1\nPAIRS 350\nRECONNECTS 3\nREKEYS 0\nRC 0\n",
 "QDC_SMS_SOAK_RESULT_V1\nRUN MANUAL01\nSECONDS 3600.1\nPAIRS 350\nRECONNECTS 3\nREKEYS 1\nRC 1\n",
 "QDC_SMS_SOAK_RESULT_V1\nRUN MANUAL01\nSECONDS 3600.1\nPAIRS -350\nRECONNECTS 3\nREKEYS 1\nRC 0\n",NULL};
 for(int i=0;bad[i];i++)assert(manual_soak_proof(bad[i],"MANUAL01"));
 snprintf(text,sizeof(text),"%sEXTRA\n",proof);assert(manual_soak_proof(text,"MANUAL01"));
 puts("SMS_MANUAL_CORE_TESTS_OK");return 0;
}
