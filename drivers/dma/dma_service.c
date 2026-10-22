#include "dma_service_i2s.h"
#include <string.h>

static struct dma_service *instance;
static unsigned creating_worker;
enum { SERVICE_ACTIVE, SERVICE_WAITING, SERVICE_CHECKING, SERVICE_RETAINED };
static void worker(void *);
BaseType_t dma_service_task_privileged(TaskFunction_t entry, void *argument)
{ return creating_worker && entry == worker && argument == instance ? pdTRUE : pdFALSE; }
static int duration(uint64_t value) { return value && value <= INT64_MAX; }
static int mapping_valid(const struct a733_dma_mapping *m, const struct dma_service *s)
{
    return a733_dma_mapping_shape(m) && (unsigned)m->access <= A733_DMA_DESCRIPTORS &&
        !a733_extents_overlap_prechecked((uintptr_t)m->cpu,m->bytes,(uintptr_t)s,sizeof(*s));
}
static int conflict(const struct a733_dma_transfer *a,const struct a733_dma_transfer *b,int exclusive)
{
    if (a733_dma_mappings_overlap(&a->descriptors,&b->descriptors)) return 1;
    for (size_t i=0;i<a->mapping_count;++i) {
        if (a733_dma_mappings_overlap(&a->mappings[i],&b->descriptors)) return 1;
        for (size_t j=0;j<b->mapping_count;++j)
            if ((exclusive || a->mappings[i].access != A733_DMA_READ || b->mappings[j].access != A733_DMA_READ) &&
                a733_dma_mappings_overlap(&a->mappings[i],&b->mappings[j])) return 1;
    }
    for (size_t j=0;j<b->mapping_count;++j)
        if (a733_dma_mappings_overlap(&a->descriptors,&b->mappings[j])) return 1;
    return 0;
}
static int job_valid(const struct dma_service_job *j,const struct dma_service *s,uint16_t channels)
{
    const struct a733_dma_transfer *t=&j->transfer;
    if ((j->kind != DMA_SERVICE_RAM && j->kind != DMA_SERVICE_I2S_STREAM && j->kind != DMA_SERVICE_FIFO) || j->channel >= A733_DMA_CHANNELS ||
        !(channels & (1u<<j->channel)) || !t->segments || !t->mappings ||
        !t->count || t->count > DMA_SERVICE_SEGMENTS ||
        !t->mapping_count || t->mapping_count > DMA_SERVICE_MAPPINGS ||
        (uintptr_t)t->segments % _Alignof(struct a733_dma_segment) ||
        (uintptr_t)t->mappings % _Alignof(struct a733_dma_mapping) ||
        (uintptr_t)t->segments > UINTPTR_MAX-t->count*sizeof(*t->segments) ||
        (uintptr_t)t->mappings > UINTPTR_MAX-t->mapping_count*sizeof(*t->mappings) ||
        !mapping_valid(&t->descriptors,s) || t->descriptors.access != A733_DMA_DESCRIPTORS ||
        t->descriptors.bytes/64 < t->count) return 0;
    if (j->kind == DMA_SERVICE_I2S_STREAM ?
        (!j->stream || sizeof(*j->stream)>UINTPTR_MAX-(uintptr_t)j->stream ||
         a733_extents_overlap_prechecked((uintptr_t)j->stream,sizeof(*j->stream),(uintptr_t)s,sizeof(*s)) ||
         !a733_i2s_stream_valid(&j->stream->stream,t)) : j->stream != NULL) return 0;
    for (size_t i=0;i<t->mapping_count;++i)
        if (!mapping_valid(&t->mappings[i],s) || t->mappings[i].access == A733_DMA_DESCRIPTORS) return 0;
    for (size_t i=0;i<t->count;++i) {
        const struct a733_dma_segment *segment=&t->segments[i];
        if (!segment->bytes) return 0;
        if (j->kind == DMA_SERVICE_RAM && (segment->source.request || segment->destination.request)) return 0;
        if(j->kind==DMA_SERVICE_FIFO){
            const struct a733_dma_segment *first=&t->segments[0];
            if(!!segment->source.request==!!segment->destination.request ||
               !!segment->source.request!=!!first->source.request)return 0;
            const struct a733_dma_endpoint *fifo=segment->source.request?&segment->source:&segment->destination;
            const struct a733_dma_endpoint *initial=first->source.request?&first->source:&first->destination;
            if(fifo->address!=initial->address || fifo->request!=initial->request ||
               fifo->width!=initial->width || fifo->burst!=initial->burst)return 0;
        }
    }
    return 1;
}
static int progressing(enum dma_service_phase phase)
{ return phase == DMA_SERVICE_OPENING || phase == DMA_SERVICE_RUNNING ||
         phase == DMA_SERVICE_DRAINING || phase == DMA_SERVICE_CLOSING; }
