#include "a733_storage_update.h"
#include "a733_range.h"
#include "sunxi_smhc.h"
#include <string.h>
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t get64(const uint8_t *p){return get32(p)|(uint64_t)get32(p+4)<<32;}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static void put64(uint8_t *p,uint64_t v){put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}
static int nonzero(const uint8_t p[32]){unsigned v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
static int card_equal(const struct sunxi_mmc_card *a,const struct sunxi_mmc_card *b)
{return !memcmp(a->cid,b->cid,16)&&!memcmp(a->csd,b->csd,16)&&a->ocr==b->ocr&&a->sectors==b->sectors&&
    a->rca==b->rca&&a->sector_addressed==b->sector_addressed&&a->ext_revision==b->ext_revision;}
static int grant_valid(const struct a733_storage_grant *g)
{return g&&g->mode<=1&&(g->kind==SUNXI_MMC_SD||g->kind==SUNXI_MMC_EMMC)&&g->host<=1&&
    (g->kind!=SUNXI_MMC_SD||!g->host)&&nonzero(g->nonce)&&g->sectors&&g->blocks&&
    g->first<g->sectors&&g->blocks<=g->sectors-g->first&&g->blocks<=UINT64_MAX/512&&g->deadline_us&&
    nonzero(g->allocation)&&nonzero(g->recovery)&&
    (!g->mode||(nonzero(g->backup)&&nonzero(g->candidate)));}
int a733_storage_grant_decode(struct a733_storage_grant *out,const uint8_t *p,size_t n)
{
    if(!out||!p||n!=256||get32(p)!=0x53554731||get32(p+4)!=1||get32(p+20)||get64(p+232)||get64(p+240)||get64(p+248))return 0;
    struct a733_storage_grant g={.mode=get32(p+8),.kind=get32(p+12),.host=get32(p+16),
        .sectors=get64(p+72),.first=get64(p+80),.blocks=get64(p+88),.deadline_us=get64(p+96)};
    memcpy(g.nonce,p+24,32);for(unsigned i=0;i<4;++i)g.cid[i]=get32(p+56+4*i);
    memcpy(g.backup,p+104,32);memcpy(g.candidate,p+136,32);memcpy(g.allocation,p+168,32);memcpy(g.recovery,p+200,32);
    if(!grant_valid(&g))return 0;*out=g;return 1;
}
void a733_storage_grant_encode(const struct a733_storage_grant *g,uint8_t p[256])
{
    memset(p,0,256);put32(p,0x53554731);put32(p+4,1);put32(p+8,g->mode);put32(p+12,g->kind);put32(p+16,g->host);
    memcpy(p+24,g->nonce,32);for(unsigned i=0;i<4;++i)put32(p+56+4*i,g->cid[i]);
    put64(p+72,g->sectors);put64(p+80,g->first);put64(p+88,g->blocks);put64(p+96,g->deadline_us);
    memcpy(p+104,g->backup,32);memcpy(p+136,g->candidate,32);memcpy(p+168,g->allocation,32);memcpy(p+200,g->recovery,32);
}
int a733_storage_update_owned(const struct a733_storage_update *s,const struct a733_mmc_platform *p)
{return s&&s->self==s&&s->platform==p&&p&&s->identity.core==(uintptr_t)&p->core&&
    s->identity.session==p->core.config.token&&s->identity.kind==p->core.config.kind&&
    card_equal(&s->identity.card,&p->core.card)&&card_equal(&s->card,&p->core.card);}
int a733_storage_update_alive(const struct a733_storage_update *s)
{return s&&s->self==s&&!atomic_load_explicit(&s->lost,memory_order_acquire)&&
    (!s->transport_lost||!atomic_load_explicit(s->transport_lost,memory_order_acquire));}
static int fault(struct a733_storage_update *s)
{
    atomic_store_explicit(&s->lost,1,memory_order_release);s->state=A733_STORAGE_RETAINED;
    if(s->platform->update==s)a733_mmc_platform_update_retain(s->platform,s);
    return 0;
}
void a733_storage_update_lost(struct a733_storage_update *s)
{if(s&&s->self==s)atomic_store_explicit(&s->lost,1,memory_order_release);}
static int timely(struct a733_storage_update *s)
{
    uint64_t now;
    if(!a733_storage_update_alive(s)||!a733_storage_update_owned(s,s->platform)||
       s->platform->ops.now_us(s->platform->cookie,&now)!=1||!a733_deadline_admit(now,&s->last_now,s->grant.deadline_us))
        return fault(s);
    return 1;
}
int a733_storage_update_init(struct a733_storage_update *s,struct a733_mmc_platform *p,
    const struct a733_storage_grant *g,uint64_t deadline)
{
    if(!s||s->self||!p||!g||!a733_ranges_disjoint(s,sizeof(*s),g,sizeof(*g))||
       !a733_mmc_platform_storage(p,s,sizeof(*s))||!a733_mmc_platform_storage(p,g,sizeof(*g)))return 0;
    const struct a733_storage_grant copied=*g;g=&copied;
    if(!grant_valid(g)||!a733_ranges_disjoint(s,sizeof(*s),p,sizeof(*p))||g->deadline_us>p->config.lifetime_deadline_us||
       deadline>g->deadline_us||!deadline)return 0;
    struct sunxi_mmc_card card;
    if(a733_mmc_platform_card(p,&card,deadline)!=A733_MMC_READY||g->kind!=(uint32_t)p->config.kind||
       g->host!=(uint32_t)p->config.emmc_host||g->sectors!=card.sectors||memcmp(g->cid,card.cid,16))return 0;
    *s=(struct a733_storage_update){.self=s,.platform=p,.grant=*g,.card=card,
        .identity={(uintptr_t)&p->core,p->core.config.token,p->core.config.kind,card}};
    atomic_init(&s->lost,0);memcpy(s->chain,g->candidate,32);storage_digest_init(&s->before_hash);storage_digest_init(&s->after_hash);
    if(!a733_mmc_platform_update_bind(p,s)){s->self=NULL;return 0;}
    return timely(s);
}
int a733_storage_update_authorize(void *v,const struct sunxi_mmc_write_identity *id,const struct sunxi_mmc_write_request *r)
{
    struct a733_storage_update *s=v;
    if(!s||!id||!r||!a733_storage_update_owned(s,s->platform)||s->platform->update!=s||
       !a733_storage_update_alive(s)||s->state!=A733_STORAGE_WRITE_WAIT||
       !s->sealed||s->grant.mode!=1||s->finished||s->written_blocks>=s->grant.blocks)return 0;
    return id->core==s->identity.core&&id->session==s->identity.session&&id->kind==s->identity.kind&&
        card_equal(&id->card,&s->identity.card)&&r->policy_token==(uintptr_t)s&&
        r->first_sector==s->grant.first+s->written_blocks&&r->first_sector==s->request.first_sector&&
        r->blocks==s->chunk_bytes/512&&r->blocks==s->request.blocks&&r->blocks<=s->grant.blocks-s->written_blocks&&
        r->source==s->input&&r->source==s->request.source&&r->bytes==s->chunk_bytes&&
        r->bytes==s->request.bytes&&r->deadline_us==s->operation_deadline&&r->deadline_us==s->request.deadline_us;
}
static int write_admitted(const struct a733_storage_update *s)
{
    const struct a733_mmc_platform *p=s?s->platform:NULL;
    return p&&p->core_call&&p->held&&!p->fault&&
        a733_storage_update_authorize((void *)s,&p->core.write_identity,&p->core.write_request);
}
int a733_storage_update_fifo(const struct a733_storage_update *s,uintptr_t address,unsigned count)
{
    if(!write_admitted(s))return 0;
    const struct a733_mmc_platform *p=s->platform;
    return address==p->base+SUNXI_SMHC_FIFO&&count&&p->core.state==SUNXI_MMC_WRITING&&p->core.writing&&
        p->core.position<p->core.data_bytes&&count<=(p->core.data_bytes-p->core.position)/4;
}
int a733_storage_update_mmio(const struct a733_storage_update *s,uintptr_t address,uint32_t value)
{
    if(!write_admitted(s))return 0;
    const struct a733_mmc_platform *p=s->platform;
    if(address==p->base+SUNXI_SMHC_FIFO)return a733_storage_update_fifo(s,address,1);
    /* Exactly the core's single-sector CMD24 command word. */
    return address==p->base+SUNXI_SMHC_CMD&&value==(SUNXI_SMHC_CMD_START|SUNXI_SMHC_CMD_WAIT|SUNXI_SMHC_CMD_WRITE|
        SUNXI_SMHC_CMD_DATA|SUNXI_SMHC_CMD_CRC|SUNXI_SMHC_CMD_RESP|24u);
}
static void respond(struct a733_storage_update *s,uint64_t offset,const uint8_t *data,unsigned bytes,const uint8_t *digest)
{
    memset(s->response,0,56);put32(s->response,s->sequence);put64(s->response+8,offset);put32(s->response+16,bytes);
    if(digest)memcpy(s->response+24,digest,32);if(bytes)memcpy(s->response+56,data,bytes);
    s->response_size=56+bytes;s->response_ready=1;s->state=A733_STORAGE_IDLE;
}
int a733_storage_update_request(struct a733_storage_update *s,const uint8_t *p,size_t n)
{
    if(!s||s->self!=s||!p||!a733_ranges_disjoint(s,sizeof(*s),p,n)||s->platform->ops.current(s->platform->cookie)!=1)return 0;
    if(!timely(s)||s->finished||s->state!=A733_STORAGE_IDLE||s->response_ready||n<88||
       memcmp(p,s->grant.nonce,32)||s->sequence==UINT32_MAX||get32(p+32)!=s->sequence+1||get32(p+52))return fault(s);
    unsigned op=get32(p+36),bytes=get32(p+48);uint64_t offset=get64(p+40);
    if(bytes>A733_STORAGE_WRITE_BYTES||n!=88u+bytes+(op==A733_STORAGE_WRITE?32u:0u))return fault(s);
    ++s->sequence;s->active_command=op;
    uint64_t remaining=s->grant.deadline_us-s->last_now;
    s->operation_deadline=s->last_now+(remaining<1000000?remaining:1000000);
    if(op==A733_STORAGE_INFO) {
        if(bytes||offset)return fault(s);
        uint8_t info[A733_STORAGE_GRANT_BYTES+4];a733_storage_grant_encode(&s->grant,info);
        put32(info+A733_STORAGE_GRANT_BYTES,A733_STORAGE_READ_BLOCKS);respond(s,0,info,sizeof(info),NULL);return 1;
    }
    if(op==A733_STORAGE_BACKUP) {
        if(bytes||s->sealed||offset!=s->read_blocks||offset>=s->grant.blocks)return fault(s);
        /* Reads finish well inside the operation deadline even at 400 kHz:
         * eight sectors take about 85 ms of one-bit bus time. */
        uint64_t left=s->grant.blocks-offset;
        s->chunk_bytes=(unsigned)(left>A733_STORAGE_READ_BLOCKS?A733_STORAGE_READ_BLOCKS:left)*512;
        s->state=A733_STORAGE_READ_START;return 1;
    }
    if(op==A733_STORAGE_SEAL) {
        if(bytes||offset!=s->grant.blocks||s->read_blocks!=s->grant.blocks||s->sealed||!nonzero(p+56))return fault(s);
        storage_digest_end(&s->before_hash,s->backup_hash);
        if(s->grant.mode&&memcmp(s->grant.backup,s->backup_hash,32))return fault(s);
        memcpy(s->receipt,p+56,32);s->sealed=1;respond(s,offset,NULL,0,s->backup_hash);return 1;
    }
    if(op==A733_STORAGE_WRITE) {
        uint64_t left=s->grant.blocks-s->written_blocks;
        /* One sector per request: its CMD24 busy period and readback share
         * the operation deadline (docs/storage-update.md). */
        if(s->grant.mode!=1||!s->sealed||s->finished||!left||offset!=s->written_blocks||
           bytes!=A733_STORAGE_WRITE_BYTES||!nonzero(p+56))return fault(s);
        uint8_t digest[32],header[16]={0};struct storage_digest commitment;
        put64(header,offset);put32(header+8,bytes);storage_digest_init(&commitment);
        if(!storage_digest_add(&commitment,header,16)||!storage_digest_add(&commitment,p+88,32)||
           !storage_digest_add(&commitment,p+120,bytes))return fault(s);
        storage_digest_end(&commitment,digest);
        if(memcmp(digest,s->chain,32))return fault(s);
        memcpy(s->next_chain,p+88,32);memcpy(s->input,p+120,bytes);s->chunk_bytes=bytes;memcpy(s->receipt,p+56,32);
        s->request=(struct sunxi_mmc_write_request){(uintptr_t)s,s->grant.first+offset,bytes/512,s->input,bytes,s->operation_deadline};
        s->state=A733_STORAGE_WRITE_START;return 1;
    }
    if(op==A733_STORAGE_FINISH) {
        if(bytes||offset!=s->grant.blocks||!s->sealed||s->finished||
           (s->grant.mode&&s->written_blocks!=s->grant.blocks))return fault(s);
        uint8_t digest[32];storage_digest_end(s->grant.mode?&s->after_hash:&s->before_hash,digest);
        if(s->grant.mode&&nonzero(s->chain))return fault(s);
        s->finished=1;respond(s,offset,NULL,0,digest);return 1;
    }
    return fault(s);
}
void a733_storage_update_poll(struct a733_storage_update *s)
{
    if(!s||s->self!=s||s->platform->ops.current(s->platform->cookie)!=1)return;
    if(!timely(s)||s->state==A733_STORAGE_RETAINED){fault(s);return;}
    if(s->state==A733_STORAGE_IDLE)return;
    enum a733_mmc_platform_result r;
    uint64_t offset=s->state==A733_STORAGE_READ_START?s->read_blocks:s->written_blocks;
    if(s->state==A733_STORAGE_READ_START||s->state==A733_STORAGE_VERIFY_START) {
        r=a733_mmc_platform_update_io(s->platform,s,0,s->grant.first+offset,s->chunk_bytes/512,s->readback,s->chunk_bytes,s->operation_deadline);
        if(r==A733_MMC_BUSY)return;
        if(r!=A733_MMC_PENDING){fault(s);return;}
        s->state=s->state==A733_STORAGE_READ_START?A733_STORAGE_READ_WAIT:A733_STORAGE_VERIFY_WAIT;return;
    }
    if(s->state==A733_STORAGE_WRITE_START) {
        s->state=A733_STORAGE_WRITE_WAIT;
        r=a733_mmc_platform_update_io(s->platform,s,3,s->request.first_sector,s->request.blocks,s->input,s->chunk_bytes,s->operation_deadline);
        if(r==A733_MMC_BUSY){s->state=A733_STORAGE_WRITE_START;return;}
        if(r!=A733_MMC_PENDING)fault(s);return;
    }
    r=a733_mmc_platform_update_poll(s->platform,s);
    if(r==A733_MMC_BUSY||r==A733_MMC_PENDING)return;
    if(r!=A733_MMC_COMPLETE){fault(s);return;}
    r=a733_mmc_platform_update_io(s->platform,s,1,0,0,&s->result,sizeof(s->result),s->operation_deadline);
    if(r==A733_MMC_BUSY)return;
    if(r!=A733_MMC_READY||s->result.error!=SUNXI_MMC_OK){fault(s);return;}
    if(s->state==A733_STORAGE_WRITE_WAIT){s->state=A733_STORAGE_VERIFY_START;return;}
    if(s->result.received!=s->chunk_bytes){fault(s);return;}
    uint8_t digest[32];storage_digest_bytes(s->readback,s->chunk_bytes,digest);
    if(s->state==A733_STORAGE_READ_WAIT) {
        if(!storage_digest_add(&s->before_hash,s->readback,s->chunk_bytes)){fault(s);return;}
        uint64_t at=s->read_blocks;s->read_blocks+=s->chunk_bytes/512;respond(s,at,s->readback,s->chunk_bytes,digest);
    }else {
        if(memcmp(s->input,s->readback,s->chunk_bytes)||!storage_digest_add(&s->after_hash,s->readback,s->chunk_bytes)){fault(s);return;}
        uint64_t at=s->written_blocks;s->written_blocks+=s->chunk_bytes/512;memcpy(s->chain,s->next_chain,32);respond(s,at,NULL,0,digest);
    }
}
int a733_storage_update_response(struct a733_storage_update *s,uint8_t *out,size_t capacity,size_t *n)
{
    if(!s||s->self!=s||s->platform->ops.current(s->platform->cookie)!=1||!out||!n||!a733_ranges_disjoint(s,sizeof(*s),out,capacity)||!a733_ranges_disjoint(s,sizeof(*s),n,sizeof(*n))||
       !a733_ranges_disjoint(out,capacity,n,sizeof(*n))||!a733_mmc_platform_storage(s->platform,out,capacity)||
       !a733_mmc_platform_storage(s->platform,n,sizeof(*n))||!s->response_ready||capacity<s->response_size||!timely(s))return 0;
    memcpy(out,s->response,s->response_size);*n=s->response_size;s->response_ready=0;return 1;
}
