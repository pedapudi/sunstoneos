#include "dma_service_i2s.h"

int dma_service_i2s_bind(struct dma_service *s,const struct dma_service_job *jobs,unsigned count)
{
    unsigned next=0;
    for(unsigned i=0;i<A733_DMA_CHANNELS;++i)s->channels[i].stream=DMA_SERVICE_STREAMS;
    for(unsigned i=0;i<count;++i) {
        struct dma_service_channel *c=&s->channels[jobs[i].channel];
        if(jobs[i].kind!=DMA_SERVICE_I2S_STREAM || c->stream!=DMA_SERVICE_STREAMS)continue;
        if(next==DMA_SERVICE_STREAMS)return 0;
        c->stream=next++;
    }
    return 1;
}
struct a733_i2s_stream *dma_service_i2s_stream(struct dma_service *s,unsigned channel)
{
    unsigned i=s->channels[channel].stream;
    return i<DMA_SERVICE_STREAMS?&s->streams[i]:NULL;
}
