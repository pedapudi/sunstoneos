#include "request.h"
#include "a733_storage_digest.h"
#include <string.h>
void crypto_secret_clear(void *memory,size_t bytes)
{volatile uint8_t *p=memory;for(size_t i=0;i<bytes;++i)p[i]=0;}
void crypto_key_clear(uint8_t key[16]){crypto_secret_clear(key,16);}
void crypto_request_clear(struct crypto_request *r)
{crypto_key_clear(r->key);crypto_secret_clear(r->secret,sizeof(r->secret));}
static int zero(const void *memory,size_t bytes)
{const uint8_t *p=memory;unsigned any=0;for(size_t i=0;i<bytes;++i)any|=p[i];return !any;}
unsigned crypto_request_output(const struct crypto_request *r)
{
 if(r->operation==CRYPTO_SHA256)return 32;
 if(r->operation==CRYPTO_HASH||r->operation==CRYPTO_HMAC){
  static const unsigned sizes[6]={0,20,28,32,48,64};return r->method<=5?sizes[r->method]:0;
 }
 if(r->version==2&&r->mode==3)return r->operation==CRYPTO_CIPHER_DECRYPT?r->bytes-r->tag_bytes:r->bytes+r->tag_bytes;
 return r->bytes;
}
int crypto_request_copy(struct crypto_request *out,const struct crypto_session *session,const void *input,size_t bytes)
{
 if(!out||!session||!crypto_session_runtime(session)||!input||bytes<CRYPTO_REQUEST_HEADER||bytes>sizeof(*out))return 0;
 /* Authenticate the private copy, including all fixed-slot padding. */
 memset(out,0,sizeof(*out));memcpy(out,input,bytes);
 uint8_t digest[32];storage_digest_bytes(out,bytes,digest);
 if(memcmp(digest,session->fixture,32)||out->magic!=CRYPTO_REQUEST_MAGIC||!out->id||out->reserved||out->bytes>CRYPTO_INPUT_MAX)goto fail;
 if(out->version==1){
  if(bytes!=CRYPTO_REQUEST_HEADER+out->bytes)goto fail;
  if(out->operation==CRYPTO_SHA256){if(!zero(out->key,16))goto fail;}
  else if((out->operation!=CRYPTO_AES_ENCRYPT&&out->operation!=CRYPTO_AES_DECRYPT)||!out->bytes||out->bytes%16)goto fail;
  return 1;
 }
 if(out->version!=2||bytes!=sizeof(*out)||!zero(out->key,16)||out->key_bytes>128||out->iv_bytes>64||out->aad_bytes>512||
    !zero(out->input+out->bytes,sizeof(out->input)-out->bytes)||
    !zero(out->secret+out->key_bytes,sizeof(out->secret)-out->key_bytes)||
    !zero(out->iv+out->iv_bytes,sizeof(out->iv)-out->iv_bytes)||
    !zero(out->aad+out->aad_bytes,sizeof(out->aad)-out->aad_bytes))goto fail;
 if(out->operation==CRYPTO_HASH||out->operation==CRYPTO_HMAC){
  if(out->method<1||out->method>5||out->mode||out->iv_bytes||out->aad_bytes||out->tag_bytes||
     (out->operation==CRYPTO_HASH&&out->key_bytes))goto fail;
 }else{
  if((out->operation!=CRYPTO_CIPHER_ENCRYPT&&out->operation!=CRYPTO_CIPHER_DECRYPT)||out->method||out->mode>3||
     (out->key_bytes!=16&&out->key_bytes!=24&&out->key_bytes!=32))goto fail;
  if(out->mode==3){
   if(!out->iv_bytes||out->tag_bytes<12||out->tag_bytes>16)goto fail;
   if(out->operation==CRYPTO_CIPHER_ENCRYPT){if(out->bytes>2032)goto fail;}
   else if(out->bytes<out->tag_bytes||out->bytes-out->tag_bytes>2032)goto fail;
  }else{
   if(out->aad_bytes||out->tag_bytes||out->iv_bytes!=(out->mode?16u:0u)||!out->bytes||
      (out->mode!=2&&out->bytes%16))goto fail;
  }
 }
 return 1;
fail:
 crypto_request_clear(out);return 0;
}
