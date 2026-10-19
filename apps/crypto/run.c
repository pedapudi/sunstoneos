#include "run.h"
#include <string.h>
static int runtime_step(struct crypto_run *r,uint64_t deadline)
{
 enum a733_ce_hash_result h;enum a733_ce_cipher_result c;size_t accepted=0;
 switch(r->stage){
 case 0:
  if(r->request.operation==CRYPTO_SHA256){
   h=a733_ce_hash_begin(&r->hash,deadline,&r->generation);
   if(h==A733_CE_HASH_BUSY)return 0;
   if(h!=A733_CE_HASH_READY)goto fail;
   r->stage=r->request.bytes?1:3;return 0;
  }
  c=a733_ce_cipher_begin(&r->cipher,r->request.key,r->request.input,r->request.bytes,
   r->request.operation==CRYPTO_AES_DECRYPT,deadline,&r->generation);
  if(c==A733_CE_CIPHER_BUSY)return 0;
  crypto_key_clear(r->request.key); /* Driver copied key, or rejected the request. */
  if(c!=A733_CE_CIPHER_PENDING)goto fail;
  r->stage=5;return 0;
 case 1:
  h=a733_ce_hash_update(&r->hash,r->generation,r->request.input,r->request.bytes,&accepted);
  if(h==A733_CE_HASH_BUSY)return 0;
  if((h!=A733_CE_HASH_READY&&h!=A733_CE_HASH_PENDING)||accepted!=r->request.bytes)goto fail;
  r->stage=h==A733_CE_HASH_PENDING?2:3;return 0;
 case 2:
  h=a733_ce_hash_poll(&r->hash,r->generation,r->payload);
  if(h==A733_CE_HASH_PENDING||h==A733_CE_HASH_BUSY)return 0;
  if(h!=A733_CE_HASH_READY)goto fail;
  r->stage=3;return 0;
 case 3:
  h=a733_ce_hash_final(&r->hash,r->generation);
  if(h==A733_CE_HASH_BUSY)return 0;
  if(h!=A733_CE_HASH_PENDING)goto fail;
  r->stage=4;return 0;
 case 4:
  h=a733_ce_hash_poll(&r->hash,r->generation,r->payload);
  if(h==A733_CE_HASH_PENDING||h==A733_CE_HASH_BUSY)return 0;
  if(h!=A733_CE_HASH_DIGEST)goto fail;
  r->output_bytes=32;r->stage=8;return 1;
 case 5:
  c=a733_ce_cipher_poll(&r->cipher,r->generation,r->payload,r->request.bytes);
  if(c==A733_CE_CIPHER_PENDING||c==A733_CE_CIPHER_BUSY)return 0;
  if(c!=A733_CE_CIPHER_OUTPUT)goto fail;
  r->output_bytes=r->request.bytes;r->stage=8;return 1;
 case 8:return 1;
 default:break;
 }
fail:
 crypto_key_clear(r->request.key);return -1;
}
int crypto_run_step(struct crypto_run *r,uint64_t deadline)
{
 if(r->runtime)return runtime_step(r,deadline);
 size_t accepted=0;
 enum a733_ce_hash_result h;
 enum a733_ce_cipher_result c;
 switch(r->stage){
 case 0:
  h=a733_ce_hash_begin(&r->hash,deadline,&r->generation);
  if(h==A733_CE_HASH_BUSY)return 0;
  if(h!=A733_CE_HASH_READY)return -1;break;
 case 1:
  h=a733_ce_hash_update(&r->hash,r->generation,crypto_inputs,3,&accepted);
  if(h==A733_CE_HASH_BUSY)return 0;
  if(h!=A733_CE_HASH_READY||accepted!=3)return -1;break;
 case 2:
  h=a733_ce_hash_final(&r->hash,r->generation);
  if(h==A733_CE_HASH_BUSY)return 0;
  if(h!=A733_CE_HASH_PENDING)return -1;break;
 case 3:
  h=a733_ce_hash_poll(&r->hash,r->generation,r->payload);
  if(h==A733_CE_HASH_PENDING||h==A733_CE_HASH_BUSY)return 0;
  if(h!=A733_CE_HASH_DIGEST||memcmp(r->payload,crypto_reference,32))return -1;break;
 case 4:case 6:
  c=a733_ce_cipher_begin(&r->cipher,crypto_inputs+3,r->stage==4?crypto_inputs+19:r->payload+32,
   64,r->stage==6,deadline,&r->generation);
  if(c==A733_CE_CIPHER_BUSY)return 0;
  if(c!=A733_CE_CIPHER_PENDING)return -1;break;
 case 5:case 7:
  c=a733_ce_cipher_poll(&r->cipher,r->generation,r->payload+(r->stage==5?32:96),64);
  if(c==A733_CE_CIPHER_PENDING||c==A733_CE_CIPHER_BUSY)return 0;
  if(c!=A733_CE_CIPHER_OUTPUT||memcmp(r->payload+(r->stage==5?32:96),crypto_reference+(r->stage==5?32:96),64))return -1;
  break;
 case 8:return memcmp(r->payload,crypto_reference,160)?-1:1;
 default:return -1;
 }
 ++r->stage;return 0;
}
#include "a733_storage_digest.h"
int crypto_run_publish(const struct crypto_run *r,uint8_t *output,uint8_t digest[32],atomic_uint *result,int (*eligible)(void *),void *cookie)
{
 unsigned bytes=r->runtime?r->output_bytes:160;
 if(r->stage!=8||!bytes||bytes>CRYPTO_INPUT_MAX)goto fail;
 if(r->runtime){if(bytes!=crypto_request_output(&r->request))goto fail;}
 else if(memcmp(r->payload,crypto_reference,160))goto fail;
 memcpy(output,r->payload,bytes);storage_digest_bytes(output,bytes,digest);
 if(r->runtime){
  unsigned oracle=0;for(unsigned i=0;i<32;++i)oracle|=r->oracle[i];
  if(oracle&&memcmp(digest,r->oracle,32))goto fail;
 }
 if(eligible(cookie)==1){atomic_store_explicit(result,1,memory_order_release);return 1;}
fail:
 atomic_store_explicit(result,2,memory_order_release);return -1;
}
