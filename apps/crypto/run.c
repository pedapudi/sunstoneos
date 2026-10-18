#include "run.h"
#include <string.h>
int crypto_run_step(struct crypto_run *r,uint64_t deadline)
{
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
int crypto_run_publish(const struct crypto_run *r,uint8_t output[160],uint8_t digest[32],atomic_uint *result,int (*eligible)(void *),void *cookie)
{
 if(r->stage!=8||memcmp(r->payload,crypto_reference,160))goto fail;
 memcpy(output,r->payload,160);storage_digest_bytes(output,160,digest);
 if(eligible(cookie)==1){atomic_store_explicit(result,1,memory_order_release);return 1;}
fail:
 atomic_store_explicit(result,2,memory_order_release);return -1;
}
