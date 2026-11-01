#include "retire.h"
enum { STATUS,CANCEL,PUT,REAP,CLOSE,CLOSING,DONE };
static int request(struct i2s_retirement *s,enum dma_service_operation op,uint64_t ticket)
{
    if(!s->pending){
        const struct dma_service_request r={op,s->job,op==DMA_SERVICE_CLOSE?100000:0,ticket};
        if(!dma_service_send(s->service,&r,0,&s->pending))return 0;
    }
    if(!dma_service_collect(s->service,s->pending,&s->reply,0))return 0;
    s->pending=0;
    if(s->reply.result==DMA_SERVICE_BUSY)return 0;
    return s->reply.result==DMA_SERVICE_OK?1:-1;
}
int i2s_retirement_step(struct i2s_retirement *s)
{
    if(!s||!s->service||!s->jobs||s->jobs>2||s->failed)return -1;
    if(s->stage==DONE)return 1;
    if(s->job>=s->jobs)return -1;
    int r;
    switch(s->stage){
    case STATUS:
        r=request(s,DMA_SERVICE_STATUS,0);if(r<=0)return r;
        if(s->reply.phase==DMA_SERVICE_CLOSED){s->stage=CLOSING;break;}
        if(s->reply.phase==DMA_SERVICE_OPENING||s->reply.phase==DMA_SERVICE_DRAINING)return 0;
        /* INPUT_ENDED still owns the serializer. Explicit CANCEL may discard
         * queued output; memory consumption alone cannot authorize close. */
        if(s->reply.phase==DMA_SERVICE_READY||s->reply.phase==DMA_SERVICE_RUNNING||
           (s->reply.phase==DMA_SERVICE_RESULT&&s->reply.outcome==DMA_SERVICE_COMPLETE))s->stage=CANCEL;
        else if(s->reply.phase==DMA_SERVICE_RESULT&&s->reply.holds_buffers&&
                (s->reply.outcome==DMA_SERVICE_CANCELED||s->reply.outcome==DMA_SERVICE_FAILED))s->stage=PUT;
        else return -1;
        break;
    case CANCEL:
        r=request(s,DMA_SERVICE_CANCEL,0);if(r<0)return -1;
        if(!r&&s->pending)return 0;s->stage=STATUS;break;
    case PUT:
        if(s->held[s->job]){
            r=request(s,DMA_SERVICE_PERIOD_PUT,s->held[s->job]);if(r<=0)return r;
            s->held[s->job]=0;
        }
        s->stage=REAP;break;
    case REAP:
        r=request(s,DMA_SERVICE_REAP,0);if(r<=0)return r;s->stage=CLOSE;break;
    case CLOSE:
        r=request(s,DMA_SERVICE_CLOSE,0);if(r<=0)return r;s->stage=CLOSING;break;
    case CLOSING:
        r=request(s,DMA_SERVICE_STATUS,0);if(r<=0)return r;
        if(s->reply.phase==DMA_SERVICE_CLOSING)return 0;
        if(s->reply.phase!=DMA_SERVICE_CLOSED||s->reply.holds_buffers||s->reply.owns_session)return -1;
        s->stage=++s->job==s->jobs?DONE:STATUS;break;
    default:return -1;
    }
    return s->stage==DONE?1:0;
}
