/* Linked ONLY into the disposable ARM fault-test executable, never production. */
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
long __real_memory_available(void);
long __wrap_memory_available(void) {
 int fd=open("/run/manual-injected-memory",O_RDONLY);
 if(fd<0)return __real_memory_available();
 char text[32];ssize_t n=read(fd,text,sizeof(text)-1);close(fd);
 if(n<1)return -1;
 text[n]=0;return strtol(text,NULL,10);
}
