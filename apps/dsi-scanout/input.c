#include "input.h"
#include "a733_storage_digest.h"
#include <string.h>
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t le64(const uint8_t *p){return le32(p)|((uint64_t)le32(p+4)<<32);}
static int reject(struct dsi_input *p){if(p)p->failed=1;return -1;}
/* Running digest of the stored copy of the slot being uploaded. Each fragment
 * is hashed back from the bank right after its copy, so the work is spread
 * over the transfer instead of one whole-slot pass at completion. The final
 * fragment is added to a copy, leaving this state unchanged on refusal. One
 * input exists per image; upload_owner names it. */
static struct storage_digest upload_digest;
static const struct dsi_input *upload_owner;
size_t dsi_input_remaining(const struct dsi_input *p)
{return !p||p->failed||!atomic_load_explicit(&p->allocated,memory_order_acquire)?0:
 p->header_used<96?96-p->header_used:(size_t)(p->bytes-p->received);}
int dsi_input_feed(struct dsi_input *p,const struct dsi_session *s,int (*clock)(void *,uint64_t *),void *context,const uint8_t *bytes,size_t n)
{
 uint64_t now;
 if(!p||!s||!bytes||!clock||!n||n>512||!atomic_load_explicit(&p->allocated,memory_order_acquire)||
 p->failed||s->version!=2||clock(context,&now)!=1||now<p->last_now||now>=p->deadline||
 p->epoch!=s->board.epoch||p->generation!=s->board.generation||p->authority!=s->authority||
 p->bytes!=dsi_frame_bytes(s)||!p->bank||n>dsi_input_remaining(p))return reject(p);
 p->last_now=now;
 if(p->header_used<96){
  memcpy(p->header+p->header_used,bytes,n);p->header_used+=n;if(p->header_used<96)return 0;
  const uint8_t *h=p->header;unsigned op=le32(h+8),slot=le32(h+12);uint64_t sequence=le64(h+40);
  if(le32(h)!=DSI_INPUT_MAGIC||le32(h+4)!=1||slot>1||le64(h+16)!=p->epoch||
   le64(h+24)!=p->generation||le64(h+32)!=p->authority||le64(h+88))return reject(p);
  if(op==1){
   if(sequence!=slot+1||le64(h+48)!=p->bytes||memcmp(h+56,s->replay.hashes[slot],32)||
    atomic_load_explicit(&p->committed[slot],memory_order_acquire))return reject(p);
   p->slot=slot;p->received=0;storage_digest_init(&upload_digest);upload_owner=p;return 0;
  }
  if(op!=2||le64(h+48)||sequence!=p->next_sequence+1||!sequence||
   memcmp(h+56,(uint8_t[32]){0},32)||atomic_load_explicit(&p->request,memory_order_acquire)||
   !atomic_load_explicit(&p->committed[0],memory_order_acquire)||!atomic_load_explicit(&p->committed[1],memory_order_acquire))return reject(p);
  if(clock(context,&now)!=1||now<p->last_now||now>=p->deadline)return reject(p);
  p->last_now=now;p->sequence=sequence;p->next_sequence=sequence;p->requested_slot=slot;p->header_used=0;
  atomic_store_explicit(&p->request,1,memory_order_release);return 1;
 }
 if(atomic_load_explicit(&p->committed[p->slot],memory_order_acquire)||upload_owner!=p)return reject(p);
 const void *stored=(const void *)(p->bank+p->slot*p->bytes+p->received);
 memcpy((void *)stored,bytes,n);p->received+=n;
 if(p->received!=p->bytes)return storage_digest_add(&upload_digest,stored,n)?0:reject(p);
 struct storage_digest final=upload_digest;uint8_t hash[32];
 if(!storage_digest_add(&final,stored,n))return reject(p);
 storage_digest_end(&final,hash);
 if(memcmp(hash,s->replay.hashes[p->slot],32)||clock(context,&now)!=1||now<p->last_now||now>=p->deadline)return reject(p);
 upload_digest=final;
 p->last_now=now;p->header_used=0;atomic_store_explicit(&p->committed[p->slot],1,memory_order_release);return 1;
}
