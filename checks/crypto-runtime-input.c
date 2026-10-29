/* Application ownership and orchestration only. CE returns are modeled;
 * established hash/cipher driver evidence supplies unchanged arithmetic paths. */
#include "run.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned length,operation,intermediate,calls,fail_at;
static uint8_t expected[CRYPTO_INPUT_MAX],key[16];
static int fail(void){return ++calls==fail_at;}
enum a733_ce_hash_result a733_ce_hash_begin(struct a733_ce_hash *h,uint64_t d,uint64_t *g)
{(void)h;assert(d==100&&operation==CRYPTO_SHA256);*g=9;return fail()?A733_CE_HASH_RETAINED:A733_CE_HASH_READY;}
enum a733_ce_hash_result a733_ce_hash_update(struct a733_ce_hash *h,uint64_t g,const uint8_t *p,size_t n,size_t *a)
{(void)h;assert(g==9&&n==length&&!memcmp(p,expected,n));if(fail())return A733_CE_HASH_RETAINED;*a=n;
 intermediate=n==2048;return intermediate?A733_CE_HASH_PENDING:A733_CE_HASH_READY;}
enum a733_ce_hash_result a733_ce_hash_final(struct a733_ce_hash *h,uint64_t g)
{(void)h;assert(g==9&&!intermediate);return fail()?A733_CE_HASH_RETAINED:A733_CE_HASH_PENDING;}
enum a733_ce_hash_result a733_ce_hash_poll(struct a733_ce_hash *h,uint64_t g,uint8_t *p)
{(void)h;assert(g==9);if(fail())return A733_CE_HASH_RETAINED;
 if(intermediate){intermediate=0;return A733_CE_HASH_READY;}storage_digest_bytes(expected,length,p);return A733_CE_HASH_DIGEST;}
enum a733_ce_cipher_result a733_ce_cipher_begin(struct a733_ce_cipher *c,const uint8_t *k,const uint8_t *p,size_t n,unsigned decrypt,uint64_t d,uint64_t *g)
{(void)c;assert(d==100&&n==length&&decrypt==(operation==CRYPTO_AES_DECRYPT)&&!memcmp(p,expected,n)&&!memcmp(k,key,16));
 *g=10;return fail()?A733_CE_CIPHER_RETAINED:A733_CE_CIPHER_PENDING;}
enum a733_ce_cipher_result a733_ce_cipher_poll(struct a733_ce_cipher *c,uint64_t g,uint8_t *p,size_t n)
{(void)c;assert(g==10&&n==length);if(fail())return A733_CE_CIPHER_RETAINED;memset(p,0x5a,n);return A733_CE_CIPHER_OUTPUT;}
enum a733_ce_hash_result a733_ce_hash_start(struct a733_ce_hash *h,enum a733_ce_hash_method m,const uint8_t *k,size_t n,uint64_t d,uint64_t *g)
{(void)h;(void)m;(void)k;(void)n;(void)d;(void)g;assert(0);return A733_CE_HASH_INVALID;}
enum a733_ce_hash_result a733_ce_hash_poll_digest(struct a733_ce_hash *h,uint64_t g,uint8_t *p,size_t n)
{(void)h;(void)g;(void)p;(void)n;assert(0);return A733_CE_HASH_INVALID;}
enum a733_ce_cipher_result a733_ce_cipher_start(struct a733_ce_cipher *c,const struct a733_ce_cipher_params *p,const uint8_t *b,size_t n,uint64_t d,uint64_t *g)
{(void)c;(void)p;(void)b;(void)n;(void)d;(void)g;assert(0);return A733_CE_CIPHER_INVALID;}
static int eligible(void *p){return *(int *)p;}
static void committed(struct crypto_session *s,struct crypto_request *q,unsigned n)
{s->version=2;storage_digest_bytes(q,48+n,s->fixture);}
int main(void)
{
 for(unsigned op=1;op<=3;++op)for(unsigned size=0;size<3;++size){
  operation=op;length=op==1?(size==0?0:size==1?65:2048):(size==0?16:size==1?64:2048);
  struct crypto_request source={.magic=CRYPTO_REQUEST_MAGIC,.version=1,.operation=op,
    .id=UINT64_C(0x123456789abcdef0),.bytes=length};
  for(unsigned i=0;i<length;++i)source.input[i]=expected[i]=(uint8_t)(i*11);
  for(unsigned i=0;i<16;++i)source.key[i]=key[i]=op==1?0:(uint8_t)(i*7);
  struct crypto_session session={0};committed(&session,&source,length);
  for(unsigned failure=0;failure<7;++failure){
   struct crypto_run r={.runtime=1};calls=intermediate=0;fail_at=failure;
   assert(crypto_request_copy(&r.request,&session,&source,48+length));
   int result=0;for(unsigned n=0;n<20&&!result;++n)result=crypto_run_step(&r,100);
   if(failure&&failure<=calls){assert(result==-1);continue;}
   assert(result==1&&r.stage==8&&r.output_bytes==(op==1?32:length));
   for(unsigned i=0;i<16;++i)assert(!r.request.key[i]);
   uint8_t output[2048],digest[32];atomic_uint state=0;int valid=1;
   assert(crypto_run_publish(&r,output,digest,&state,eligible,&valid)==1&&atomic_load(&state)==1);
   memcpy(r.oracle,digest,32);assert(crypto_run_publish(&r,output,digest,&state,eligible,&valid)==1);
   r.oracle[0]^=1;assert(crypto_run_publish(&r,output,digest,&state,eligible,&valid)==-1&&atomic_load(&state)==2);
   memset(r.oracle,0,32);valid=0;assert(crypto_run_publish(&r,output,digest,&state,eligible,&valid)==-1);
  }
  struct crypto_request copied;assert(crypto_request_copy(&copied,&session,&source,48+length));
  source.input[0]^=1;assert(!memcmp(copied.input,expected,length));source.input[0]^=1;
  assert(!crypto_request_copy(&copied,&session,&source,47));
  assert(!crypto_request_copy(&copied,&session,&source,sizeof(source)+1));
  assert(!crypto_request_copy(&copied,&session,&source,48+length-1));
  for(unsigned field=0;field<4;++field){
   struct crypto_request bad=source;
   if(field==0)bad.reserved=1;if(field==1)bad.id=0;if(field==2)bad.bytes=length+1;if(field==3)bad.operation=4;
   committed(&session,&bad,length);assert(!crypto_request_copy(&copied,&session,&bad,48+length));
  }
 }
 puts("PASS runtime crypto: copied arbitrary SHA/AES bounds, partial hash polling, generation, modeled failures, key staging cleanup, optional oracle and final eligibility");
}
