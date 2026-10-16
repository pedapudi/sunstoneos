#include "a733_dma_startup.h"
#include "a733_dma_platform_native.h"

static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
static int privileged(void)
{
    uint64_t level;__asm__ volatile("mrs %0, CurrentEL" : "=r"(level));return level==4;
}
static int exact(const struct a733_dma_platform_config *a,const struct a733_dma_platform_config *b)
{
    for(unsigned i=0;i<3;++i) if(a->fabric[i]!=b->fabric[i] || a->rate_hz[i]!=b->rate_hz[i]) return 0;
    return a->expected_cpu==b->expected_cpu && a->expected_mcu==b->expected_mcu &&
        a->desired_cpu==b->desired_cpu && a->desired_mcu==b->desired_mcu &&
        a->route_change_mask==b->route_change_mask && a->owner==b->owner &&
        a->handoff==b->handoff && a->deadline_us==b->deadline_us && a->cold==b->cold;
}
static int same_token(const struct a733_clock_token *a,const struct a733_clock_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
static int task_owner(struct a733_dma_startup *b)
{
    return b && b->initialized && !b->failed && privileged() && b->task &&
        xTaskGetSchedulerState()==taskSCHEDULER_RUNNING && xTaskGetCurrentTaskHandle()==b->task;
}
static enum a733_dma_platform_result acquire(void *cookie,const struct a733_dma_platform_config *c,
    a733_clock_mask_t shared,struct a733_clock_token *out)
{
    struct a733_dma_startup *b=cookie;a733_clock_mask_t expected;
    if(!task_owner(b) || b->register_held || !c || !out || !exact(c,&b->handoff.allowed) ||
        !a733_dma_platform_scopes(&b->handoff.allowed,&expected) || shared!=expected ||
        b->handoff.controller!=0x04601000 || b->handoff.evidence!=c->handoff ||
        b->handoff.owned_channels!=0xffff || b->handoff.established!=A733_DMA_HANDOFF_REQUIRED) {
        if(b) b->failed=1;
        return A733_DMA_PLATFORM_RETAINED;
    }
    if(out->id || b->token.id) {
        if(!out->id || !same_token(out,&b->token)) {b->failed=1;return A733_DMA_PLATFORM_RETAINED;}
        return A733_DMA_PLATFORM_READY;
    }
    enum a733_clock_result r=a733_clock_acquire(b->registry,c->owner,shared,A733_CLOCK_DMA0,c->deadline_us,&b->token);
    if(r==A733_CLOCK_BUSY || r==A733_CLOCK_EXPIRED) return A733_DMA_PLATFORM_BUSY;
    if(r!=A733_CLOCK_OK) {b->failed=1;return A733_DMA_PLATFORM_RETAINED;}
    *out=b->token;return A733_DMA_PLATFORM_READY;
}
static int enter(void *cookie,uint64_t deadline)
{
    struct a733_dma_startup *b=cookie;
    if(!task_owner(b) || b->register_held || !b->token.id ||
        !same_token(&b->token,&b->platform.token) || deadline!=b->handoff.allowed.deadline_us) return -1;
    if(!a733_platform_lock_enter(b->locks,A733_PLATFORM_REGISTER_LOCK,deadline)) return 0;
    b->register_held=1;return 1;
}
static void leave(void *cookie)
{
    struct a733_dma_startup *b=cookie;
    configASSERT(b && b->register_held && xTaskGetCurrentTaskHandle()==b->task);
    a733_platform_lock_leave(b->locks,A733_PLATFORM_REGISTER_LOCK);b->register_held=0;
}
static int now(void *cookie,uint64_t *out)
{
    struct a733_dma_startup *b=cookie;
    return task_owner(b) && a733_counter_clock_now(b->clock,out);
}
static int read_reg(void *cookie,uintptr_t address,uint32_t *out)
{
    struct a733_dma_startup *b=cookie;
    return task_owner(b) && b->register_held && a733_dma_platform_native_read(0,address,out);
}
static int write_reg(void *cookie,uintptr_t address,uint32_t value)
{
    struct a733_dma_startup *b=cookie;
    return task_owner(b) && b->register_held && a733_dma_platform_native_write(0,address,value);
}
int a733_dma_startup_init(struct a733_dma_startup *b,const struct a733_dma_handoff *h,
    struct a733_counter_clock *clock,struct a733_clock_leases *registry,struct a733_platform_locks *locks)
{
    a733_clock_mask_t shared;
    if(!privileged() || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED || !b || b->initialized ||
        !h || !clock || !registry || !locks || !clock->initialized || !registry->initialized || !locks->initialized ||
        !separate(b,sizeof(*b),h,sizeof(*h)) || !separate(b,sizeof(*b),clock,sizeof(*clock)) ||
        !separate(b,sizeof(*b),registry,sizeof(*registry)) || !separate(b,sizeof(*b),locks,sizeof(*locks)) ||
        !separate(h,sizeof(*h),clock,sizeof(*clock)) || !separate(h,sizeof(*h),registry,sizeof(*registry)) ||
        !separate(h,sizeof(*h),locks,sizeof(*locks)) ||
        !separate(clock,sizeof(*clock),registry,sizeof(*registry)) || !separate(clock,sizeof(*clock),locks,sizeof(*locks)) ||
        !separate(registry,sizeof(*registry),locks,sizeof(*locks)) ||
        registry->now!=a733_counter_clock_now || registry->context!=clock ||
        locks->now!=a733_counter_clock_now || locks->context!=clock ||
        h->controller!=0x04601000 || h->owned_channels!=0xffff || !h->service_channels ||
        (h->service_channels&~h->allowed.desired_cpu) || h->evidence!=h->allowed.handoff ||
        h->established!=A733_DMA_HANDOFF_REQUIRED || !a733_dma_platform_scopes(&h->allowed,&shared)) return 0;
    b->handoff=*h;b->clock=clock;b->registry=registry;b->locks=locks;
    b->operations=(struct a733_dma_platform_operations){acquire,enter,leave,now,read_reg,write_reg};
    if(!a733_dma_platform_init(&b->platform,&h->allowed,&b->operations,b)) return 0;
    b->initialized=1;return 1;
}
int a733_dma_startup_bind_task(struct a733_dma_startup *b,TaskHandle_t task)
{
    if(!b || !b->initialized || b->failed || b->task || !task || !privileged() ||
        xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED) return 0;
    b->task=task;return 1;
}
enum a733_dma_platform_result a733_dma_startup_poll(struct a733_dma_startup *b)
{
    if(!b || !b->initialized || b->failed || !privileged() ||
        xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING) return A733_DMA_PLATFORM_RETAINED;
    TaskHandle_t self=xTaskGetCurrentTaskHandle();
    if(!self || !b->task || b->task!=self) return A733_DMA_PLATFORM_RETAINED;
    enum a733_dma_platform_result r=a733_dma_platform_poll(&b->platform);
    if(r==A733_DMA_PLATFORM_RETAINED) b->failed=1;
    return r;
}
int a733_dma_startup_ready(const struct a733_dma_startup *b)
{return b && b->initialized && !b->failed && a733_dma_platform_ready(&b->platform);}

int a733_dma_startup_prepared(void *cookie,uintptr_t controller,uint16_t channels)
{
    struct a733_dma_startup *b=cookie;
    if(!task_owner(b)) return -1;
    if(controller!=b->handoff.controller || channels!=b->handoff.service_channels ||
        !exact(&b->platform.config,&b->handoff.allowed) ||
        b->platform.failed || atomic_load_explicit(&b->clock->failed,memory_order_acquire) ||
        atomic_load_explicit(&b->locks->retained,memory_order_acquire) ||
        (b->token.id && !same_token(&b->token,&b->platform.token))) {
        b->failed=1;return -1;
    }
    if(b->token.id) {
        enum a733_clock_result status=a733_clock_token_status(b->registry,&b->token);
        if(status==A733_CLOCK_BUSY) return 0;
        if(status!=A733_CLOCK_OK) {b->failed=1;return -1;}
    }
    return a733_dma_startup_ready(b) ? 1 : 0;
}