static void closed(struct dma_service_channel *c)
{
    c->owner=DMA_SERVICE_CLIENTS; c->profile=c->job=DMA_SERVICE_NO_JOB;
    c->phase=DMA_SERVICE_CLOSED; c->job_ticket=0; c->outcome=DMA_SERVICE_NO_RESULT;
}
static void dma_state(struct dma_service_channel *c,enum a733_dma_control_status status)
{
    switch (status) {
    case A733_DMA_ACTIVE: c->phase=DMA_SERVICE_RUNNING; break;
    case A733_DMA_STOPPING: c->phase=DMA_SERVICE_DRAINING; break;
    case A733_DMA_COMPLETE: c->phase=DMA_SERVICE_RESULT; c->outcome=DMA_SERVICE_COMPLETE; break;
    case A733_DMA_CANCELED: c->phase=DMA_SERVICE_RESULT; c->outcome=DMA_SERVICE_CANCELED; break;
    case A733_DMA_FAILED: c->phase=DMA_SERVICE_RESULT; c->outcome=DMA_SERVICE_FAILED; break;
    default: c->phase=DMA_SERVICE_RECOVERY; break;
    }
}
static void stream_state(struct dma_service_channel *c,const struct a733_i2s_stream *st)
{
    switch(st->status) {
    case A733_I2S_CLOSED: c->holds_buffers=0;closed(c);break;
    case A733_I2S_OPENING:c->phase=DMA_SERVICE_OPENING;break;
    case A733_I2S_READY:case A733_I2S_COMPLETE:c->phase=DMA_SERVICE_READY;break;
    case A733_I2S_PREPARING:case A733_I2S_ACTIVE:c->phase=DMA_SERVICE_RUNNING;break;
    case A733_I2S_STOPPING:c->phase=DMA_SERVICE_DRAINING;break;
    case A733_I2S_CLOSING:c->phase=DMA_SERVICE_CLOSING;break;
    case A733_I2S_INPUT_ENDED:c->phase=DMA_SERVICE_RESULT;c->outcome=DMA_SERVICE_COMPLETE;break;
    case A733_I2S_CANCELED:c->phase=DMA_SERVICE_RESULT;c->outcome=DMA_SERVICE_CANCELED;break;
    case A733_I2S_FAILED:c->phase=DMA_SERVICE_RESULT;c->outcome=DMA_SERVICE_FAILED;break;
    default:c->phase=DMA_SERVICE_RECOVERY;break;
    }
}
/* A stream that faulted outside its own poll reports recovery to the service. */
static void refresh_stream(struct dma_service *s,unsigned channel)
{
    struct a733_i2s_stream *st=dma_service_i2s_stream(s,channel);
    if(st && st->self==st && st->status==A733_I2S_RECOVERY)stream_state(&s->channels[channel],st);
}
/* Advance one channel; return whether it still needs polling. */
static unsigned poll_channel(struct dma_service *s,unsigned i)
{
    struct dma_service_channel *c=&s->channels[i];
    refresh_stream(s,i);
    if (!progressing(c->phase)) return 0;
    if (s->jobs[c->profile].kind == DMA_SERVICE_I2S_STREAM) {
        /* The stream owns the producer and the DMA stop/drain sequence. */
        struct a733_i2s_stream *st=dma_service_i2s_stream(s,i);
        (void)a733_i2s_stream_poll(st);stream_state(c,st);
    } else dma_state(c,a733_dma_poll(&s->controller,i));
    return progressing(c->phase);
}
enum { IRQ_EVENT=1u, IRQ_FAILED=2u };
/* Whether channel i can wait for its next DMA interrupt. Stop drain, I2S
 * register and health operations, TX priming and a copyable RX period all
 * progress without one, so they keep tick polling. */
