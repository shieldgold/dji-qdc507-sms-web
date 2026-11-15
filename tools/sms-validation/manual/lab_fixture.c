/* Computer-only fixture extension; never included in a device bundle. */
#define main qdc_coexist_fixture_main
#include "../../ike-validation/runtime/coexist-vm-fixture.c"
#undef main
#include <inttypes.h>
#include <sys/stat.h>

int main(int argc,char **argv) {
 if(argc!=3 || (strcmp(argv[1],"fileinfo") && strcmp(argv[1],"state") && strcmp(argv[1],"baseline")))
  return qdc_coexist_fixture_main(argc,argv);
 char text[8192];
 if(read_file("/proc/cmdline",text,sizeof(text)) || !strstr(text,"qdc.coexist.lab=1"))return 77;
 if(!strcmp(argv[1],"fileinfo")) {
  struct stat s;if(stat(argv[2],&s) || !S_ISREG(s.st_mode) || s.st_size<1)return 1;
  printf("%" PRIu64 " %" PRIu64 " %" PRIu64 "\n",(uint64_t)s.st_dev,(uint64_t)s.st_ino,(uint64_t)s.st_size);return 0;
 }
 if(!strcmp(argv[1],"state")) {
  char path[80];char *end;long pid=strtol(argv[2],&end,10);
  if(*end || pid<1 || pid>2147483647L)return 1;
  snprintf(path,sizeof(path),"/proc/%ld/stat",pid);
  if(read_file(path,text,sizeof(text)))return 1;
  char *p=strrchr(text,')');if(!p || p[1]!=' ' || !p[2])return 1;
  printf("%c\n",p[2]);return 0;
 }
 if(read_file(argv[2],text,sizeof(text)))return 1;
 char *p=strstr(text,"MANUAL_BASELINE baseline_kib=");long baseline=0;int used=0;
 if(!p || sscanf(p,"MANUAL_BASELINE baseline_kib=%ld before_c_start=1%n",&baseline,&used)!=1 || !used || p[used]!='\n' || baseline<8192)return 1;
 printf("%ld\n",baseline);return 0;
}
