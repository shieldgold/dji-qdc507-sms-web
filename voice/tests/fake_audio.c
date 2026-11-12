#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
void *quec_pcm_open(const char *d,unsigned a,unsigned b,unsigned c,unsigned e,unsigned f) {
 (void)d;(void)a;(void)b;(void)c;(void)e;(void)f;
 puts("vendor diagnostic that must never enter PCM"); fflush(stdout);
 return malloc(1);
}
int quec_pcm_close(void *p) { free(p); return 0; }
int quec_read_pcm(void *p,void *b,unsigned n) { (void)p; memset(b,0x12,n); if (!getenv("FAKE_AUDIO_UNPACED")) usleep(20000); return 0; }
int quec_write_pcm(void *p,void *b,unsigned n) { (void)p; (void)b; return n%2 ? -1 : 0; }
unsigned quec_get_pem_buffer_len(void *p) { (void)p; return 320; }
int quectel_clt_set_mixer_value(const char *a,int b,const char *c) { (void)a;(void)b;(void)c; return 1; }