static int event_driven(struct dma_service *s,unsigned i)
{
    const struct dma_service_channel *c=&s->channels[i];
    if (c->phase != DMA_SERVICE_RUNNING ||
        !a733_dma_event_wait(&s->controller,i,2*s->irq.fallback_ticks)) return 0;
    if (s->jobs[c->profile].kind != DMA_SERVICE_I2S_STREAM) return 1;
    const struct a733_i2s_stream *st=dma_service_i2s_stream(s,i);
    uint64_t packages;
    if (st->status != A733_I2S_ACTIVE || st->deadline || st->priming) return 0;
    if (!st->profile.cyclic || st->profile.direction == A733_I2S_TRANSMIT) return 1;
    /* Receive period raw_next becomes copyable once raw_next+1 completes. */
    return a733_dma_cyclic_progress(&s->controller,i,&packages) && packages < st->raw_next+2;
}
/* Account for ISR notifications, after this pass has acknowledged events. */
static void irq_observe(struct dma_service *s,unsigned pending)
{
    if (pending & IRQ_FAILED) s->irq_retained=1;
    if (!(pending & IRQ_EVENT)) return;
    s->irq_armed=0; /* The ISR masked the SPI. */
    /* Another owner's channel or a retained channel can hold the shared
     * line asserted. Rearming then would only repeat the interrupt. */
    if (s->controller.events == s->irq_events) s->irq_spurious=1;
}
/* Wait while channels progress: up to the fallback with the SPI enabled,
 * otherwise one tick, as without a binding. */
