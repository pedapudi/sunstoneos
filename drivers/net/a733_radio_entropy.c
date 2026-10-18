#include "a733_radio_entropy.h"
/* Public FreeRTOS+TCP 4.4.2 accessor, declared in FreeRTOS_IP.h. Keeping this
 * declaration local avoids importing the stack configuration into CE users. */
extern TaskHandle_t FreeRTOS_GetIPTaskHandle(void);

static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an && bn && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y &&
        (x+an<=y || y+bn<=x);
}
static int contains(const void *a,size_t n,const void *p,size_t bytes)
{return a && n && bytes && n<=UINTPTR_MAX-(uintptr_t)a && (uintptr_t)p>=(uintptr_t)a &&
    (uintptr_t)p-(uintptr_t)a<n && bytes<=n-((uintptr_t)p-(uintptr_t)a);}
static void erase(void *p,size_t n)
{volatile uint8_t *v=p;while(n--)*v++=0;}
static int lock(atomic_uint *p)
{unsigned expected=0;return atomic_compare_exchange_strong_explicit(p,&expected,1,memory_order_acquire,memory_order_relaxed);}
static void unlock(atomic_uint *p)
{atomic_store_explicit(p,0,memory_order_release);}
static int outside(struct a733_radio_entropy *s,const void *p,size_t n)
{
    return separate(s,sizeof(*s),p,n) && separate(s->config.source,sizeof(*s->config.source),p,n) &&
        separate(s->config.provider_state,s->config.provider_bytes,p,n) &&
        separate(s->config.qualification_context,s->config.qualification_bytes,p,n) &&
        a733_ce_session_outside(s->engine,p,n);
}
static int identity(struct a733_radio_entropy *s)
{return s && s->self==s;}
static int healthy(struct a733_radio_entropy *s)
{
    return !atomic_load_explicit(&s->fault,memory_order_acquire) &&
        !atomic_load_explicit(&s->engine->fault,memory_order_acquire) &&
        !atomic_load_explicit(&s->config.clock->failed,memory_order_acquire) &&
        !atomic_load_explicit(&s->engine->binding.registry->failed,memory_order_acquire);
}
static void fault(struct a733_radio_entropy *s)
{
    atomic_store_explicit(&s->fault,1,memory_order_release);
    if(lock(&s->reservoir_busy)) {
        erase(s->reservoir,sizeof(s->reservoir));s->first=s->count=0;
        unlock(&s->reservoir_busy);
    }
}
int a733_radio_entropy_init(struct a733_radio_entropy *s,const struct a733_radio_entropy_config *input)
{
    if(!s || s->self || !input || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED ||
        !separate(s,sizeof(*s),input,sizeof(*input)))return 0;
    struct a733_radio_entropy_config c=*input;
    if(!c.source || !c.source->initialized || c.source->self!=(uintptr_t)c.source ||
        c.source->engine!=c.source->bound_engine || c.source->active || c.source->fault ||
        !c.owner || !c.pool_mutex || !c.clock || !c.clock->initialized ||
        !c.qualification_record || !c.request_timeout_us ||
        !c.qualified || !c.qualification_context || !c.qualification_bytes ||
        !a733_ce_session_bound(c.source->engine) ||
        !contains(c.provider_state,c.provider_bytes,c.source->engine->binding.cookie,1) ||
        !contains(c.provider_state,c.provider_bytes,c.pool_mutex,sizeof(StaticSemaphore_t)) ||
        !contains(c.provider_state,c.provider_bytes,c.clock,sizeof(*c.clock)))return 0;
    struct a733_ce_session *e=c.source->engine;
    if(e->binding.registry->now!=a733_counter_clock_now || e->binding.registry->context!=c.clock)return 0;
    const void *objects[]={s,input};const size_t sizes[]={sizeof(*s),sizeof(*input)};
    for(unsigned i=0;i<2;++i)
        if(!separate(objects[i],sizes[i],c.source,sizeof(*c.source)) ||
            !separate(objects[i],sizes[i],c.provider_state,c.provider_bytes) ||
            !separate(objects[i],sizes[i],c.qualification_context,c.qualification_bytes) ||
            !a733_ce_session_outside(e,objects[i],sizes[i]))return 0;
    if(c.qualified(c.qualification_context,e,c.qualification_record)!=1 ||
        c.source->engine!=e || c.source->bound_engine!=e || c.source->active || c.source->fault ||
        !a733_ce_session_bound(e) || a733_clock_token_status(e->binding.registry,&e->binding.token)!=A733_CLOCK_OK)return 0;
    s->config=c;s->engine=e;s->self=s;
    atomic_init(&s->busy,0);atomic_init(&s->reservoir_busy,0);atomic_init(&s->fault,0);
    return 1;
}
static enum a733_radio_entropy_result finish(struct a733_radio_entropy *s,enum a733_radio_entropy_result r)
{unlock(&s->busy);return r;}
static enum a733_radio_entropy_result retain(struct a733_radio_entropy *s)
{
    fault(s);erase(s->staging,sizeof(s->staging));s->staged=0;
    if(s->retained)return A733_RADIO_ENTROPY_RETAINED;
    if(xSemaphoreGetMutexHolder(s->config.pool_mutex)==xTaskGetCurrentTaskHandle() ||
        xSemaphoreTake(s->config.pool_mutex,0)!=pdTRUE)return A733_RADIO_ENTROPY_BUSY;
    enum a733_ce_result r=a733_ce_session_retain(s->engine);
    /* The service owns partial conditioned bytes separately from the CE page.
     * Its retained path erases them without further device operations. */
    if(s->active)
        (void)a733_ce_entropy_poll(s->config.source,s->generation,s->staging,sizeof(s->staging));
    erase(s->staging,sizeof(s->staging));
    BaseType_t gave=xSemaphoreGive(s->config.pool_mutex);
    if(gave!=pdTRUE || r==A733_CE_BUSY)return A733_RADIO_ENTROPY_BUSY;
    s->retained=1;return A733_RADIO_ENTROPY_RETAINED;
}
enum a733_radio_entropy_result a733_radio_entropy_retain(struct a733_radio_entropy *s)
{
    if(!identity(s)||xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING||
        xTaskGetCurrentTaskHandle()!=s->config.owner)return A733_RADIO_ENTROPY_INVALID;
    if(!lock(&s->busy))return A733_RADIO_ENTROPY_BUSY;
    return finish(s,retain(s));
}
enum a733_radio_entropy_result a733_radio_entropy_poll(struct a733_radio_entropy *s)
{
    if(!identity(s) || xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle()!=s->config.owner)return A733_RADIO_ENTROPY_INVALID;
    if(!lock(&s->busy))return A733_RADIO_ENTROPY_BUSY;
    if(!healthy(s))return finish(s,retain(s));
    uint64_t now;
    if(!a733_counter_clock_now(s->config.clock,&now) || now<s->last_now ||
        (s->deadline && now>=s->deadline))return finish(s,retain(s));
    s->last_now=now;
    if(!s->active && !s->staged) {
        if(!lock(&s->reservoir_busy))return finish(s,A733_RADIO_ENTROPY_BUSY);
        unsigned full=s->count>A733_RADIO_ENTROPY_CAPACITY-A733_CE_ENTROPY_MAX;
        unlock(&s->reservoir_busy);
        if(full)return finish(s,A733_RADIO_ENTROPY_READY);
    }
    if(xSemaphoreGetMutexHolder(s->config.pool_mutex)==s->config.owner ||
        xSemaphoreTake(s->config.pool_mutex,0)!=pdTRUE)return finish(s,A733_RADIO_ENTROPY_BUSY);
    unsigned failed=0;
    if(!a733_ce_session_bound(s->engine) || s->config.source->engine!=s->engine ||
        s->engine->binding.ops.now_us(s->engine->binding.cookie,&now)!=1 || now<s->last_now)failed=1;
    else {
        s->last_now=now;
        if(!s->deadline) {
            if(s->config.request_timeout_us>UINT64_MAX-now)failed=1;
            else s->deadline=now+s->config.request_timeout_us;
        }
        if(now>=s->deadline)failed=1;
    }
    enum a733_radio_entropy_result result=A733_RADIO_ENTROPY_PENDING;
    if(!failed && !s->staged) {
        enum a733_ce_entropy_result r;
        if(!s->active) {
            r=a733_ce_entropy_request(s->config.source,A733_CE_ENTROPY_MAX,s->deadline,&s->generation);
            if(r==A733_CE_ENTROPY_PENDING)s->active=1;
            else if(r==A733_CE_ENTROPY_BUSY)result=A733_RADIO_ENTROPY_BUSY;
            else failed=1;
        } else {
            r=a733_ce_entropy_poll(s->config.source,s->generation,s->staging,sizeof(s->staging));
            if(r==A733_CE_ENTROPY_BYTES){s->staged=1;s->active=0;}
            else if(r==A733_CE_ENTROPY_BUSY)result=A733_RADIO_ENTROPY_BUSY;
            else if(r!=A733_CE_ENTROPY_PENDING)failed=1;
        }
    }
    /* A lower fault may return BUSY while retaining registry metadata. It must
     * revoke already buffered bytes before that retention can finish. */
    if(s->config.source->fault || s->engine->fault || s->engine->controller.fault)failed=1;
    if(!failed && s->staged) {
        if(s->engine->binding.ops.now_us(s->engine->binding.cookie,&now)!=1 ||
            now<s->last_now || now>=s->deadline)failed=1;
        else {
            s->last_now=now;
            enum a733_clock_result r=a733_clock_token_status(s->engine->binding.registry,&s->engine->binding.token);
            if(r==A733_CLOCK_BUSY)result=A733_RADIO_ENTROPY_BUSY;
            else if(r!=A733_CLOCK_OK)failed=1;
            else result=A733_RADIO_ENTROPY_READY;
        }
    }
    if(xSemaphoreGive(s->config.pool_mutex)!=pdTRUE)failed=1;
    if(failed || !healthy(s))return finish(s,retain(s));
    if(s->staged && result==A733_RADIO_ENTROPY_READY) {
        if(!lock(&s->reservoir_busy))return finish(s,A733_RADIO_ENTROPY_BUSY);
        if(!healthy(s)) {unlock(&s->reservoir_busy);return finish(s,retain(s));}
        for(size_t i=0;i<sizeof(s->staging);++i)
            s->reservoir[(s->first+s->count+i)%A733_RADIO_ENTROPY_CAPACITY]=s->staging[i];
        s->count+=sizeof(s->staging);erase(s->staging,sizeof(s->staging));s->staged=0;s->deadline=0;
        unlock(&s->reservoir_busy);
    }
    return finish(s,result);
}
int a733_radio_entropy_bytes(void *cookie,uint8_t *out,size_t bytes)
{
    struct a733_radio_entropy *s=cookie;
    if(!identity(s) || !bytes || bytes>A733_CE_ENTROPY_MAX || !outside(s,out,bytes) ||
        xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING)return 0;
    TaskHandle_t current=xTaskGetCurrentTaskHandle();
    if(!current || (current!=s->config.owner && current!=FreeRTOS_GetIPTaskHandle()))return 0;
    if(!lock(&s->reservoir_busy))return 0;
    if(!healthy(s)) {
        atomic_store_explicit(&s->fault,1,memory_order_release);
        erase(s->reservoir,sizeof(s->reservoir));s->first=s->count=0;
        unlock(&s->reservoir_busy);return 0;
    }
    if(bytes>s->count){unlock(&s->reservoir_busy);return 0;}
    for(size_t i=0;i<bytes;++i) {
        out[i]=s->reservoir[s->first];s->reservoir[s->first]=0;
        s->first=(s->first+1)%A733_RADIO_ENTROPY_CAPACITY;
    }
    s->count-=bytes;unlock(&s->reservoir_busy);return 1;
}
int a733_radio_entropy_random32(void *cookie,uint32_t *out)
{return a733_radio_entropy_bytes(cookie,(uint8_t *)out,sizeof(*out));}
int a733_radio_entropy_sequence(void *cookie,uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,uint32_t *out)
{
    /* A fresh independent word per call; no tuple-derived deterministic seed. */
    (void)src;(void)sport;(void)dst;(void)dport;
    return a733_radio_entropy_random32(cookie,out);
}
