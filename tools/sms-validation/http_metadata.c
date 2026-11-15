#include "http_metadata.h"
#include <string.h>
#include <strings.h>

static const char *find(const char *p,size_t n,const char *needle,size_t count) {
    if(n<count)return NULL;
    for(size_t i=0;i<=n-count;i++)if(!memcmp(p+i,needle,count))return p+i;
    return NULL;
}
int sms_http_status(const char *response,size_t length) {
    if(length<13 || (memcmp(response,"HTTP/1.0 ",9) && memcmp(response,"HTTP/1.1 ",9)) ||
       response[9]<'1' || response[9]>'5' || response[10]<'0' || response[10]>'9' ||
       response[11]<'0' || response[11]>'9' || response[12]!=' ')return -1;
    const char *body=find(response,length,"\r\n\r\n",4);
    if(!body || (size_t)(body-response)>8192)return -1;
    const char *end=find(response,(size_t)(body-response)+2,"\r\n",2);
    if(!end)return -1;
    const char *p=end+2;size_t declared=0;int seen=0;
    while(p<body) {
        end=find(p,(size_t)(body-p)+2,"\r\n",2);if(!end)return -1;
        const char *colon=memchr(p,':',(size_t)(end-p));if(!colon)return -1;
        size_t name=(size_t)(colon-p);
        if(name==17 && !strncasecmp(p,"Transfer-Encoding",17))return -1;
        if(name==14 && !strncasecmp(p,"Content-Length",14)) {
            if(seen++)return -1;
            const char *v=colon+1;
            while(v<end && (*v==' ' || *v=='\t'))v++;
            const char *tail=end;while(tail>v && (tail[-1]==' ' || tail[-1]=='\t'))tail--;
            if(v==tail)return -1;
            for(;v<tail;v++) {
                if(*v<'0' || *v>'9' || declared>(1048576-(size_t)(*v-'0'))/10)return -1;
                declared=declared*10+(size_t)(*v-'0');
            }
        }
        p=end+2;
    }
    if(!seen || length-(size_t)(body+4-response)!=declared)return -1;
    return (response[9]-'0')*100+(response[10]-'0')*10+response[11]-'0';
}