static TickType_t irq_wait(struct dma_service *s,unsigned event_driven_only)
{
    if (!event_driven_only || s->irq_retained) return 1;
    if (s->irq_spurious) {
        if (s->controller.events == s->irq_events) return 1;
        s->irq_spurious=0;
    }
    if (!s->irq_armed) {
        int r=0;
        taskENTER_CRITICAL();
        if (!atomic_load_explicit(&s->irq_pending,memory_order_acquire)) r=s->irq.rearm(s->irq.cookie);
        taskEXIT_CRITICAL();
        if (r < 0) s->irq_retained=1;
        if (r != 1) return 1;
        s->irq_armed=1; s->irq_events=s->controller.events;
    }
    return s->irq.fallback;
}
static enum dma_service_result stream_execute(struct dma_service *s,
    const struct dma_service_message *m,struct a733_i2s_period *period)
{
    const struct dma_service_request *r=&m->request;
    struct dma_service_private_job *j=&s->jobs[r->job];
    struct dma_service_channel *c=&s->channels[j->channel];
    struct a733_i2s_stream *st=dma_service_i2s_stream(s,j->channel);
    enum a733_i2s_status result=A733_I2S_READY;
    if(r->operation==DMA_SERVICE_OPEN) {
        if(c->phase!=DMA_SERVICE_CLOSED || st->platform_held || st->peer)return DMA_SERVICE_BUSY;
        struct a733_i2s_stream *peer=NULL;
        if(j->stream.duplex)for(unsigned i=0;i<s->job_count;++i) {
            if(i==r->job || s->jobs[i].kind!=DMA_SERVICE_I2S_STREAM ||
                !dma_service_i2s_stream_pair(&j->stream,&s->jobs[i].stream))continue;
            struct dma_service_channel *other=&s->channels[s->jobs[i].channel];
            struct a733_i2s_stream *other_stream=dma_service_i2s_stream(s,s->jobs[i].channel);
            if(other_stream->platform_held){
                if(other->owner!=m->client)return DMA_SERVICE_DENIED;
                if(other->phase!=DMA_SERVICE_READY || other_stream->peer)return DMA_SERVICE_BUSY;
                peer=other_stream;
            }
        }
        for(unsigned i=0;i<A733_DMA_CHANNELS;++i)if(s->channels[i].holds_buffers &&
            conflict(&j->transfer,&s->jobs[s->channels[i].job].transfer,1))return DMA_SERVICE_BUSY;
        /* Full catalogue ownership precedes all platform callbacks and client fill. */
        c->owner=m->client;c->profile=c->job=r->job;c->holds_buffers=1;
        /* A clean prior close proves all periods returned; fresh generations
         * must still invalidate stale tickets from earlier sessions. */
        uint64_t generation=st->generation;
        memset(st,0,sizeof(*st));
        result=j->stream.duplex?
            dma_service_i2s_stream_duplex_open(st,&j->stream,&j->transfer,
                &s->controller,j->channel,r->timeout,peer):
            dma_service_i2s_stream_open(st,&j->stream,&j->transfer,
                &s->controller,j->channel,r->timeout);
        st->generation=generation;
        if(result==A733_I2S_CONFLICT || result==A733_I2S_INVALID) {
            c->holds_buffers=0;closed(c);return result==A733_I2S_CONFLICT?DMA_SERVICE_BUSY:DMA_SERVICE_INVALID;
        }
    } else {
        if(c->owner!=m->client || c->profile!=r->job)return DMA_SERVICE_DENIED;
        if(c->phase==DMA_SERVICE_RECOVERY)return DMA_SERVICE_FAULT;
        switch(r->operation) {
        case DMA_SERVICE_SUBMIT:
            if(c->phase!=DMA_SERVICE_READY || st->status!=A733_I2S_READY)return DMA_SERVICE_BUSY;
            result=a733_i2s_stream_start(st,r->timeout);
            if(result!=A733_I2S_INVALID && result!=A733_I2S_CONFLICT)c->job_ticket=m->ticket;
            break;
        case DMA_SERVICE_CANCEL:
            if(c->phase!=DMA_SERVICE_RUNNING && c->phase!=DMA_SERVICE_READY&&st->status!=A733_I2S_INPUT_ENDED)return DMA_SERVICE_BUSY;
            result=a733_i2s_stream_cancel(st);break;
        case DMA_SERVICE_PERIOD_GET: {
            int got=a733_i2s_stream_get(st,period);
            stream_state(c,st);return got==1?DMA_SERVICE_OK:got==0?DMA_SERVICE_BUSY:DMA_SERVICE_FAULT;
        }
        case DMA_SERVICE_PERIOD_PUT: {
            int put=a733_i2s_stream_put(st,r->period_ticket);
            stream_state(c,st);return put==1?DMA_SERVICE_OK:put==0?DMA_SERVICE_INVALID:DMA_SERVICE_FAULT;
        }
        case DMA_SERVICE_PERIOD_END: {
            int put=a733_i2s_stream_end(st,r->period_ticket,r->timeout);
            stream_state(c,st);return put==1?DMA_SERVICE_OK:DMA_SERVICE_INVALID;
        }
        case DMA_SERVICE_REAP:
            if(!a733_i2s_stream_reap(st))return DMA_SERVICE_BUSY;
            break;
        case DMA_SERVICE_CLOSE:
            if(c->phase!=DMA_SERVICE_READY)return DMA_SERVICE_BUSY;
            result=dma_service_i2s_stream_close(st,r->timeout);break;
        default:return DMA_SERVICE_INVALID;
        }
    }
    if(result==A733_I2S_RECOVERY)st->status=A733_I2S_RECOVERY;
    stream_state(c,st);
    if(result==A733_I2S_INVALID)return DMA_SERVICE_INVALID;
    if(result==A733_I2S_CONFLICT)return DMA_SERVICE_BUSY;
    return c->phase==DMA_SERVICE_RECOVERY?DMA_SERVICE_FAULT:DMA_SERVICE_OK;
}
static enum dma_service_result execute(struct dma_service *s,const struct dma_service_message *m,
    struct a733_i2s_period *period)
{
    const struct dma_service_request *r=&m->request;
    if ((unsigned)r->operation > DMA_SERVICE_PERIOD_END || r->job >= s->job_count) return DMA_SERVICE_INVALID;
    if (!(s->clients[m->client].jobs & (UINT32_C(1)<<r->job))) return DMA_SERVICE_DENIED;
    struct dma_service_private_job *j=&s->jobs[r->job];
    struct dma_service_channel *c=&s->channels[j->channel];
    refresh_stream(s,j->channel);
    int timed=r->operation == DMA_SERVICE_SUBMIT ||
        (j->kind == DMA_SERVICE_I2S_STREAM && (r->operation == DMA_SERVICE_OPEN || r->operation == DMA_SERVICE_CLOSE));
    if(r->operation==DMA_SERVICE_PERIOD_END){if(!r->timeout)return DMA_SERVICE_INVALID;}
    else if (timed ? !duration(r->timeout) : r->timeout != 0) return DMA_SERVICE_INVALID;
    if((r->operation==DMA_SERVICE_PERIOD_PUT||r->operation==DMA_SERVICE_PERIOD_END) ? !r->period_ticket : r->period_ticket!=0)return DMA_SERVICE_INVALID;
    if(j->kind!=DMA_SERVICE_I2S_STREAM && r->operation>DMA_SERVICE_CLOSE)return DMA_SERVICE_INVALID;
    if (r->operation == DMA_SERVICE_STATUS)
        return c->phase == DMA_SERVICE_RECOVERY ? DMA_SERVICE_FAULT : DMA_SERVICE_OK;
    if(j->kind==DMA_SERVICE_I2S_STREAM)return stream_execute(s,m,period);
    if (r->operation == DMA_SERVICE_OPEN) {
        if (c->phase != DMA_SERVICE_CLOSED) return DMA_SERVICE_BUSY;
        c->owner=m->client; c->profile=r->job;
        enum a733_dma_control_status d=a733_dma_claim(&s->controller,j->channel);
        if (d != A733_DMA_CONTROL_OK) { closed(c); return DMA_SERVICE_BUSY; }
        c->phase=DMA_SERVICE_READY;
        return DMA_SERVICE_OK;
    }
    if (c->owner != m->client) return DMA_SERVICE_DENIED;
    if (c->phase == DMA_SERVICE_RECOVERY) return DMA_SERVICE_FAULT;
    if (s->jobs[c->profile].kind != j->kind) return DMA_SERVICE_DENIED;
    switch (r->operation) {
    case DMA_SERVICE_SUBMIT: {
        if (c->phase != DMA_SERVICE_READY || c->holds_buffers) return DMA_SERVICE_BUSY;
        for (unsigned i=0;i<A733_DMA_CHANNELS;++i) {
            const struct dma_service_channel *other=&s->channels[i];
            if (other->holds_buffers && conflict(&j->transfer,&s->jobs[other->job].transfer,
                s->jobs[other->job].kind==DMA_SERVICE_I2S_STREAM)) return DMA_SERVICE_BUSY;
        }
        c->holds_buffers=1; c->job=r->job; c->job_ticket=m->ticket;
        enum a733_dma_control_status status=a733_dma_submit(&s->controller,j->channel,&j->transfer,r->timeout);
        if (status == A733_DMA_INVALID || status == A733_DMA_CONFLICT) {
            c->holds_buffers=0; c->job=DMA_SERVICE_NO_JOB; c->job_ticket=0;
            return status == A733_DMA_CONFLICT ? DMA_SERVICE_BUSY : DMA_SERVICE_INVALID;
        }
        dma_state(c,status);
        break;
    }
    case DMA_SERVICE_CANCEL:
        if (c->phase != DMA_SERVICE_RUNNING) return DMA_SERVICE_BUSY;
        dma_state(c,a733_dma_cancel(&s->controller,j->channel));
        break;
    case DMA_SERVICE_REAP:
        if (c->phase != DMA_SERVICE_RESULT || !c->holds_buffers ||
            a733_dma_owns(&s->controller,&s->jobs[c->job].transfer)) return DMA_SERVICE_BUSY;
        /* Reply snapshots the terminal outcome/ticket before worker clears it. */
        c->holds_buffers=0; c->phase=DMA_SERVICE_READY;
        break;
    case DMA_SERVICE_CLOSE:
        if (c->phase != DMA_SERVICE_READY || c->holds_buffers) return DMA_SERVICE_BUSY;
        if (a733_dma_release(&s->controller,j->channel) != A733_DMA_CONTROL_OK) c->phase=DMA_SERVICE_RECOVERY;
        else closed(c);
        break;
    default: return DMA_SERVICE_INVALID;
    }
    return c->phase == DMA_SERVICE_RECOVERY ? DMA_SERVICE_FAULT : DMA_SERVICE_OK;
}
static void worker(void *argument)
{
    struct dma_service *s=argument;
    uint64_t level,stack;
    __asm__ volatile("mrs %0,CurrentEL\n mrs %1,spsel" : "=r"(level),"=r"(stack));
    configASSERT(level == 4);
#if configNUMBER_OF_CORES > 1
    configASSERT(stack == 0);
#else
    (void)stack;
#endif
    for (;;) {
        if (atomic_load_explicit(&s->activation_state,memory_order_acquire)!=SERVICE_ACTIVE) {
            (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
            continue;
        }
        unsigned pending=s->irq_bound ? atomic_exchange_explicit(&s->irq_pending,0,memory_order_acq_rel) : 0;
        unsigned active=0,event_driven_only=1;
        for (unsigned i=0;i<A733_DMA_CHANNELS;++i)
            if (poll_channel(s,i)) { active=1; if (s->irq_bound && !event_driven(s,i)) event_driven_only=0; }
        struct dma_service_message message;
        if (!s->irq_bound) {
            if (xQueueReceive(s->requests,&message,active ? 1 : portMAX_DELAY) != pdTRUE) continue;
        } else {
            /* Clients and the ISR both notify, so requests are taken without
             * blocking on the queue. */
            irq_observe(s,pending);
            if (xQueueReceive(s->requests,&message,0) != pdTRUE) {
                (void)ulTaskNotifyTake(pdTRUE,active ? irq_wait(s,event_driven_only) : portMAX_DELAY);
                continue;
            }
        }
        configASSERT(message.client < DMA_SERVICE_CLIENTS && message.ticket);
        /* A transfer can finish while the worker blocks. Observe it before
         * answering, so the reply needs no further worker tick. */
        enum dma_service_operation op=message.request.operation;
        if (message.request.job < s->job_count &&
            (op == DMA_SERVICE_STATUS || op == DMA_SERVICE_PERIOD_GET || op == DMA_SERVICE_REAP))
            (void)poll_channel(s,s->jobs[message.request.job].channel);
        struct dma_service_reply reply={.ticket=message.ticket};
        reply.result=execute(s,&message,&reply.period);
        if (message.request.job < s->job_count &&
            (s->clients[message.client].jobs & (UINT32_C(1)<<message.request.job))) {
            struct dma_service_channel *c=&s->channels[s->jobs[message.request.job].channel];
            reply.phase=c->phase; reply.outcome=c->outcome; reply.job_ticket=c->job_ticket;
            reply.owns_session=c->owner == message.client; reply.holds_buffers=c->holds_buffers;
            const struct a733_i2s_stream *st=dma_service_i2s_stream(s,s->jobs[message.request.job].channel);
            if(s->jobs[message.request.job].kind==DMA_SERVICE_I2S_STREAM && st->profile.cyclic)
                reply.stream_frames=st->frames;
            if (message.request.operation == DMA_SERVICE_REAP && reply.result == DMA_SERVICE_OK) {
                if(s->jobs[message.request.job].kind!=DMA_SERVICE_I2S_STREAM)c->job=DMA_SERVICE_NO_JOB;
                c->job_ticket=0; c->outcome=DMA_SERVICE_NO_RESULT;
            }
        }
        configASSERT(xQueueSend(s->clients[message.client].replies,&reply,0) == pdTRUE);
    }
}
static int initialize(struct dma_service *s,const struct a733_dma_operations *ops,void *cookie,
    uintptr_t base,uint16_t channels,uint64_t drain,const struct dma_service_job *jobs,
    unsigned count,const struct dma_service_protected *protected,unsigned protected_count,
    UBaseType_t priority,UBaseType_t affinity,const struct dma_service_activation *activation)
{
    struct dma_service_activation saved_activation={0};
    if (activation) {
        memcpy(&saved_activation,activation,sizeof(saved_activation));
        if (!saved_activation.bootstrap || !saved_activation.prepared) return 0;
        activation=&saved_activation;
    }
    if (!s || instance || s->initialized || !ops || !jobs || !count || count > DMA_SERVICE_JOBS ||
        !protected || !protected_count || protected_count > 32 ||
        !priority || priority >= configMAX_PRIORITIES || !affinity || (affinity >> configNUMBER_OF_CORES) ||
        base != UINT32_C(0x04601000) || !channels || !duration(drain) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    unsigned covered=0;
    for (unsigned n=0;n<protected_count;++n) {
        const struct dma_service_protected *g=&protected[n];
        if (!g->bytes || g->bytes > UINTPTR_MAX-g->cpu || g->bytes > UINT64_MAX-g->physical) return 0;
        if (g->cpu <= (uintptr_t)s && (uintptr_t)s-g->cpu <= g->bytes &&
            sizeof(*s) <= g->bytes-((uintptr_t)s-g->cpu)) covered=1;
    }
    if (!covered) return 0;
    unsigned i2s_channel=A733_DMA_CHANNELS,i2s_count=0,duplex_count=0;
    for (unsigned i=0;i<count;++i) {
        if (!job_valid(&jobs[i],s,channels)) return 0;
        const struct a733_dma_transfer *t=&jobs[i].transfer;
        for (unsigned n=0;n<protected_count;++n) {
            const struct dma_service_protected *g=&protected[n];
            for (size_t k=0;k<=t->mapping_count;++k) {
                const struct a733_dma_mapping *m=k == t->mapping_count ? &t->descriptors : &t->mappings[k];
                if (a733_extents_overlap_prechecked((uintptr_t)m->cpu,m->bytes,g->cpu,g->bytes) ||
                    a733_extents_overlap_prechecked(m->bus,m->bytes,g->physical,g->bytes)) return 0;
            }
        }
        if (jobs[i].kind == DMA_SERVICE_I2S_STREAM) {
            ++i2s_count;
            if(jobs[i].stream && jobs[i].stream->duplex){
                if(jobs[i].stream->duplex!=1)return 0;
                ++duplex_count;
            } else if(i2s_channel!=A733_DMA_CHANNELS && i2s_channel!=jobs[i].channel)return 0;
            if(!jobs[i].stream || !jobs[i].stream->duplex)i2s_channel=jobs[i].channel;
        }
    }
    if(duplex_count){
        if(duplex_count!=2 || i2s_count!=2)return 0;
        const struct dma_service_job *pair[2];unsigned n=0;
        for(unsigned i=0;i<count;++i)if(jobs[i].kind==DMA_SERVICE_I2S_STREAM && jobs[i].stream->duplex)pair[n++]=&jobs[i];
        if(pair[0]->channel==pair[1]->channel ||
           !dma_service_i2s_stream_pair(pair[0]->stream,pair[1]->stream) ||
           conflict(&pair[0]->transfer,&pair[1]->transfer,1))return 0;
    }
    if (!dma_service_i2s_bind(s,jobs,count) ||
        a733_dma_controller_init(&s->controller,ops,cookie,base,channels,drain) != A733_DMA_CONTROL_OK) return 0;
    for (unsigned i=0;i<count;++i) {
        struct dma_service_private_job *j=&s->jobs[i];
        j->kind=jobs[i].kind; j->channel=jobs[i].channel; j->transfer=jobs[i].transfer;
        for (size_t n=0;n<j->transfer.count;++n) j->segments[n]=jobs[i].transfer.segments[n];
        for (size_t n=0;n<j->transfer.mapping_count;++n) j->mappings[n]=jobs[i].transfer.mappings[n];
        j->transfer.segments=j->segments; j->transfer.mappings=j->mappings;
        if (jobs[i].stream) j->stream=*jobs[i].stream;
    }
    s->job_count=count;
    for (unsigned i=0;i<A733_DMA_CHANNELS;++i) closed(&s->channels[i]);
    s->requests=xQueueCreateStatic(DMA_SERVICE_CLIENTS,sizeof(struct dma_service_message),s->request_storage,&s->request_queue);
    configASSERT(s->requests);
    for (unsigned i=0;i<DMA_SERVICE_CLIENTS;++i) {
        struct dma_service_client *c=&s->clients[i];
        c->replies=xQueueCreateStatic(1,sizeof(struct dma_service_reply),c->storage,&c->queue);
        configASSERT(c->replies);
    }
    s->activation=activation ? *activation : (struct dma_service_activation){0};
    atomic_init(&s->activation_state,activation ? SERVICE_WAITING : SERVICE_ACTIVE);
    atomic_init(&s->irq_pending,0);
    s->initialized=1; instance=s; creating_worker=1;
#if configNUMBER_OF_CORES > 1
    s->worker=xTaskCreateStaticAffinitySet(worker,"dma-service",DMA_SERVICE_STACK_WORDS,s,priority,
        s->worker_stack,&s->worker_state,affinity);
#else
    s->worker=xTaskCreateStatic(worker,"dma-service",DMA_SERVICE_STACK_WORDS,s,priority,s->worker_stack,&s->worker_state);
#endif
    creating_worker=0; configASSERT(s->worker); return 1;
}
int dma_service_init(struct dma_service *s,const struct a733_dma_operations *ops,void *cookie,
    uintptr_t base,uint16_t channels,uint64_t drain,const struct dma_service_job *jobs,
    unsigned count,const struct dma_service_protected *protected,unsigned protected_count,
    UBaseType_t priority,UBaseType_t affinity)
{
    return initialize(s,ops,cookie,base,channels,drain,jobs,count,protected,protected_count,priority,affinity,NULL);
}
int dma_service_init_deferred(struct dma_service *s,const struct a733_dma_operations *ops,void *cookie,
    uintptr_t base,uint16_t channels,uint64_t drain,const struct dma_service_job *jobs,
    unsigned count,const struct dma_service_protected *protected,unsigned protected_count,
    UBaseType_t priority,UBaseType_t affinity,const struct dma_service_activation *activation)
{
    if(!activation) return 0;
    return initialize(s,ops,cookie,base,channels,drain,jobs,count,protected,protected_count,priority,affinity,activation);
}
enum dma_service_result dma_service_activate(struct dma_service *s)
{
    if(!s || s!=instance || !s->initialized || !s->activation.bootstrap ||
        xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle()!=s->activation.bootstrap) return DMA_SERVICE_DENIED;
    uint64_t level;__asm__ volatile("mrs %0,CurrentEL" : "=r"(level));
    if(level!=4) return DMA_SERVICE_DENIED;
    unsigned expected=SERVICE_WAITING;
    if(!atomic_compare_exchange_strong_explicit(&s->activation_state,&expected,SERVICE_CHECKING,
        memory_order_acq_rel,memory_order_acquire))
        return expected==SERVICE_RETAINED ? DMA_SERVICE_FAULT : DMA_SERVICE_BUSY;
    int prepared=s->activation.prepared(s->activation.context,s->controller.base,s->controller.authorized_channels);
    unsigned state=prepared==1 ? SERVICE_ACTIVE : prepared==0 ? SERVICE_WAITING : SERVICE_RETAINED;
    atomic_store_explicit(&s->activation_state,state,memory_order_release);
    if(state==SERVICE_ACTIVE) {xTaskNotifyGive(s->worker);return DMA_SERVICE_OK;}
    return state==SERVICE_WAITING ? DMA_SERVICE_BUSY : DMA_SERVICE_FAULT;
}
int dma_service_bind(struct dma_service *s,unsigned slot,TaskHandle_t task,uint32_t jobs)
{
    if (!s || s != instance || !s->initialized || slot >= DMA_SERVICE_CLIENTS || !task || task == s->worker ||
        s->clients[slot].task || !jobs || (jobs >> s->job_count) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    for (unsigned i=0;i<DMA_SERVICE_CLIENTS;++i) if (s->clients[i].task == task) return 0;
    s->clients[slot].task=task; s->clients[slot].jobs=jobs; return 1;
}
int dma_service_bind_irq(struct dma_service *s,const struct dma_service_irq *irq)
{
    if (!s || s != instance || !s->initialized || s->irq_bound || !irq || !irq->rearm ||
        !irq->cookie || !irq->fallback || irq->fallback > configTICK_RATE_HZ/4 ||
        !irq->fallback_ticks || irq->fallback_ticks > INT64_MAX/2 ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    s->irq=*irq; s->irq_bound=1; return 1;
}
void dma_service_irq_notify_from_isr(struct dma_service *s,int mask_ok,BaseType_t *woken)
{
    configASSERT(s && s == instance && s->initialized && s->irq_bound && woken);
    atomic_fetch_or_explicit(&s->irq_pending,IRQ_EVENT|(mask_ok == 1 ? 0 : IRQ_FAILED),memory_order_release);
    vTaskNotifyGiveFromISR(s->worker,woken);
}
static struct dma_service_client *caller(struct dma_service *s,unsigned *slot)
{
    if (!s || s != instance || !s->initialized ||
        atomic_load_explicit(&s->activation_state,memory_order_acquire)!=SERVICE_ACTIVE ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return NULL;
    TaskHandle_t task=xTaskGetCurrentTaskHandle();
    for (unsigned i=0;i<DMA_SERVICE_CLIENTS;++i) if (s->clients[i].task == task) { *slot=i; return &s->clients[i]; }
    return NULL;
}
int dma_service_copy_period(struct dma_service *s,unsigned job,uint64_t ticket,void *out,size_t bytes)
{
    unsigned slot;struct dma_service_client *client=caller(s,&slot);
    if(!client || client->pending || job>=s->job_count || !out || bytes!=4096 ||
       !(client->jobs&(UINT32_C(1)<<job)) || (uintptr_t)out>UINTPTR_MAX-bytes ||
       a733_extents_overlap_prechecked((uintptr_t)out,bytes,(uintptr_t)s,sizeof(*s)))return 0;
    struct dma_service_private_job *j=&s->jobs[job];
    struct dma_service_channel *ch=&s->channels[j->channel];
    if(j->kind!=DMA_SERVICE_I2S_STREAM || ch->owner!=slot || ch->profile!=job)return 0;
    for(unsigned n=0;n<s->job_count;++n) {
        const struct a733_dma_transfer *t=&s->jobs[n].transfer;
        if(a733_extents_overlap_prechecked((uintptr_t)out,bytes,(uintptr_t)t->descriptors.cpu,t->descriptors.bytes))return 0;
        for(size_t i=0;i<t->mapping_count;++i)
            if(a733_extents_overlap_prechecked((uintptr_t)out,bytes,(uintptr_t)t->mappings[i].cpu,t->mappings[i].bytes))return 0;
    }
    return a733_i2s_stream_copy(dma_service_i2s_stream(s,j->channel),ticket,out,bytes);
}
int dma_service_send(struct dma_service *s,const struct dma_service_request *request,TickType_t wait,uint64_t *ticket)
{
    if (!request || !ticket || wait == portMAX_DELAY) return 0;
    unsigned slot;
    struct dma_service_client *c=caller(s,&slot);
    if (!c || c->pending || c->next_ticket == UINT64_MAX) return 0;
    struct dma_service_message m={*request,c->next_ticket+1,slot};
    c->pending=m.ticket;
    if (xQueueSend(s->requests,&m,wait) != pdTRUE) { c->pending=0; return 0; }
    c->next_ticket=m.ticket; *ticket=m.ticket;
    if (s->irq_bound) xTaskNotifyGive(s->worker);
    return 1;
}
int dma_service_collect(struct dma_service *s,uint64_t ticket,struct dma_service_reply *out,TickType_t wait)
{
    if (!out || !ticket || wait == portMAX_DELAY) return 0;
    unsigned slot;
    struct dma_service_client *c=caller(s,&slot);
    (void)slot;
    if (!c || c->pending != ticket) return 0;
    struct dma_service_reply reply;
    if (xQueueReceive(c->replies,&reply,wait) != pdTRUE) return 0;
    configASSERT(reply.ticket == ticket); c->pending=0; *out=reply; return 1;
}
