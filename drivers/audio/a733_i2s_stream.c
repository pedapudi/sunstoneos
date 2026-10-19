#include "a733_i2s_stream.h"
#include <string.h>

enum { CTL=0,FMT0=4,FMT1=8,ISTA=12,FCTL=20,FSTA=24,ICTL=28,
    CLK=36,TXCNT=40,RXCNT=44,CHCFG=48,TXSEL=52,TXMAP0=68,TXMAP1=72,
    RXSEL=100,RXMAP0=104,RXMAP1=108,RXMAP2=112,RXMAP3=116 };
enum { FREE,QUEUED,DEVICE,DONE,CLIENT };
enum { ACQUIRE,CONFIGURE,FLUSH,ACTIVE,DRAIN,PUBLISH,STOP,CLOSE,EOF_FIFO };
#define IO(s) ((s)->profile.operations.io)
#define COOKIE(s) ((s)->profile.cookie)
static int tx(const struct a733_i2s_stream *s) {return s->profile.direction==A733_I2S_TRANSMIT;}
static unsigned errors(const struct a733_i2s_stream *s) {return tx(s)?0x60:6;}
static unsigned flush(const struct a733_i2s_stream *s) {return 1u<<(tx(s)?25:24);}
static unsigned drq(const struct a733_i2s_stream *s) {return tx(s)?128:8;}
static enum a733_i2s_status fault(struct a733_i2s_stream *s)
{
    s->status=A733_I2S_RECOVERY;
    if(s->peer)s->peer->status=A733_I2S_RECOVERY;
    return s->status;
}
static int valid(const struct a733_i2s_stream *s)
{return s && s->self==s && s->status!=A733_I2S_RECOVERY;}
static int now(struct a733_i2s_stream *s)
{
    uint64_t n;
    if(!IO(s).now_us(COOKIE(s),&n) || n<s->time)return 0;
    s->time=n;return !s->deadline || n<s->deadline;
}
static int health(struct a733_i2s_stream *s)
{
    if(!now(s))return -1;
    if(!s->deadline) {
        if(s->time>UINT64_MAX-s->profile.operation_us)return -1;
        s->deadline=s->time+s->profile.operation_us;
    }
    enum a733_i2s_resource_result r=s->profile.operations.health(COOKIE(s),s->deadline);
    if(!now(s) || r==A733_I2S_RESOURCE_RETAINED)return -1;
    return r==A733_I2S_RESOURCE_READY ? 1 : r==A733_I2S_RESOURCE_BUSY ? 0 : -1;
}
static int rd(struct a733_i2s_stream *s,unsigned a,uint32_t *v)
{return now(s) && IO(s).read(COOKIE(s),A733_I2S0_BASE+a,v) && now(s);}
static int wr(struct a733_i2s_stream *s,unsigned a,uint32_t v)
{return now(s) && IO(s).write(COOKIE(s),A733_I2S0_BASE+a,v) && now(s);}
static int checked(struct a733_i2s_stream *s,unsigned a,uint32_t v)
{uint32_t got;return wr(s,a,v) && rd(s,a,&got) && got==v;}
/* Only the serialized worker changes CTL/ICTL; preserve the peer's enable and
 * request bits at every start, handoff, cancellation and drain boundary. */
