#include "http_metadata.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const char good[]="HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    assert(sms_http_status(good,strlen(good))==200);
    assert(sms_http_status(good,strlen(good)-1)==-1);
    const char binary[]="HTTP/1.1 202 Accepted\r\ncontent-length: 3\r\n\r\na\0b";
    assert(sms_http_status(binary,sizeof binary-1)==202);
    const char *bad[]={"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nabc",
                       "HTTP/1.0 200 OK\r\n\r\nabc",
                       "HTTP/1.0 200 OK\r\nContent-Length: 3x\r\n\r\nabc",
                       "HTTP/1.0 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc",
                       "HTTP/1.0 200 OK\r\nContent-Length: 999999999999999999999999\r\n\r\na"};
    for(size_t i=0;i<sizeof bad/sizeof bad[0];i++)assert(sms_http_status(bad[i],strlen(bad[i]))==-1);
    puts("HTTP_METADATA_TEST_PASS");return 0;
}
