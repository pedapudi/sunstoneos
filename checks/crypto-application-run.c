#include "run.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned calls,fail_at,polls;
static int fail(void){return ++calls==fail_at;}
enum a733_ce_hash_result a733_ce_hash_begin(struct a733_ce_hash *h,uint64_t d,uint64_t *g)
{(void)h;assert(d==100);*g=1;return fail()?A733_CE_HASH_RETAINED:A733_CE_HASH_READY;}
enum a733_ce_hash_result a733_ce_hash_update(struct a733_ce_hash*h,uint64_t g,const uint8_t*p,size_t n,size_t*a)
{(void)h;assert(g==1&&n==3&&!memcmp(p,"abc",3));*a=3;return fail()?A733_CE_HASH_RETAINED:A733_CE_HASH_READY;}
enum a733_ce_hash_result a733_ce_hash_final(struct a733_ce_hash*h,uint64_t g)
{(void)h;assert(g==1);return fail()?A733_CE_HASH_RETAINED:A733_CE_HASH_PENDING;}
enum a733_ce_hash_result a733_ce_hash_poll(struct a733_ce_hash*h,uint64_t g,uint8_t *out)
{(void)h;assert(g==1);if(fail())return A733_CE_HASH_RETAINED;if(!(polls++%2))return A733_CE_HASH_PENDING;memcpy(out,crypto_reference,32);return A733_CE_HASH_DIGEST;}
enum a733_ce_cipher_result a733_ce_cipher_begin(struct a733_ce_cipher*c,const uint8_t*k,const uint8_t*p,size_t n,unsigned dec,uint64_t d,uint64_t*g)
{(void)c;assert(n==64&&d==100&&!memcmp(k,crypto_inputs+3,16)&&!memcmp(p,dec?crypto_reference+32:crypto_inputs+19,64));*g=dec?3:2;return fail()?A733_CE_CIPHER_RETAINED:A733_CE_CIPHER_PENDING;}
enum a733_ce_cipher_result a733_ce_cipher_poll(struct a733_ce_cipher*c,uint64_t g,uint8_t*out,size_t n)
{(void)c;assert((g==2||g==3)&&n==64);if(fail())return A733_CE_CIPHER_RETAINED;if(!(polls++%2))return A733_CE_CIPHER_BUSY;memcpy(out,crypto_reference+(g==2?32:96),64);return A733_CE_CIPHER_OUTPUT;}
enum a733_ce_hash_result a733_ce_hash_start(struct a733_ce_hash *h,enum a733_ce_hash_method m,const uint8_t *k,size_t n,uint64_t d,uint64_t *g)
{(void)h;(void)m;(void)k;(void)n;(void)d;(void)g;assert(0);return A733_CE_HASH_INVALID;}
enum a733_ce_hash_result a733_ce_hash_poll_digest(struct a733_ce_hash *h,uint64_t g,uint8_t *p,size_t n)
{(void)h;(void)g;(void)p;(void)n;assert(0);return A733_CE_HASH_INVALID;}
enum a733_ce_cipher_result a733_ce_cipher_start(struct a733_ce_cipher *c,const struct a733_ce_cipher_params *p,const uint8_t *b,size_t n,uint64_t d,uint64_t *g)
{(void)c;(void)p;(void)b;(void)n;(void)d;(void)g;assert(0);return A733_CE_CIPHER_INVALID;}
static int eligibility(void *v){return *(int *)v;}
int main(void)
{
 /* Application expiry can revoke a request before a service poll observes
  * the failure. Its CPU-only scrub must erase all staging without touching
  * the retained controller, DMA page, binding or generation metadata. */
 struct a733_ce_session retained={0},before=retained;
 struct crypto_run revoked;memset(&revoked,0xa5,sizeof(revoked));
 revoked.hash.engine=revoked.hash.bound_engine=&retained;
 revoked.cipher.engine=revoked.cipher.bound_engine=&retained;
 struct crypto_run expected=revoked;
#define CLEAR(member) memset(expected.member,0,sizeof(expected.member))
 CLEAR(hash.key);CLEAR(hash.buffer);CLEAR(hash.digest);
 CLEAR(cipher.key);CLEAR(cipher.iv);CLEAR(cipher.hash_subkey);
 CLEAR(cipher.subkey_block);CLEAR(cipher.j0);CLEAR(cipher.tag);
 CLEAR(cipher.nonce);CLEAR(cipher.aad);CLEAR(cipher.input);CLEAR(cipher.output);
 CLEAR(request.key);CLEAR(request.secret);CLEAR(request.input);
 CLEAR(request.iv);CLEAR(request.aad);CLEAR(payload);
#undef CLEAR
 struct crypto_request producer_owned=revoked.request;
 crypto_run_clear_clients(&revoked);
 assert(!memcmp(&revoked.request,&producer_owned,sizeof(producer_owned)));
 crypto_run_clear_secrets(&revoked);
 assert(!memcmp(&revoked,&expected,sizeof(revoked)));
 assert(!memcmp(&retained,&before,sizeof(retained)));
 unsigned total=0;
 for(unsigned test=0;test<12;++test){struct crypto_run r={0};calls=polls=0;fail_at=test;int result=0;
 for(unsigned n=0;n<30&&!result;++n)result=crypto_run_step(&r,100);
 if(!test){assert(result==1&&!memcmp(r.payload,crypto_reference,160));total=calls;}
 else if(test<=total)assert(result==-1);
 }
 struct crypto_run r={.stage=8};memcpy(r.payload,crypto_reference,160);r.payload[100]^=1;assert(crypto_run_step(&r,100)==-1);
 uint8_t output[160],digest[32];atomic_uint published=0;int valid=1;
 memcpy(r.payload,crypto_reference,160);assert(crypto_run_publish(&r,output,digest,&published,eligibility,&valid)==1);
 assert(atomic_load_explicit(&published,memory_order_acquire)==1&&!memcmp(output,crypto_reference,160));
 valid=0;atomic_store(&published,0);assert(crypto_run_publish(&r,output,digest,&published,eligibility,&valid)==-1&&atomic_load(&published)==2);
 r.payload[32]^=1;valid=1;assert(crypto_run_publish(&r,output,digest,&published,eligibility,&valid)==-1&&atomic_load(&published)==2);
 puts("Crypto actual orchestration: sequential success, pending/busy, each operation failure and final mismatch passed");
}
