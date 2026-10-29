#include "dma_service_i2s_stream.h"
#include "a733_range.h"
static int deadline(const struct a733_i2s_stream_profile *p,uint64_t duration,uint64_t *out)
{
    uint64_t now;
    if(!duration || duration>INT64_MAX || !p->operations.io.now_us(p->cookie,&now))return 0;
    return a733_deadline_after(now,duration,out);
}
enum a733_i2s_status dma_service_i2s_stream_open(struct a733_i2s_stream *s,
    const struct dma_service_i2s_stream_profile *p,const struct a733_dma_transfer *t,
    struct a733_dma_controller *dma,unsigned channel,uint64_t duration)
{
    uint64_t until;
    if(!p || !deadline(&p->stream,duration,&until))return A733_I2S_CONFLICT;
    return a733_i2s_stream_open(s,&p->stream,t,dma,channel,until);
}
enum a733_i2s_status dma_service_i2s_stream_close(struct a733_i2s_stream *s,uint64_t duration)
{
    uint64_t until;
    if(!s)return A733_I2S_INVALID;
    if(!deadline(&s->profile,duration,&until)){
        if(s->peer)s->peer->status=A733_I2S_RECOVERY;
        return s->status=A733_I2S_RECOVERY;
    }
    return a733_i2s_stream_close(s,until);
}

int dma_service_i2s_stream_pair(const struct dma_service_i2s_stream_profile *a,
    const struct dma_service_i2s_stream_profile *b)
{
    if(!a || !b || a->stream.route!=A733_I2S_ROUTE_I2S0 || b->stream.route!=A733_I2S_ROUTE_I2S0 || a->duplex!=1 || b->duplex!=1 ||
       a->stream.direction==b->stream.direction || a->stream.cookie!=b->stream.cookie ||
       a->stream.operation_us!=b->stream.operation_us)return 0;
    const struct a733_i2s_format *x=&a->stream.format,*y=&b->stream.format;
    const struct a733_i2s_stream_operations *p=&a->stream.operations,*q=&b->stream.operations;
    return x->module_hz==y->module_hz && x->rate_hz==y->rate_hz && x->mclk_hz==y->mclk_hz &&
        x->container_bits==y->container_bits && x->slot_bits==y->slot_bits &&
        x->channels==y->channels && x->first_slot==y->first_slot &&
        p->health==q->health && p->io.acquire==q->io.acquire && p->io.acquire_poll==q->io.acquire_poll &&
        p->io.release==q->io.release && p->io.now_us==q->io.now_us &&
        p->io.read==q->io.read && p->io.write==q->io.write;
}
enum a733_i2s_status dma_service_i2s_stream_duplex_open(struct a733_i2s_stream *s,
    const struct dma_service_i2s_stream_profile *p,const struct a733_dma_transfer *t,
    struct a733_dma_controller *dma,unsigned channel,uint64_t duration,struct a733_i2s_stream *peer)
{
    uint64_t until;
    if(!p || p->duplex!=1 || !deadline(&p->stream,duration,&until))return A733_I2S_CONFLICT;
    return a733_i2s_stream_duplex_open(s,&p->stream,t,dma,channel,until,peer);
}
