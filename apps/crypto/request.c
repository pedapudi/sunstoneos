#include "request.h"
#include "a733_storage_digest.h"
#include <string.h>
void crypto_key_clear(uint8_t key[16])
{volatile uint8_t *p=key;for(unsigned i=0;i<16;++i)p[i]=0;}
unsigned crypto_request_output(const struct crypto_request *r)
{return r->operation==CRYPTO_SHA256?32:r->bytes;}
int crypto_request_copy(struct crypto_request *out,const struct crypto_session *session,const void *input,size_t bytes)
{
 if(!out||!session||session->version!=2||!input||bytes<CRYPTO_REQUEST_HEADER||bytes>sizeof(*out))return 0;
 /* Copy first: authenticated bytes are the bytes consumed by the CE owner. */
 memset(out,0,sizeof(*out));memcpy(out,input,bytes);
 uint8_t digest[32];storage_digest_bytes(out,bytes,digest);
 if(memcmp(digest,session->fixture,32)||out->magic!=CRYPTO_REQUEST_MAGIC||out->version!=1||
 !out->id||out->reserved||out->bytes!=bytes-CRYPTO_REQUEST_HEADER||
 out->bytes>CRYPTO_INPUT_MAX)goto fail;
 if(out->operation==CRYPTO_SHA256){
  unsigned key=0;for(unsigned i=0;i<16;++i)key|=out->key[i];if(key)goto fail;
 }else if((out->operation!=CRYPTO_AES_ENCRYPT&&out->operation!=CRYPTO_AES_DECRYPT)||
 !out->bytes||out->bytes%16)goto fail;
 return 1;
fail:
 crypto_key_clear(out->key);return 0;
}
