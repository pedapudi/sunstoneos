#include "input.h"
#include "a733_storage_digest.h"
#include <string.h>
static uint32_t le32(const uint8_t *p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t le64(const uint8_t *p){return le32(p)|((uint64_t)le32(p+4)<<32);}
static int reject(struct hevc_input *p){if(p)p->failed=1;return -1;}
int hevc_input_bind(struct hevc_input *p,uint8_t *bytes,size_t capacity,void *workspace,size_t n)
{
 if(!p||p->bytes||p->failed||p->header_used||!bytes||(uintptr_t)bytes%4096||capacity!=524288||
 !workspace||(uintptr_t)workspace%8||n<a733_hevc_parser_bytes())return 0;
 p->bytes=bytes;p->capacity=capacity;p->workspace=workspace;p->workspace_bytes=n;return 1;
}
int hevc_input_ready(const struct hevc_input *p)
{return p&&atomic_load_explicit(&p->committed,memory_order_acquire)==1;}
size_t hevc_input_remaining(const struct hevc_input *p)
{return !p||!p->bytes||p->failed||hevc_input_ready(p)?0:p->header_used<32?32-p->header_used:p->total-p->received;}
int hevc_input_feed(struct hevc_input *p,const struct a733_hevc_session *s,
 int (*clock)(void *,uint64_t *),void *context,uint64_t deadline,const uint8_t *b,size_t n)
{
 uint64_t now;
 if(!p||!s||!clock||!b||!n||n>4096||p->failed||hevc_input_ready(p)||!p->bytes||
 p->capacity!=524288||!deadline||clock(context,&now)!=1||now>=deadline||!a733_hevc_session_valid(s))return reject(p);
 uint64_t ranges[]={s->pictures[0].offset,s->pictures[0].bytes,s->pictures[1].offset,s->pictures[1].bytes};
 if(!p->header_used){p->epoch=s->board.epoch;p->generation=s->board.generation;p->authority=s->authority;
  p->deadline=deadline;p->total=(size_t)(ranges[2]+ranges[3]);memcpy(p->ranges,ranges,sizeof(ranges));memcpy(p->hash,s->input_digest,32);}
 if(p->epoch!=s->board.epoch||p->generation!=s->board.generation||p->authority!=s->authority||
 p->deadline!=deadline||now<p->last_now||memcmp(p->hash,s->input_digest,32)||memcmp(p->ranges,ranges,sizeof(ranges))||n>hevc_input_remaining(p))return reject(p);
 p->last_now=now;
 if(p->header_used<32){
  memcpy(p->header+p->header_used,b,n);p->header_used+=n;
  if(p->header_used!=32)return 0;
  if(le32(p->header)!=HEVC_UPLOAD_MAGIC||le32(p->header+4)!=1||le32(p->header+8)!=p->total||
   le32(p->header+12)!=2||le64(p->header+16)!=p->epoch||le64(p->header+24)!=p->generation)return reject(p);
  return 0;
 }
 memcpy(p->bytes+p->received,b,n);p->received+=n;if(p->received!=p->total)return 0;
 uint8_t hash[32];storage_digest_bytes(p->bytes,p->received,hash);
 if(memcmp(hash,p->hash,32)||!a733_hevc_parse(p->bytes,p->received,p->ranges,p->workspace,p->workspace_bytes,&p->plan)||
 clock(context,&now)!=1||now<p->last_now||now>=deadline)return reject(p);
 p->last_now=now;atomic_store_explicit(&p->committed,1,memory_order_release);return 1;
}
