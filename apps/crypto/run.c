#include "run.h"
#include <string.h>
void crypto_run_clear_clients(struct crypto_run *r)
{
 crypto_secret_clear(r->hash.key,sizeof(r->hash.key));
 crypto_secret_clear(r->hash.buffer,sizeof(r->hash.buffer));
 crypto_secret_clear(r->cipher.key,sizeof(r->cipher.key));
 crypto_secret_clear(r->cipher.hash_subkey,sizeof(r->cipher.hash_subkey));
 crypto_secret_clear(r->cipher.subkey_block,sizeof(r->cipher.subkey_block));
}
void crypto_run_clear_secrets(struct crypto_run *r)
{crypto_request_clear(&r->request);crypto_run_clear_clients(r);}
static enum a733_ce_hash_result hash_poll(struct crypto_run *r)
{
 if(r->request.version==1)return a733_ce_hash_poll(&r->hash,r->generation,r->payload);
 return a733_ce_hash_poll_digest(&r->hash,r->generation,r->payload,crypto_request_output(&r->request));
}
static int runtime_step(struct crypto_run *r,uint64_t deadline)
{
 enum a733_ce_hash_result h;enum a733_ce_cipher_result c;size_t accepted=0;
 const struct crypto_request *q=&r->request;
 switch(r->stage){
 case 0:
  if(q->operation==CRYPTO_SHA256||q->operation==CRYPTO_HASH||q->operation==CRYPTO_HMAC){
   h=q->version==1?a733_ce_hash_begin(&r->hash,deadline,&r->generation):
    a733_ce_hash_start(&r->hash,(enum a733_ce_hash_method)q->method,
     q->operation==CRYPTO_HMAC?q->secret:NULL,q->key_bytes,deadline,&r->generation);
   if(h==A733_CE_HASH_BUSY)return 0;
   crypto_request_clear(&r->request);
   if(h!=A733_CE_HASH_READY&&h!=A733_CE_HASH_PENDING)goto fail;
   r->stage=h==A733_CE_HASH_PENDING?9:q->bytes?1:3;return 0;
  }
  if(q->version==1)c=a733_ce_cipher_begin(&r->cipher,q->key,q->input,q->bytes,
    q->operation==CRYPTO_AES_DECRYPT,deadline,&r->generation);
  else {
   const struct a733_ce_cipher_params params={.key=q->secret,.iv=q->iv_bytes?q->iv:NULL,.aad=q->aad_bytes?q->aad:NULL,
    .key_bytes=q->key_bytes,.iv_bytes=q->iv_bytes,.aad_bytes=q->aad_bytes,.tag_bytes=q->tag_bytes,
    .mode=(enum a733_ce_cipher_mode)q->mode,.decrypt=q->operation==CRYPTO_CIPHER_DECRYPT};
   c=a733_ce_cipher_start(&r->cipher,&params,q->input,q->bytes,deadline,&r->generation);
  }
  if(c==A733_CE_CIPHER_BUSY)return 0;
  crypto_request_clear(&r->request);
  if(c!=A733_CE_CIPHER_PENDING)goto fail;
  r->stage=5;return 0;
 case 9:
  h=hash_poll(r);
  if(h==A733_CE_HASH_PENDING||h==A733_CE_HASH_BUSY)return 0;
  if(h!=A733_CE_HASH_READY)goto fail;
  r->stage=q->bytes?1:3;return 0;
 case 1:
  h=a733_ce_hash_update(&r->hash,r->generation,q->input+r->input_offset,q->bytes-r->input_offset,&accepted);
  if(h==A733_CE_HASH_BUSY)return 0;
  if((h!=A733_CE_HASH_READY&&h!=A733_CE_HASH_PENDING)||!accepted||accepted>q->bytes-r->input_offset)goto fail;
  r->input_offset+=(unsigned)accepted;
  r->stage=h==A733_CE_HASH_PENDING?2:r->input_offset<q->bytes?1:3;return 0;
 case 2:
  h=hash_poll(r);
  if(h==A733_CE_HASH_PENDING||h==A733_CE_HASH_BUSY)return 0;
  if(h!=A733_CE_HASH_READY)goto fail;
  r->stage=r->input_offset<q->bytes?1:3;return 0;
 case 3:
  h=a733_ce_hash_final(&r->hash,r->generation);
  if(h==A733_CE_HASH_BUSY)return 0;
  if(h!=A733_CE_HASH_PENDING)goto fail;
  r->stage=4;return 0;
 case 4:
  h=hash_poll(r);
  if(h==A733_CE_HASH_PENDING||h==A733_CE_HASH_BUSY)return 0;
  if(h!=A733_CE_HASH_DIGEST)goto fail;
  r->output_bytes=crypto_request_output(q);r->stage=8;return 1;
 case 5:
  c=a733_ce_cipher_poll(&r->cipher,r->generation,r->payload,crypto_request_output(q));
  if(c==A733_CE_CIPHER_PENDING||c==A733_CE_CIPHER_BUSY)return 0;
  if(c!=A733_CE_CIPHER_OUTPUT)goto fail;
  r->output_bytes=crypto_request_output(q);r->stage=8;return 1;
 case 8:return 1;
 default:break;
 }
fail:
 crypto_run_clear_secrets(r);return -1;
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
 if(r->stage!=8||(!r->runtime&&!bytes)||bytes>CRYPTO_INPUT_MAX)goto fail;
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