static int requests(struct a733_i2s_stream *s,unsigned enabled)
{
    uint32_t v;if(!rd(s,ICTL,&v))return 0;
    return checked(s,ICTL,(v&~drq(s))|(enabled?drq(s):0));
}
static int control(struct a733_i2s_stream *s,unsigned bits)
{
    uint32_t v;if(!rd(s,CTL,&v))return 0;
    unsigned own=tx(s)?4u:2u,other=tx(s)?2u:4u;
    v=(v&~(own|1u))|(bits&own)|((bits&1u)||(v&other)?1u:0u);
    return checked(s,CTL,v);
}
static int mapping(const struct a733_dma_mapping *m)
{
    return m->cpu && m->bytes && !((uintptr_t)m->cpu&63) && !(m->bus&63) && !(m->bytes&63) &&
        m->bytes<=UINTPTR_MAX-(uintptr_t)m->cpu && m->bus<(UINT64_C(1)<<35) &&
        m->bytes<=(UINT64_C(1)<<35)-m->bus;
}
static int overlaps(const struct a733_dma_mapping *a,const struct a733_dma_mapping *b)
{
    return ((uintptr_t)a->cpu<(uintptr_t)b->cpu+b->bytes &&
        (uintptr_t)b->cpu<(uintptr_t)a->cpu+a->bytes) ||
        (a->bus<b->bus+b->bytes && b->bus<a->bus+a->bytes);
}
int a733_i2s_stream_valid(const struct a733_i2s_stream_profile *p,const struct a733_dma_transfer *t)
{
    struct a733_i2s_plan plan;
    if(!p || !a733_i2s_plan(&p->format,&plan) || (unsigned)p->direction>A733_I2S_TRANSMIT ||
        p->cyclic>1 || !p->operation_us || p->operation_us>INT64_MAX || !p->operations.health ||
        !p->operations.io.acquire || !p->operations.io.acquire_poll || !p->operations.io.release ||
        !p->operations.io.read || !p->operations.io.write || !p->operations.io.now_us ||
        !t || t->count<2 || t->count>A733_I2S_STREAM_PERIODS || t->mapping_count!=t->count ||
        !t->segments || !t->mappings || !mapping(&t->descriptors) ||
        t->descriptors.access!=A733_DMA_DESCRIPTORS || t->descriptors.bytes<t->count*64 ||
        t->descriptors.bus>UINT32_C(0xfffff800)-t->count*64 ||
        (p->direction==A733_I2S_TRANSMIT && p->format.first_slot))return 0;
    for(size_t i=0;i<t->count;++i) {
        const struct a733_dma_segment *g=&t->segments[i];
        const struct a733_dma_mapping *m=&t->mappings[i];
        const struct a733_dma_endpoint *fifo=p->direction==A733_I2S_RECEIVE?&g->source:&g->destination;
        const struct a733_dma_endpoint *ram=p->direction==A733_I2S_RECEIVE?&g->destination:&g->source;
        if(!mapping(m) || m->access!=(p->direction==A733_I2S_RECEIVE?A733_DMA_WRITE:A733_DMA_READ) ||
            !g->bytes || g->bytes>A733_DMA_MAX_SEGMENT_BYTES || g->bytes!=t->segments[0].bytes ||
            g->bytes%plan.frame_bytes || g->bytes%(plan.dma_width*8) || g->bytes>m->bytes || ram->address!=m->bus || ram->request ||
            fifo->address!=(p->direction==A733_I2S_RECEIVE?A733_I2S0_RXFIFO:A733_I2S0_TXFIFO) ||
            fifo->request!=3 || fifo->width!=plan.dma_width || ram->width!=plan.dma_width ||
            fifo->burst!=8 || ram->burst!=8 || overlaps(m,&t->descriptors) ||
            (p->cyclic && (g->bytes!=m->bytes || (p->direction==A733_I2S_RECEIVE &&
             (g->bytes!=4096 || m->bus>UINT32_MAX-m->bytes)))))return 0;
        for(size_t k=0;k<i;++k)if(overlaps(m,&t->mappings[k]))return 0;
    }
    return 1;
}
static enum a733_i2s_status configure(struct a733_i2s_stream *s)
{
    int h=health(s);if(h<0)return fault(s);if(!h)return s->status;
    if(!s->dma_held) {
        if(a733_dma_claim(s->dma,s->channel)!=A733_DMA_CONTROL_OK)return fault(s);
        s->dma_held=1;
    }
    if(s->peer) {
        /* Common framing and both directions' maps were fixed before START. */
        if(!s->peer->dma_held)return fault(s);
        s->deadline=0;return s->status=A733_I2S_READY;
    }
    uint32_t ctl,ic,fc;
    if(!rd(s,CTL,&ctl) || (ctl&7) || !rd(s,ICTL,&ic) || ic || !rd(s,FCTL,&fc) ||
        (fc&((1u<<31)|(3u<<24))))return fault(s);
    uint32_t cfg=s->duplex?(s->plan.chcfg|(s->profile.format.channels-1)):
        tx(s)?s->profile.format.channels-1:s->plan.chcfg;
    if(!checked(s,CTL,s->plan.ctl) || !checked(s,FMT0,s->plan.fmt0) || !checked(s,FMT1,0) ||
        !checked(s,CLK,s->plan.clkdiv) || !checked(s,CHCFG,cfg))return fault(s);
    if(tx(s) || s->duplex) {
        /* DOUT0 slots 0/1 map to contiguous FIFO channels 0/1. */
        unsigned channels=s->profile.format.channels;
        if(!checked(s,TXSEL,(1u<<20)|((channels-1)<<16)|((1u<<channels)-1)) ||
            !checked(s,TXMAP0,0) || !checked(s,TXMAP1,channels==2?0x10:0))return fault(s);
    }
    if((!tx(s) || s->duplex) && (!checked(s,RXSEL,s->plan.rxchsel) || !checked(s,RXMAP0,0) ||
        !checked(s,RXMAP1,0) || !checked(s,RXMAP2,0) || !checked(s,RXMAP3,s->plan.rxmap3)))return fault(s);
    /* Trigger levels: RX >= 8 entries; TX empty space >= 64 entries. TXIM=1, RXOM=1.
     * Both 16-bit and 32-bit sample words use the source-defined packing. */
    if(!checked(s,FCTL,(fc&~UINT32_C(0x7f3f7))|(64u<<12)|(7u<<4)|5u) ||
        !now(s))return fault(s);
    s->deadline=0;return s->status=A733_I2S_READY;
}
static enum a733_i2s_status open_stream(struct a733_i2s_stream *s,
    const struct a733_i2s_stream_profile *p,const struct a733_dma_transfer *t,
    struct a733_dma_controller *dma,unsigned channel,uint64_t deadline,
    unsigned duplex,struct a733_i2s_stream *peer)
{
    if(!s || s->self || !a733_i2s_stream_valid(p,t) || !dma || !dma->initialized ||
        dma->base!=0x04601000u || channel>=A733_DMA_CHANNELS || !deadline)return A733_I2S_INVALID;
    struct a733_i2s_plan plan;if(!a733_i2s_plan(&p->format,&plan))return A733_I2S_INVALID;
    if(p->direction==A733_I2S_TRANSMIT) {
        plan.pb_mask=(plan.pb_mask&~(1u<<8))|(1u<<7);plan.ctl|=1u<<8;
    }
    if(duplex){plan.pb_mask|=(1u<<7)|(1u<<8);plan.ctl|=1u<<8;}
    if(peer && (!duplex || !valid(peer) || !peer->duplex || peer->peer ||
        peer->status!=A733_I2S_READY || peer->dma!=dma || peer->channel==channel ||
        peer->profile.direction==p->direction || peer->profile.cookie!=p->cookie ||
        memcmp(&peer->profile.format,&p->format,sizeof(p->format))))return A733_I2S_CONFLICT;
    uint64_t n;if(!p->operations.io.now_us(p->cookie,&n) || n>=deadline)return A733_I2S_CONFLICT;
    *s=(struct a733_i2s_stream){.self=s,.profile=*p,.plan=plan,.dma=dma,.catalogue=t,
        .deadline=deadline,.time=n,.channel=channel,.count=(unsigned)t->count,.duplex=duplex,
        .status=A733_I2S_OPENING,.current=A733_I2S_STREAM_PERIODS};
    if(peer){
        s->peer=peer;peer->peer=s;s->platform_held=1;s->stage=CONFIGURE;
        return s->status;
    }
    enum a733_i2s_resource_result r=IO(s).acquire(COOKIE(s),&s->plan,deadline);
    if(r==A733_I2S_RESOURCE_BUSY) { *s=(struct a733_i2s_stream){0};return A733_I2S_CONFLICT; }
    s->platform_held=1;
    if(!now(s) || r==A733_I2S_RESOURCE_RETAINED)return fault(s);
    if(r==A733_I2S_RESOURCE_READY)s->stage=CONFIGURE;
    else if(r!=A733_I2S_RESOURCE_PENDING)return fault(s);
    return s->status;
}
enum a733_i2s_status a733_i2s_stream_open(struct a733_i2s_stream *s,
    const struct a733_i2s_stream_profile *p,const struct a733_dma_transfer *t,
    struct a733_dma_controller *dma,unsigned channel,uint64_t deadline)
{return open_stream(s,p,t,dma,channel,deadline,0,NULL);}
enum a733_i2s_status a733_i2s_stream_duplex_open(struct a733_i2s_stream *s,
    const struct a733_i2s_stream_profile *p,const struct a733_dma_transfer *t,
    struct a733_dma_controller *dma,unsigned channel,uint64_t deadline,struct a733_i2s_stream *peer)
{return open_stream(s,p,t,dma,channel,deadline,1,peer);}
static int requests_off(struct a733_i2s_stream *s) {return requests(s,0);}
static enum a733_i2s_status stop(struct a733_i2s_stream *s,enum a733_i2s_status outcome)
{
    if(!requests_off(s) || !control(s,1))return fault(s);
    s->outcome=outcome;s->stage=STOP;s->status=A733_I2S_STOPPING;
    if(a733_dma_owns(s->dma,&s->active)) {
        enum a733_dma_control_status d=a733_dma_cancel(s->dma,s->channel);
        if(d!=A733_DMA_STOPPING)return fault(s);
    }
    return s->status;
}
static enum a733_i2s_status arm(struct a733_i2s_stream *s)
{
    unsigned i=s->next;
    if(s->profile.cyclic) {
        if(tx(s))for(unsigned n=0;n<s->count;++n)if(s->slots[n].state!=QUEUED)return stop(s,A733_I2S_FAILED);
        s->active=*s->catalogue;s->current=A733_I2S_STREAM_PERIODS;
        if(!requests_off(s))return fault(s);
        enum a733_dma_control_status d=tx(s)?a733_dma_submit_cyclic_tx(s->dma,s->channel,&s->active,s->timeout):
            a733_dma_submit_cyclic_rx(s->dma,s->channel,&s->active,s->timeout);
        if(d==A733_DMA_RECOVERY)return fault(s);
        if(d==A733_DMA_ACTIVE || d==A733_DMA_STOPPING) {
            s->cyclic_started=1;
            for(unsigned n=0;n<s->count;++n)s->slots[n].state=DEVICE;
        }
        if(d!=A733_DMA_ACTIVE)return stop(s,A733_I2S_FAILED);
        if(!now(s))return fault(s);
        s->cyclic_observed_us=s->cyclic_since=s->time;
        if(!requests(s,1) || (!tx(s) && !control(s,1|2)) || !now(s))return fault(s);
        s->stage=ACTIVE;s->status=A733_I2S_ACTIVE;return s->status;
    }
    if(s->slots[i].state!=(tx(s)?QUEUED:FREE))return stop(s,A733_I2S_FAILED);
    s->active_segment=s->catalogue->segments[i];
    if(s->input_ended&&i==s->end_slot)s->active_segment.bytes=s->end_frames*s->plan.frame_bytes;
    s->active=(struct a733_dma_transfer){s->catalogue->descriptors,&s->active_segment,1,
        &s->catalogue->mappings[i],1};
    s->active.descriptors.cpu=(unsigned char *)s->active.descriptors.cpu+i*64;
    s->active.descriptors.bus+=i*64;s->active.descriptors.bytes=64;
    s->current=i;s->slots[i].state=DEVICE;
    enum a733_dma_control_status d=a733_dma_submit(s->dma,s->channel,&s->active,s->timeout);
    if(!now(s))return fault(s);
    if(d!=A733_DMA_ACTIVE) {
        if(d==A733_DMA_RECOVERY)return fault(s);
        return stop(s,A733_I2S_FAILED);
    }
    if(s->bridge_deadline && s->time>=s->bridge_deadline)return stop(s,A733_I2S_FAILED);
    if(!requests(s,1))return fault(s);
    if(!tx(s) && !control(s,1|2))return fault(s);
    if(!now(s))return fault(s);
    if(s->bridge_deadline && s->time>=s->bridge_deadline)return stop(s,A733_I2S_FAILED);
    s->bridge_deadline=0;s->stage=ACTIVE;s->status=A733_I2S_ACTIVE;return s->status;
}
enum a733_i2s_status a733_i2s_stream_start(struct a733_i2s_stream *s,uint64_t timeout)
{
    if(!valid(s) || s->status!=A733_I2S_READY || !timeout || timeout>INT64_MAX)return A733_I2S_INVALID;
    if(s->duplex && (!s->peer || (s->peer->status!=A733_I2S_READY &&
        s->peer->status!=A733_I2S_ACTIVE && s->peer->status!=A733_I2S_PREPARING)))return A733_I2S_CONFLICT;
    if(tx(s) && (s->slots[0].state!=QUEUED || (s->slots[1].state!=QUEUED&&!(s->input_ended&&s->end_slot==0))))return A733_I2S_CONFLICT;
    if(tx(s) && s->profile.cyclic)for(unsigned n=0;n<s->count;++n)
        if(s->slots[n].state!=QUEUED)return A733_I2S_CONFLICT;
    int h=health(s);if(h<0)return fault(s);if(!h)return A733_I2S_CONFLICT;
    s->timeout=timeout;s->outcome=A733_I2S_FAILED;s->priming=tx(s);
    uint32_t fc;
    if(!control(s,1) || !rd(s,FCTL,&fc) || !wr(s,FCTL,fc|flush(s)) ||
        !checked(s,tx(s)?TXCNT:RXCNT,0) || !wr(s,ISTA,errors(s)))return fault(s);
    s->stage=FLUSH;s->status=A733_I2S_PREPARING;
    /* Keep this deadline until the self-clearing FIFO operation completes. */
    return s->status;
}
static enum a733_i2s_status publish(struct a733_i2s_stream *s)
{
    int h=health(s);if(h<0)return fault(s);if(!h)return s->status;
    uint32_t status,counter;
    if(!rd(s,ISTA,&status) || !rd(s,tx(s)?TXCNT:RXCNT,&counter))return fault(s);
    if(status&errors(s))return stop(s,A733_I2S_FAILED);
    if(s->bridge_deadline && s->time>=s->bridge_deadline)return stop(s,A733_I2S_FAILED);
    struct a733_i2s_stream_slot *slot=&s->slots[s->current];
    unsigned frames=s->active_segment.bytes/s->plan.frame_bytes;
    if(s->frames>UINT64_MAX-frames)return fault(s);
    slot->result=(struct a733_i2s_period){.slot=s->current,.first_frame=s->frames,
        .frames=frames,.counter=counter,.observed_before_us=slot->result.observed_before_us,
        .observed_after_us=s->time,.kind=tx(s)?A733_I2S_TX_MEMORY_CONSUMED:A733_I2S_CAPTURED};
    slot->state=DONE;s->frames+=frames;s->next=(s->current+1)%s->count;
    unsigned final=s->input_ended&&s->current==s->end_slot;
    s->current=A733_I2S_STREAM_PERIODS;
    if(final){
        /* DMA's existing completion/drain contract has returned this allocation.
         * Leave the serial direction running. FIFO space is an observation,
         * not a serializer fence, and does not authorize release. */
        if(s->time>UINT64_MAX-s->profile.operation_us)return fault(s);
        if(s->priming){if(!control(s,1|4))return fault(s);s->priming=0;}
        s->fifo_deadline=s->time+s->profile.operation_us;s->stage=EOF_FIFO;
        return s->status;
    }
    return arm(s);
}
static int cyclic_frames(struct a733_i2s_stream *s)
{
    uint64_t count;unsigned frames=s->catalogue->segments[0].bytes/s->plan.frame_bytes;
    if(!a733_dma_cyclic_progress(s->dma,s->channel,&count) || count>UINT64_MAX/frames)return 0;
    uint64_t total=count*frames;
    uint64_t elapsed=s->time-s->cyclic_since,rate=s->profile.format.rate_hz;
    /* Observe at least once per second: even the maximum supported sample
     * demand cannot complete 2^32 nonempty periods in that interval. The
     * cumulative rate bound separately rejects implausible forward jumps. */
    if(s->time-s->cyclic_observed_us>=1000000 || elapsed>UINT64_MAX/rate || elapsed*rate/1000000>UINT64_MAX-128 ||
       total>elapsed*rate/1000000+128 || total<s->frames)return 0;
    s->cyclic_observed_us=s->time;s->frames=total;return 1;
}
static enum a733_i2s_status cyclic_active(struct a733_i2s_stream *s)
{
    enum a733_dma_control_status d=a733_dma_poll(s->dma,s->channel);
    if(!now(s) || d==A733_DMA_RECOVERY || !cyclic_frames(s))return fault(s);
    if(d!=A733_DMA_ACTIVE)return stop(s,A733_I2S_FAILED);
    if(!tx(s)) {
        unsigned frames=s->catalogue->segments[0].bytes/s->plan.frame_bytes;
        uint64_t count=s->frames/frames;
        if(s->raw_next>UINT64_MAX-s->count || count>=s->raw_next+s->count)return stop(s,A733_I2S_FAILED);
        if(s->snapshot_state==FREE && count>s->raw_next) {
            uint64_t before=s->time;
            int copied=a733_dma_cyclic_rx_copy(s->dma,s->channel,s->raw_next,s->snapshot,4096);
            if(copied<0)return stop(s,A733_I2S_FAILED);
            if(copied) {
                uint32_t counter;if(!rd(s,RXCNT,&counter) || !cyclic_frames(s) || !now(s))return fault(s);
                s->snapshot_result=(struct a733_i2s_period){.slot=(unsigned)(s->raw_next%s->count),
                    .first_frame=s->raw_next*frames,.frames=frames,.counter=counter,
                    .observed_before_us=before,.observed_after_us=s->time,.kind=A733_I2S_CAPTURED_COPY};
                ++s->raw_next;s->snapshot_state=DONE;
            }
        }
    }
    s->deadline=0;return s->status;
}
enum a733_i2s_status a733_i2s_stream_poll(struct a733_i2s_stream *s)
{
    if(!valid(s))return s?s->status:A733_I2S_INVALID;
    if(s->status==A733_I2S_OPENING) {
        if(!now(s))return fault(s);
        if(s->stage==ACQUIRE) {
            enum a733_i2s_resource_result r=IO(s).acquire_poll(COOKIE(s),s->deadline);
            if(!now(s) || r==A733_I2S_RESOURCE_RETAINED)return fault(s);
            if(r==A733_I2S_RESOURCE_PENDING || r==A733_I2S_RESOURCE_BUSY)return s->status;
            if(r!=A733_I2S_RESOURCE_READY)return fault(s);
            s->stage=CONFIGURE;
        }
        return configure(s);
    }
    if(s->status==A733_I2S_CLOSING) {
        if(!now(s))return fault(s);
        enum a733_i2s_resource_result r=IO(s).release(COOKIE(s),s->deadline);
        if(!now(s) || r==A733_I2S_RESOURCE_RETAINED)return fault(s);
        if(r==A733_I2S_RESOURCE_PENDING || r==A733_I2S_RESOURCE_BUSY)return s->status;
        if(r!=A733_I2S_RESOURCE_READY)return fault(s);
        s->platform_held=0;
        if(s->peer){s->peer->platform_held=0;s->peer->peer=NULL;s->peer=NULL;}
        s->status=A733_I2S_CLOSED;return s->status;
    }
    if(s->status!=A733_I2S_PREPARING && s->status!=A733_I2S_ACTIVE && s->status!=A733_I2S_STOPPING)
        return s->status;
    int h=health(s);if(h<0)return fault(s);if(!h)return s->status;
    if(s->stage==FLUSH) {
        uint32_t fc;if(!rd(s,FCTL,&fc))return fault(s);
        if(fc&flush(s))return s->status;
        enum a733_i2s_status r=arm(s);if(r!=A733_I2S_RECOVERY)s->deadline=0;return r;
    }
    if(s->stage==STOP) {
        if(a733_dma_owns(s->dma,&s->active)) {
            enum a733_dma_control_status d=a733_dma_poll(s->dma,s->channel);
            if(!now(s) || d==A733_DMA_RECOVERY)return fault(s);
            if(d==A733_DMA_STOPPING){s->deadline=0;return s->status;}
            if(d!=A733_DMA_COMPLETE && d!=A733_DMA_CANCELED && d!=A733_DMA_FAILED)return fault(s);
        }
        if(s->profile.cyclic && s->cyclic_started) {
            if(!cyclic_frames(s))return fault(s);
            for(unsigned i=0;i<s->count;++i)if(s->slots[i].state==DEVICE)s->slots[i].state=FREE;
        }
        if(!control(s,0))return fault(s);
        if(s->current<s->count)s->slots[s->current].state=FREE;
        s->current=A733_I2S_STREAM_PERIODS;s->deadline=0;return s->status=s->outcome;
    }
    if(s->stage==EOF_FIFO){
        uint32_t status,fs;
        if(s->time>=s->fifo_deadline||a733_dma_owns(s->dma,&s->active))return fault(s);
        if(!rd(s,ISTA,&status)||!rd(s,FSTA,&fs)||!now(s))return fault(s);
        if((status&errors(s))||s->time>=s->fifo_deadline)return fault(s);
        unsigned free=(fs>>16)&255;if(free>128)return fault(s);
        s->deadline=0;
        if(free==128)return s->status=A733_I2S_INPUT_ENDED;
        return s->status;
    }
    if(s->stage==ACTIVE) {
        uint32_t status,fs;
        if(!rd(s,ISTA,&status) || !rd(s,FSTA,&fs))return fault(s);
        if(status&errors(s))return stop(s,A733_I2S_FAILED);
        if(s->priming && ((fs>>16)&255)<128) {
            if(!control(s,1|4))return fault(s);
            s->priming=0;
        }
        if(s->profile.cyclic)return cyclic_active(s);
        struct a733_dma_channel *channel=&s->dma->channels[s->channel];
        if(channel->transfer!=&s->active)return fault(s);
        uint32_t pending=s->dma->operations.read(s->dma->cookie,s->dma->base+0x138+s->channel*0x40);
        uint64_t ticks=s->dma->operations.ticks(s->dma->cookie);
        if(!now(s))return fault(s);
        if(!(pending&~3u) && ticks-channel->started<channel->timeout){s->deadline=0;return s->status;}
        if((pending&~7u) || ticks-channel->started>=channel->timeout)return stop(s,A733_I2S_FAILED);
        if(!requests_off(s))return fault(s);
        /* Occupancy sampled after DRQ is disabled. One sample is withheld as
         * margin; status flags still govern actual FIFO discontinuities. */
        uint64_t before=s->time;
        if(!rd(s,FSTA,&fs))return fault(s);
        unsigned available=tx(s)?128-((fs>>16)&255):64-(fs&127);
        if(available>128)return stop(s,A733_I2S_FAILED);
        if(s->input_ended&&s->current==s->end_slot){
            s->bridge_deadline=0;s->slots[s->current].result.observed_before_us=before;s->stage=DRAIN;
        }else{
        if(available<2)return stop(s,A733_I2S_FAILED);
        uint64_t us=((uint64_t)(available-1)*1000000)/(s->profile.format.rate_hz*s->profile.format.channels);
        if(!us || before>UINT64_MAX-us)return stop(s,A733_I2S_FAILED);
        s->bridge_deadline=before+us;s->slots[s->current].result.observed_before_us=before;
        s->stage=DRAIN;
        }
    }
    if(s->stage==DRAIN) {
        enum a733_dma_control_status d=a733_dma_poll(s->dma,s->channel);
        if(!now(s) || d==A733_DMA_RECOVERY)return fault(s);
        if(d==A733_DMA_STOPPING) {
            if(s->bridge_deadline&&s->time>=s->bridge_deadline)return stop(s,A733_I2S_FAILED);
            s->deadline=0;return s->status;
        }
        if(d!=A733_DMA_COMPLETE || a733_dma_owns(s->dma,&s->active))return stop(s,A733_I2S_FAILED);
        s->stage=PUBLISH;
    }
    enum a733_i2s_status r=publish(s);
    if(r!=A733_I2S_RECOVERY && s->stage!=PUBLISH)s->deadline=0;
    return r;
}
enum a733_i2s_status a733_i2s_stream_cancel(struct a733_i2s_stream *s)
{
    if(!valid(s))return s?s->status:A733_I2S_INVALID;
    if(s->status!=A733_I2S_PREPARING && s->status!=A733_I2S_ACTIVE && s->status!=A733_I2S_READY && s->status!=A733_I2S_INPUT_ENDED)return s->status;
    int h=health(s);if(h<0)return fault(s);if(!h)return A733_I2S_CONFLICT;
    enum a733_i2s_status r=stop(s,A733_I2S_CANCELED);
    if(r!=A733_I2S_RECOVERY)s->deadline=0;
    return r;
}
int a733_i2s_stream_get(struct a733_i2s_stream *s,struct a733_i2s_period *out)
{
    if(!valid(s) || !out)return -1;
    if(s->status!=A733_I2S_READY && s->status!=A733_I2S_ACTIVE &&
        s->status!=A733_I2S_FAILED && s->status!=A733_I2S_CANCELED && s->status!=A733_I2S_INPUT_ENDED)return 0;
    int h=health(s);if(h<0){fault(s);return -1;}if(!h)return 0;
    if(s->profile.cyclic && s->status!=A733_I2S_READY) {
        if(tx(s)) {
            uint64_t delta=s->frames-s->reported;
            if(!delta){s->deadline=0;return 0;}
            if(delta>UINT32_MAX || s->generation==UINT64_MAX){fault(s);return -1;}
            struct a733_i2s_period p={.ticket=++s->generation,.first_frame=s->reported,
                .frames=(unsigned)delta,.slot=0,.kind=A733_I2S_TX_REPEAT_PROGRESS};
            s->reported=s->frames;*out=p;s->deadline=0;return 1;
        }
        if(s->snapshot_state!=DONE){s->deadline=0;return 0;}
        if(s->generation==UINT64_MAX){fault(s);return -1;}
        s->snapshot_result.ticket=++s->generation;s->snapshot_state=CLIENT;
        *out=s->snapshot_result;s->deadline=0;return 1;
    }
    unsigned selected=s->count;
    for(unsigned i=0;i<s->count;++i)if(s->slots[i].state==DONE &&
        (selected==s->count || s->slots[i].result.first_frame<s->slots[selected].result.first_frame))selected=i;
    if(selected==s->count && tx(s) && s->status==A733_I2S_READY)
        for(unsigned i=0;i<s->count;++i)if(s->slots[i].state==FREE){selected=i;break;}
    if(selected<s->count) {
        unsigned i=selected;
        struct a733_i2s_stream_slot *slot=&s->slots[i];
        if(slot->state!=DONE && !(tx(s) && slot->state==FREE && s->status==A733_I2S_READY))return 0;
        if(s->generation==UINT64_MAX){fault(s);return -1;}
        if(!now(s)){fault(s);return -1;}
        struct a733_i2s_period result=slot->result;
        if(slot->state==FREE)result=(struct a733_i2s_period){.slot=i,.kind=A733_I2S_TX_FILL,
            .frames=s->catalogue->segments[i].bytes/s->plan.frame_bytes};
        result.ticket=++s->generation;slot->result=result;slot->state=CLIENT;
        *out=result;s->deadline=0;return 1;
    }
    s->deadline=0;return 0;
}
int a733_i2s_stream_copy(const struct a733_i2s_stream *s,uint64_t ticket,void *out,size_t bytes)
{
    if(!s || s->self!=s || !out || !s->profile.cyclic || tx(s) ||
       s->snapshot_state!=CLIENT || s->snapshot_result.ticket!=ticket || bytes!=4096)return 0;
    unsigned char *p=out;for(size_t i=0;i<bytes;++i)p[i]=s->snapshot[i];return 1;
}
int a733_i2s_stream_put(struct a733_i2s_stream *s,uint64_t ticket)
{
    if(!valid(s) || !ticket)return -1;
    if(s->profile.cyclic && !tx(s) && s->snapshot_state==CLIENT && s->snapshot_result.ticket==ticket) {
        s->snapshot_state=FREE;return 1;
    }
    for(unsigned i=0;i<s->count;++i)if(s->slots[i].state==CLIENT && s->slots[i].result.ticket==ticket) {
        s->slots[i].state=tx(s) && !s->input_ended && (s->status==A733_I2S_READY || s->status==A733_I2S_ACTIVE ||
            s->status==A733_I2S_PREPARING)?QUEUED:FREE;return 1;
    }
    return 0;
}
int a733_i2s_stream_end(struct a733_i2s_stream *s,uint64_t ticket,uint64_t frames)
{
    if(!valid(s)||!ticket||!tx(s)||s->profile.cyclic||s->input_ended||!frames||
       !s->plan.frame_bytes||frames>UINT32_MAX/s->plan.frame_bytes||
       (s->status!=A733_I2S_READY&&s->status!=A733_I2S_ACTIVE&&s->status!=A733_I2S_PREPARING))return 0;
    unsigned slot=s->count;
    for(unsigned i=0;i<s->count;++i)if(s->slots[i].state==CLIENT&&s->slots[i].result.ticket==ticket){slot=i;break;}
    if(slot==s->count||frames>s->catalogue->segments[slot].bytes/s->plan.frame_bytes)return 0;
    /* Commit the tail of the currently queued logical prefix, never abandon
     * a later queued payload or allow an uncommitted hole before the tail. */
    unsigned first=s->current<s->count?(s->current+1)%s->count:s->next;
    unsigned i=first;
    while(i!=slot){if(s->slots[i].state!=QUEUED)return 0;i=(i+1)%s->count;}
    i=(slot+1)%s->count;
    unsigned stop=s->current<s->count?s->current:first;
    while(i!=stop){if(s->slots[i].state==QUEUED||s->slots[i].state==DEVICE)return 0;i=(i+1)%s->count;}
    s->input_ended=1;s->end_slot=slot;s->end_frames=(unsigned)frames;s->slots[slot].state=QUEUED;return 1;
}
int a733_i2s_stream_reap(struct a733_i2s_stream *s)
{
    if(!valid(s) || (s->status!=A733_I2S_FAILED && s->status!=A733_I2S_CANCELED) ||
        a733_dma_owns(s->dma,&s->active))return 0;
    if(s->snapshot_state==CLIENT)return 0;
    for(unsigned i=0;i<s->count;++i)if(s->slots[i].state==CLIENT || s->slots[i].state==DEVICE)return 0;
    for(unsigned i=0;i<s->count;++i)s->slots[i].state=FREE;
    /* COMPLETE permits close, not another START on this open. */
    s->snapshot_state=FREE;s->status=A733_I2S_COMPLETE;return 1;
}
enum a733_i2s_status a733_i2s_stream_close(struct a733_i2s_stream *s,uint64_t deadline)
{
    if(!valid(s) || (s->status!=A733_I2S_READY && s->status!=A733_I2S_COMPLETE))return A733_I2S_INVALID;
    for(unsigned i=0;i<s->count;++i)if(s->slots[i].state!=FREE)return A733_I2S_CONFLICT;
    s->deadline=deadline;int h=health(s);if(h<0)return fault(s);if(!h)return A733_I2S_CONFLICT;
    if(!requests_off(s) || !control(s,0))return fault(s);
    if(a733_dma_release(s->dma,s->channel)!=A733_DMA_CONTROL_OK)return fault(s);
    s->dma_held=0;if(!now(s))return fault(s);
    if(s->peer && s->peer->status!=A733_I2S_CLOSED){
        /* The peer still owns the shared local clock/pin binding. */
        s->deadline=0;return s->status=A733_I2S_CLOSED;
    }
    s->status=A733_I2S_CLOSING;return a733_i2s_stream_poll(s);
}
