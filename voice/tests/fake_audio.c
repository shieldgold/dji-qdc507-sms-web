#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
struct fake_pcm { unsigned flags, bytes; };
static unsigned period(void) { const char *s=getenv("FAKE_AUDIO_PERIOD"); return s ? (unsigned)atoi(s) : 320; }
void *quec_pcm_open(const char *d,unsigned a,unsigned b,unsigned c,unsigned e,unsigned f) {
 (void)d;(void)b;(void)c;(void)e;(void)f;
 puts("vendor diagnostic that must never enter PCM"); fflush(stdout);
 struct fake_pcm *p=calloc(1,sizeof(*p));p->flags=a;return p;
}
int quec_pcm_close(void *p) {
 struct fake_pcm *f=p;
 if (!(f->flags & 0x10000000) && getenv("FAKE_AUDIO_STRICT_PERIOD")) fprintf(stderr,"FAKE_PLAYBACK_BYTES=%u\n",f->bytes);
 free(p);return 0;
}
int quec_read_pcm(void *p,void *b,unsigned n) {
 (void)p;
 if (getenv("FAKE_AUDIO_STRICT_PERIOD") && n!=period()) { fprintf(stderr,"BAD_CAPTURE_PERIOD=%u\n",n);return -1; }
 memset(b,0x12,n);
 if (!getenv("FAKE_AUDIO_UNPACED")) usleep(n*1000000ULL/16000);
 return 0;
}
int quec_write_pcm(void *p,void *b,unsigned n) {
 struct fake_pcm *f=p;
 if(n%2)return -1;
 if (getenv("FAKE_AUDIO_STRICT_PERIOD")) {
  if(n!=period()) { fprintf(stderr,"BAD_PLAYBACK_PERIOD=%u\n",n);return -1; }
  for(unsigned i=0;i<n;i++)if(((unsigned char*)b)[i]!=(f->bytes+i)%251+1) { fprintf(stderr,"BAD_PLAYBACK_SEQUENCE\n");return -1; }
 }
 f->bytes+=n;return 0;
}
unsigned quec_get_pem_buffer_len(void *p) { (void)p; return period(); }
int quectel_clt_set_mixer_value(const char *a,int b,const char *c) { (void)a;(void)b;(void)c; return 1; }
