#include "../apps/dsi-scanout/input.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t tick=1;static unsigned clock_calls,expire_commit;
static int clock_now(void *context,uint64_t *out){assert(context==&tick);++clock_calls;*out=expire_commit&&clock_calls==2?1000:tick;return 1;}
static void feed_file(struct dsi_input *p,const struct dsi_session *s,const uint8_t *data,size_t n)
{while(n){size_t k=dsi_input_remaining(p);if(k>17)k=17;if(k>n)k=n;assert(k&&dsi_input_feed(p,s,clock_now,&tick,data,k)>=0);data+=k;n-=k;}}
static void init(struct dsi_input *p,const struct dsi_session *s,void *bank)
{memset(p,0,sizeof(*p));p->bank=(uintptr_t)bank;p->bytes=dsi_frame_bytes(s);p->epoch=s->board.epoch;p->generation=s->board.generation;p->authority=s->authority;p->deadline=1000;atomic_store(&p->allocated,1);}
static uint8_t *read_file(const char *path,size_t *bytes)
{FILE *f=fopen(path,"rb");assert(f&&!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0&&!fseek(f,0,SEEK_SET));uint8_t *p=malloc((size_t)n);assert(p&&fread(p,1,(size_t)n,f)==(size_t)n&&!fclose(f));*bytes=(size_t)n;return p;}
int main(int argc,char **argv)
{
 assert(argc==5);size_t n,bytes[2];uint8_t *raw=read_file(argv[1],&n);assert(n==4096);struct dsi_session s;memcpy(&s,raw,4096);free(raw);
 uint8_t *upload[2]={read_file(argv[2],&bytes[0]),read_file(argv[3],&bytes[1])};uint8_t *request=read_file(argv[4],&n);assert(n==96);
 void *bank;assert(!posix_memalign(&bank,4096,(size_t)dsi_bank_bytes(&s)));memset(bank,0,(size_t)dsi_bank_bytes(&s));struct dsi_input p;
 init(&p,&s,bank);feed_file(&p,&s,upload[0],bytes[0]);feed_file(&p,&s,upload[1],bytes[1]);assert(atomic_load(&p.committed[0])&&atomic_load(&p.committed[1]));
 uint8_t bank_hash[32],after[32];storage_digest_bytes(bank,(size_t)dsi_bank_bytes(&s),bank_hash);
 assert(dsi_input_feed(&p,&s,clock_now,&tick,request,96)==1&&atomic_load(&p.request)==1);
 assert(dsi_input_feed(&p,&s,clock_now,&tick,request,96)==-1);storage_digest_bytes(bank,(size_t)dsi_bank_bytes(&s),after);assert(!memcmp(bank_hash,after,32));
 for(unsigned fault=0;fault<6;++fault){
  init(&p,&s,bank);atomic_store(&p.committed[0],1);atomic_store(&p.committed[1],1);uint8_t h[96];memcpy(h,request,96);
  if(fault==0)h[16]^=1; /* epoch */
  if(fault==1)h[32]^=1; /* authority */
  if(fault==2)h[40]=2; /* sequence */
  if(fault==3)h[12]=2; /* slot */
  if(fault==4)h[88]=1; /* reserved */
  if(fault==5){memcpy(h,upload[0],96);} /* published slot upload */
  assert(dsi_input_feed(&p,&s,clock_now,&tick,h,96)==-1&&!atomic_load(&p.request));
 }
 /* Continue the last fragment from one accepted private producer prefix. */
 init(&p,&s,bank);feed_file(&p,&s,upload[0],bytes[0]-1);struct dsi_input prefix=p;
 uint8_t last=upload[0][bytes[0]-1]^1;assert(dsi_input_feed(&p,&s,clock_now,&tick,&last,1)==-1&&!atomic_load(&p.committed[0]));
 p=prefix;clock_calls=0;expire_commit=1;last=upload[0][bytes[0]-1];assert(dsi_input_feed(&p,&s,clock_now,&tick,&last,1)==-1&&!atomic_load(&p.committed[0]));expire_commit=0;
 puts("PASS actual Python uploads:17-byte fragments, immutable slots, duplicate/epoch/authority/sequence/slot/reserved/digest and fresh commit-deadline refusal; no device callbacks");
 free(upload[0]);free(upload[1]);free(request);free(bank);return 0;
}
