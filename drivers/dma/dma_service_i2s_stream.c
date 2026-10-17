#include "dma_service_i2s_stream.h"
static int deadline(const struct a733_i2s_stream_profile *p,uint64_t duration,uint64_t *out)
{
    uint64_t now;
    if(!duration || duration>INT64_MAX || !p->operations.io.now_us(p->cookie,&now) ||
        now>UINT64_MAX-duration)return 0;
    *out=now+duration;return 1;
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
    if(!deadline(&s->profile,duration,&until))return s->status=A733_I2S_RECOVERY;
    return a733_i2s_stream_close(s,until);
}
