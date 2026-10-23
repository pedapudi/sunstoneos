#include "a733_npu_startup.h"
#include "a733_npu_platform_native.h"
#include "a733_range.h"

#define READY A733_NPU_PLATFORM_READY
#define BUSY A733_NPU_PLATFORM_BUSY
#define PENDING A733_NPU_PLATFORM_PENDING
#define RETAINED A733_NPU_PLATFORM_RETAINED
enum { CLOCK, RAIL, PCK, FIRST_SNAPSHOT, SETTLE, SECOND_SNAPSHOT, RESOURCES_READY };
static int privileged(void)
{uint64_t level;__asm__ volatile("mrs %0,CurrentEL":"=r"(level));return level==4;}
static int resource_now(void *v,uint64_t *out)
{
    struct a733_npu_startup_resources *s=v;
    return !atomic_load_explicit(&s->locks->retained,memory_order_acquire) && a733_counter_clock_now(s->clock,out);
}
static void retain_owned(struct a733_platform_locks *locks)
{
    TaskHandle_t self=xTaskGetCurrentTaskHandle();
    for(unsigned i=0;i<2;++i)
        if(xSemaphoreGetMutexHolder(locks->mutex[i])==self)
            (void)a733_platform_lock_retain(locks,(enum a733_platform_lock)i);
}
static int resource_read(void *v,uintptr_t address,uint32_t *out)
{
    struct a733_npu_startup_resources *s=v;
    return s->native.operations.read32(&s->native,address,out);
}
static int resource_update(void *v,uintptr_t address,uint32_t required,uint32_t set,uint64_t deadline)
{
    struct a733_npu_startup_resources *s=v;
    int ok=s->native.operations.update32(&s->native,address,required,set,deadline);
    if(!ok)retain_owned(s->locks);
    return ok;
}
static int resource_rail(void *v,struct a733_gpu_resource_rail *out,uint64_t deadline)
{
    struct a733_npu_startup_resources *s=v;
    int ok=s->native.operations.rail(&s->native,out,deadline);
    if(!ok)retain_owned(s->locks);
    return ok;
}
int a733_npu_startup_resources_init(struct a733_npu_startup_resources *s,
    struct a733_counter_clock *clock,struct a733_platform_locks *locks)
{
    if(!privileged() || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED ||
        !s || s->initialized || !clock || !clock->initialized || !locks || !locks->initialized ||
        !a733_ranges_disjoint(s,sizeof(*s),clock,sizeof(*clock)) ||
        !a733_ranges_disjoint(s,sizeof(*s),locks,sizeof(*locks)) ||
        !a733_ranges_disjoint(clock,sizeof(*clock),locks,sizeof(*locks)) ||
        locks->now!=a733_counter_clock_now || locks->context!=clock)return 0;
    s->clock=clock;s->locks=locks;
    if(!a733_gpu_resources_native_init(&s->native,&locks->callbacks))return 0;
    const struct a733_gpu_resource_ops ops={resource_now,resource_read,resource_update,resource_rail};
    if(!a733_gpu_resources_init(&s->manager,&ops,s))return 0;
    s->native.manager=&s->manager;s->native.manager_operations=ops;s->native.manager_context=s;
    s->initialized=1;return 1;
}
static int exact_config(const struct a733_npu_platform_config *a,const struct a733_npu_platform_config *b)
{
    for(unsigned i=0;i<3;++i)if(a->fabric[i]!=b->fabric[i] || a->rate_hz[i]!=b->rate_hz[i])return 0;
    return a->owner==b->owner && a->handoff==b->handoff && a->deadline_us==b->deadline_us;
}
static int same_clock(const struct a733_clock_token *a,const struct a733_clock_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
static int task_owner(struct a733_npu_startup *b)
{
    return b && b->initialized && !b->failed && b->task &&
        xTaskGetSchedulerState()==taskSCHEDULER_RUNNING && xTaskGetCurrentTaskHandle()==b->task && privileged();
}
static enum a733_npu_platform_result fault(struct a733_npu_startup *b)
{b->failed=1;retain_owned(b->shared->locks);return RETAINED;}
static int timely(struct a733_npu_startup *b,uint64_t *out)
{
    uint64_t now;
    if(!a733_counter_clock_now(b->shared->clock,&now) || now>=b->handoff.allowed.deadline_us)return 0;
    if(out)*out=now;
    return 1;
}
static int pll_config_equal(const struct a733_npu_pll_config *a,const struct a733_npu_pll_config *b)
{
    if(a->owner!=b->owner || a->handoff!=b->handoff || a->deadline_us!=b->deadline_us ||
        a->mode!=b->mode || a->initial_control!=b->initial_control)return 0;
    for(unsigned i=0;i<A733_NPU_PLL_SNAPSHOTS;++i)if(a->consumers[i]!=b->consumers[i])return 0;
    return 1;
}
static int pll_ops_equal(const struct a733_npu_pll_operations *a,const struct a733_npu_pll_operations *b)
{
    return a->acquire==b->acquire && a->enter==b->enter && a->leave==b->leave &&
        a->retain==b->retain && a->now_us==b->now_us && a->read==b->read && a->write==b->write;
}
static int pll_integrity(const struct a733_npu_startup *b)
{
    const struct a733_npu_pll *p=b->pll_source;
    if(!p)return !b->pll_binding_self && !b->pll_transferred && !b->pll_whole_platform && !b->platform.adopt_on;
    if(b->pll_binding_self!=b || p->self!=p || !p->initialized || p->failed ||
        p->registry!=b->registry || p->successor!=b || p->transferred!=b->pll_transferred ||
        !pll_config_equal(&p->config,&b->pll_config) || !pll_ops_equal(&p->ops,&b->pll_operations) ||
        p->cookie!=b->pll_cookie ||
        b->platform.adopt_on!=(unsigned)(p->config.mode==A733_NPU_PLL_LIVE_ENABLE))return 0;
    if(b->pll_transferred && (!a733_npu_pll_ready(p) || !p->resources.whole_platform ||
        p->resources.whole_platform!=b->pll_whole_platform || p->observed.whole_platform!=b->pll_whole_platform ||
        !same_clock(&p->resources.clock,&b->clock_token) || !same_clock(&p->observed.clock,&b->clock_token)))return 0;
    return 1;
}
static int rail_snapshot(struct a733_npu_startup *b)
{
    if(b->shared->native.pmic)return 0;
    struct a733_platform_locks *locks=b->shared->locks;
    if(!timely(b,0) || !a733_platform_lock_enter(locks,A733_PLATFORM_I2C_LOCK,b->handoff.allowed.deadline_us))return 0;
    b->i2c_held=1;b->rail_step=0;
    if(!timely(b,0) || a733_i2c_open(&b->bus)!=I2C_OK || !timely(b,0))return 0;
    static const uint8_t registers[]={0xff,0x0e,0x0f,0x10,0x13,0xff};
    for(unsigned i=0;i<6;++i) {
        b->rail_step=i+1;
        if(!timely(b,0) || a733_i2c_read8(&b->bus,0x36,registers[i],&b->rail[i])!=I2C_OK || !timely(b,0))return 0;
        if((i==0 && b->rail[i]!=0) || (i==1 && b->rail[i]!=3) || (i==2 && b->rail[i]!=0) ||
            (i==3 && !(b->rail[i]&2)) || (i==4 && (b->rail[i]&127)!=30) || (i==5 && b->rail[i]!=0))return 0;
    }
    b->rail_step=7;
    a733_i2c_close(&b->bus);
    if(!timely(b,0))return 0;
    const uintptr_t addresses[]={0x0708300c,0x07083010,0x07083020};
    const uint32_t masks[]={0xf4,UINT32_MAX,0x35},expected[]={b->bus.control&0xc4,0xf8,0x30};
    for(unsigned i=0;i<3;++i) {
        uint32_t value;b->rail_step=8+i;
        if(!timely(b,0) || !b->shared->native.operations.read32(&b->shared->native,addresses[i],&value) ||
            !timely(b,0) || (value&masks[i])!=expected[i])return 0;
    }
    a733_platform_lock_leave(locks,A733_PLATFORM_I2C_LOCK);b->i2c_held=0;
    return timely(b,0);
}
static void publish(struct a733_npu_startup *b,struct a733_npu_platform_resources *out)
{
    out->clock=b->clock_token;out->rail=b->pmic.self ? b->pmic.rail.id : b->rail_token.id;out->pck=b->pck_token.token;
    out->supply_uv=b->stage==RESOURCES_READY ? A733_NPU_INITIAL_UV : 0;
}
static enum a733_npu_platform_result acquire(void *v,const struct a733_npu_platform_config *c,
    a733_clock_mask_t shared,struct a733_npu_platform_resources *out)
{
    struct a733_npu_startup *b=v;a733_clock_mask_t expected;
    if(!b || !b->initialized)return RETAINED;
    if(!task_owner(b) || b->register_held || b->i2c_held || !c || !out ||
        !exact_config(c,&b->handoff.allowed) || !a733_npu_platform_scopes(c,&expected) || shared!=expected ||
        !same_clock(&out->clock,&b->clock_token) ||
        out->rail!=(b->pmic.self ? b->pmic.rail.id : b->rail_token.id) || out->pck!=b->pck_token.token ||
        b->handoff.evidence!=c->handoff || !a733_npu_handoff_established(b->handoff.established))return fault(b);
    if(!timely(b,0) || !pll_integrity(b))return fault(b);
    enum a733_clock_result cr;
    if(b->stage==CLOCK && b->pll_source) {
        struct a733_npu_pll *p=b->pll_source;
        if(!a733_npu_pll_ready(p))return PENDING;
        if(p->transferred || !p->resources.whole_platform ||
            p->observed.whole_platform!=p->resources.whole_platform ||
            !same_clock(&p->observed.clock,&p->resources.clock) ||
            p->resources.clock.owner!=c->owner || p->resources.clock.shared!=(shared&~A733_CLOCK_PLL_NPU) ||
            p->resources.clock.exclusive!=(A733_CLOCK_PLL_NPU|A733_CLOCK_NPU))return fault(b);
        /* Persistent output is populated at registry commit. No failing action
         * separates that commit from installing every live local token copy. */
        cr=a733_clock_publish_stable(b->registry,&p->resources.clock,A733_CLOCK_PLL_NPU,
            c->deadline_us,&b->clock_token);
        if(cr==A733_CLOCK_BUSY)return BUSY;
        if(cr!=A733_CLOCK_OK)return fault(b);
        p->resources.clock=b->clock_token;p->observed.clock=b->clock_token;
        p->transferred=1;b->pll_transferred=1;
        b->pll_whole_platform=p->resources.whole_platform;b->stage=RAIL;
        publish(b,out);
        if(!timely(b,0))return fault(b);
        return PENDING;
    }
    if(b->stage==RAIL && b->pmic.self) {
        int r=a733_pmic_consumer_reserve(&b->pmic,c->deadline_us);
        publish(b,out);
        if(r<0)return fault(b);
        if(r==1)++b->stage;
        return PENDING;
    }
    if(b->stage==CLOCK || b->stage==RAIL) {
        cr=a733_clock_acquire(b->registry,c->owner,b->stage==CLOCK ? shared : A733_POWER_DCDC2,
            b->stage==CLOCK ? A733_CLOCK_NPU : 0,c->deadline_us,
            b->stage==CLOCK ? &b->clock_token : &b->rail_token);
        if(cr==A733_CLOCK_BUSY)return BUSY;
        publish(b,out);
        if(cr!=A733_CLOCK_OK)return fault(b);
        ++b->stage;return PENDING;
    }
    if(b->stage==PCK) {
        struct a733_gpu_power_resources ready={0};
        uint32_t mask=A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK;
        if(shared&A733_CLOCK_PERI0)mask|=A733_GPU_RESOURCE_PARENT;
        b->pck_token.mask=mask;b->pck_token.mode=A733_GPU_RESOURCE_STABLE;
        enum a733_gpu_resource_result r=a733_gpu_resources_acquire(&b->shared->manager,b->handoff.pck_client,
            mask,c->deadline_us,&b->pck_token.token,&ready);
        if(r==A733_GPU_RESOURCE_BUSY)return BUSY;
        publish(b,out);
        if(r!=A733_GPU_RESOURCE_OK)return fault(b);
        ++b->stage;return PENDING;
    }
    if(b->stage==FIRST_SNAPSHOT || b->stage==SECOND_SNAPSHOT) {
        if(b->pmic.self) {
            int r=a733_pmic_consumer_sample(&b->pmic,c->deadline_us);
            if(r<0)return fault(b);
            if(!r)return PENDING;
        } else if(!rail_snapshot(b))return fault(b);
        if(b->stage==FIRST_SNAPSHOT) {
            uint64_t now;
            if(!timely(b,&now) ||
                !a733_deadline_after(now,A733_NPU_RAIL_SETTLE_US,&b->settle_until))return fault(b);
            ++b->stage;
        } else b->stage=RESOURCES_READY;
    } else if(b->stage==SETTLE) {
        uint64_t now;if(!timely(b,&now))return fault(b);
        if(now>=b->settle_until) {
            if(b->pmic.self) {
                int r=a733_pmic_consumer_refresh(&b->pmic,c->deadline_us);
                if(r<0)return fault(b);
                if(!r)return PENDING;
            }
            ++b->stage;
        }
    }
    publish(b,out);
    return b->stage==RESOURCES_READY ? READY : PENDING;
}
static int enter(void *v,uint64_t deadline)
{
    struct a733_npu_startup *b=v;
    if(!task_owner(b) || b->register_held || b->i2c_held || b->stage!=RESOURCES_READY ||
        deadline!=b->handoff.allowed.deadline_us || !same_clock(&b->clock_token,&b->platform.resources.clock))return -1;
    if(!a733_platform_lock_enter(b->shared->locks,A733_PLATFORM_REGISTER_LOCK,deadline))return 0;
    b->register_held=1;return 1;
}
static int leave(void *v)
{
    struct a733_npu_startup *b=v;
    configASSERT(b->register_held && xTaskGetCurrentTaskHandle()==b->task);
    if(b->failed || b->platform.failed ||
        atomic_load_explicit(&b->shared->locks->retained,memory_order_acquire) || !timely(b,0)) {
        (void)fault(b);return 0;
    }
    a733_platform_lock_leave(b->shared->locks,A733_PLATFORM_REGISTER_LOCK);b->register_held=0;return 1;
}
/* Called only from a startup or stop step, which checks the owner task first;
 * register access also needs the lock that enter() took after its owner check. */
static int now(void *v,uint64_t *out)
{struct a733_npu_startup *b=v;return resource_now(b->shared,out);}
static int read_reg(void *v,uintptr_t address,uint32_t *out)
{
    struct a733_npu_startup *b=v;
    if(!b->register_held)return 0;
    int ok=a733_npu_platform_native_read(0,address,out);if(!ok)(void)fault(b);return ok;
}
static int write_reg(void *v,uintptr_t address,uint32_t value)
{
    struct a733_npu_startup *b=v;
    if(!b->register_held)return 0;
    int ok=a733_npu_platform_native_write(0,address,value);if(!ok)(void)fault(b);return ok;
}
static int valid_backend(const struct a733_npu_handoff *h)
{
    const struct a733_npu_ops *o=&h->backend;
    if(!h->command_admission || !h->command_count || h->command_count>NPU_SERVICE_COMMANDS ||
        !o->read || !o->write || !o->microseconds || !o->delay || !o->barrier || !o->publish || !o->finish)return 0;
    for(unsigned i=0;i<h->command_count;++i) {
        const struct a733_npu_command *c=&h->commands[i];
        if(!c->device || (c->device&63) || !c->token || c->readable_bytes<A733_NPU_FETCH_BYTES ||
            c->readable_bytes>(UINT64_C(1)<<32)-c->device)return 0;
    }
    return 1;
}
int a733_npu_startup_init(struct a733_npu_startup *b,const struct a733_npu_handoff *h,
    struct a733_npu_startup_resources *s,struct a733_clock_leases *registry)
{
    a733_clock_mask_t shared;
    if(!privileged() || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED || !b || b->initialized ||
        !h || !s || !s->initialized || !registry || !registry->initialized ||
        !a733_ranges_disjoint(b,sizeof(*b),h,sizeof(*h)) ||
        !a733_ranges_disjoint(b,sizeof(*b),s,sizeof(*s)) ||
        !a733_ranges_disjoint(b,sizeof(*b),registry,sizeof(*registry)) ||
        !a733_ranges_disjoint(s,sizeof(*s),registry,sizeof(*registry)) ||
        !a733_ranges_disjoint(h,sizeof(*h),s,sizeof(*s)) ||
        !a733_ranges_disjoint(h,sizeof(*h),registry,sizeof(*registry)) ||
        !a733_ranges_disjoint(b,sizeof(*b),s->clock,sizeof(*s->clock)) ||
        !a733_ranges_disjoint(b,sizeof(*b),s->locks,sizeof(*s->locks)) ||
        !a733_ranges_disjoint(h,sizeof(*h),s->clock,sizeof(*s->clock)) ||
        !a733_ranges_disjoint(h,sizeof(*h),s->locks,sizeof(*s->locks)) ||
        !a733_ranges_disjoint(registry,sizeof(*registry),s->clock,sizeof(*s->clock)) ||
        !a733_ranges_disjoint(registry,sizeof(*registry),s->locks,sizeof(*s->locks)) ||
        registry->now!=a733_counter_clock_now || registry->context!=s->clock ||
        s->manager.ops.now!=resource_now || s->manager.context!=s ||
        h->controller!=0x03600000 || h->evidence!=h->allowed.handoff ||
        !a733_npu_handoff_established(h->established) || h->pck_client>=A733_GPU_RESOURCE_CLIENTS ||
        !a733_npu_platform_scopes(&h->allowed,&shared) || !valid_backend(h))return 0;
    b->handoff=*h;b->shared=s;b->registry=registry;
    b->operations=(struct a733_npu_platform_operations){acquire,enter,leave,now,read_reg,write_reg};
    if(!a733_npu_platform_init(&b->platform,&b->handoff.allowed,&b->operations,b))return 0;
    b->initialized=1;return 1;
}
int a733_npu_startup_bind_pll(struct a733_npu_startup *b,struct a733_npu_pll *p)
{
    a733_clock_mask_t shared,exclusive,platform_shared;
    if(!privileged() || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED ||
        !b || !b->initialized || b->failed || b->pll_source || b->stage!=CLOCK || b->platform.stage ||
        b->clock_token.id || b->rail_token.id || b->pck_token.token || b->platform.resources.clock.id ||
        !p || p->self!=p || !p->initialized || p->failed || p->successor || p->transferred ||
        p->registry!=b->registry || !a733_ranges_disjoint(b,sizeof(*b),p,sizeof(*p)) ||
        !a733_ranges_disjoint(p,sizeof(*p),b->shared,sizeof(*b->shared)) ||
        !a733_ranges_disjoint(p,sizeof(*p),b->registry,sizeof(*b->registry)) ||
        !a733_ranges_disjoint(p,sizeof(*p),b->shared->clock,sizeof(*b->shared->clock)) ||
        !a733_ranges_disjoint(p,sizeof(*p),b->shared->locks,sizeof(*b->shared->locks)) ||
        !a733_npu_pll_scopes(&p->config,&shared,&exclusive) ||
        !a733_npu_platform_scopes(&b->handoff.allowed,&platform_shared) ||
        (shared|A733_CLOCK_PLL_NPU)!=platform_shared ||
        p->config.owner!=b->handoff.allowed.owner || p->config.handoff!=b->handoff.allowed.handoff ||
        b->registry->now!=a733_counter_clock_now || b->registry->context!=b->shared->clock)return 0;
    const uint32_t masks[]={0x0300001f,0x8700001f,0x8700001f};
    for(unsigned i=0;i<3;++i)
        if((p->config.consumers[i]&masks[i])!=b->handoff.allowed.fabric[i])return 0;
    if(p->config.mode==A733_NPU_PLL_LIVE_ENABLE &&
       (b->handoff.established!=A733_NPU_HANDOFF_SUPERVISED_REQUIRED ||
        !a733_npu_platform_adopt_on(&b->platform)))return 0;
    b->pll_source=p;b->pll_binding_self=b;b->pll_config=p->config;
    b->pll_operations=p->ops;b->pll_cookie=p->cookie;p->successor=b;
    return 1;
}
int a733_npu_startup_bind_pmic(struct a733_npu_startup *b,struct a733_pmic *p,
    const struct a733_pmic_lifetime *life)
{
    if(!privileged() || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED ||
        !b || !b->initialized || b->failed || b->stage!=CLOCK || b->pmic.self || b->task ||
        b->clock_token.id || b->rail_token.id || b->pck_token.token || !p || p->self!=p || !life ||
        p->clocks!=b->registry || p->rails->binding.gpu!=&b->shared->manager ||
        !a733_pmic_consumer_storage(p,b,sizeof(*b)) ||
        !a733_gpu_resources_native_use_pmic(&b->shared->native,&b->shared->manager,p))return 0;
    const struct a733_pmic_consumer_config config={p,*life,b->handoff.allowed.owner,A733_RAIL_DCDC2};
    return a733_pmic_consumer_init(&b->pmic,&config);
}
int a733_npu_startup_bind_task(struct a733_npu_startup *b,TaskHandle_t task)
{
    if(!b || !b->initialized || b->failed || b->task || !task || !privileged() ||
        xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    b->task=task;return 1;
}
int a733_npu_startup_supply_sample(struct a733_npu_startup *b)
{
    if(!task_owner(b))return -1;
    if(b->stage!=CLOCK || b->platform.stage || !b->pll_source || !pll_integrity(b) ||
       b->register_held || b->i2c_held || b->clock_token.id || b->pck_token.token ||
       b->pmic.self!=&b->pmic || b->pmic.config.provider!=b->shared->native.pmic ||
       b->shared->native.pmic_manager!=&b->shared->manager || !timely(b,0)) {
        fault(b);return -1;
    }
    int r=a733_pmic_consumer_sample(&b->pmic,b->handoff.allowed.deadline_us);
    publish(b,&b->platform.resources);b->platform.observed=b->platform.resources;
    if(r<0 || !timely(b,0)){fault(b);return -1;}
    return r;
}
static enum a733_npu_platform_result platform_step(struct a733_npu_startup *b,int stop)
{
    if(!task_owner(b))return RETAINED;
    if((stop || !a733_npu_platform_ready(&b->platform)) && !timely(b,0))return fault(b);
    if(b->shared->native.pmic) {
        if(b->pmic.self!=&b->pmic || b->pmic.config.provider!=b->shared->native.pmic ||
            b->shared->native.pmic_manager!=&b->shared->manager ||
            !a733_gpu_resources_native_use_pmic(&b->shared->native,&b->shared->manager,b->pmic.config.provider))return fault(b);
        if(b->stage==RESOURCES_READY) {
            int r=a733_pmic_consumer_health(&b->pmic);
            if(r<0)return fault(b);
            if(!r)return BUSY;
        }
    } else if(b->pmic.self)return fault(b);
    if(!pll_integrity(b))return fault(b);
    if(b->pll_transferred) {
        enum a733_clock_result health=a733_clock_token_status(b->registry,&b->clock_token);
        if(health==A733_CLOCK_BUSY)return BUSY;
        if(health!=A733_CLOCK_OK)return fault(b);
    }
    enum a733_npu_platform_result r=stop?a733_npu_platform_stop_poll(&b->platform):a733_npu_platform_poll(&b->platform);
    if(r==RETAINED)(void)fault(b);
    if(r==READY && b->pmic.self) {
        int health=a733_pmic_consumer_health(&b->pmic);
        if(health<0)return fault(b);
        if(!health)return BUSY;
    }
    return r;
}
/* Run acquisition steps, each with every entry check, until one waits; the platform's
 * register pass then follows. Stage only advances, so at most six steps repeat. */
enum a733_npu_platform_result a733_npu_startup_poll(struct a733_npu_startup *b)
{
    enum a733_npu_platform_result r;unsigned stage;
    do {stage=b ? b->stage : 0;r=platform_step(b,0);}
    while(r==PENDING && !b->platform.stage && b->stage>stage);
    return r;
}
enum a733_npu_platform_result a733_npu_startup_stop_poll(struct a733_npu_startup *b)
{return platform_step(b,1);}
int a733_npu_startup_ready(const struct a733_npu_startup *b)
{return b && b->initialized && !b->failed && pll_integrity(b) && a733_npu_platform_ready(&b->platform);}
static int backend_equal(const struct a733_npu_ops *a,const struct a733_npu_ops *b)
{
    return a && a->read==b->read && a->write==b->write && a->microseconds==b->microseconds &&
        a->delay==b->delay && a->barrier==b->barrier && a->publish==b->publish && a->finish==b->finish;
}
int a733_npu_startup_prepared(void *v,const struct a733_npu_ops *o,void *cookie,
    const struct a733_npu_mmu_image *image,const struct a733_npu_command *commands,unsigned count)
{
    struct a733_npu_startup *b=v;
    if(!task_owner(b))return -1;
    if(!pll_integrity(b)){fault(b);return -1;}
    const struct a733_npu_handoff *h=&b->handoff;
    if(!backend_equal(o,&h->backend) || cookie!=h->backend_cookie || !image || !commands ||
        count!=h->command_count || image->physical!=h->image.physical || image->bytes!=h->image.bytes ||
        image->pd_entry!=h->image.pd_entry || image->tables!=h->image.tables ||
        !exact_config(&b->platform.config,&h->allowed) || b->platform.failed ||
        atomic_load_explicit(&b->shared->clock->failed,memory_order_acquire) ||
        atomic_load_explicit(&b->shared->locks->retained,memory_order_acquire)) {fault(b);return -1;}
    for(unsigned i=0;i<count;++i)
        if(commands[i].device!=h->commands[i].device || commands[i].readable_bytes!=h->commands[i].readable_bytes ||
            commands[i].token!=h->commands[i].token){fault(b);return -1;}
    const struct a733_npu_platform_resources *r=&b->platform.resources;
    if(!same_clock(&r->clock,&b->clock_token) ||
        r->rail!=(b->pmic.self ? b->pmic.rail.id : b->rail_token.id) || r->pck!=b->pck_token.token){fault(b);return -1;}
    if(b->shared->native.pmic) {
        if(b->pmic.self!=&b->pmic || b->pmic.config.provider!=b->shared->native.pmic ||
            b->shared->native.pmic_manager!=&b->shared->manager ||
            !a733_gpu_resources_native_use_pmic(&b->shared->native,&b->shared->manager,b->pmic.config.provider))
            {fault(b);return -1;}
        int health=a733_pmic_consumer_health(&b->pmic);
        if(health<0){fault(b);return -1;}
        if(!health)return 0;
    } else if(b->pmic.self){fault(b);return -1;}
    const struct a733_clock_token *tokens[]={&b->clock_token,&b->rail_token};
    for(unsigned i=0;i<2;++i)if(tokens[i]->id) {
        enum a733_clock_result status=a733_clock_token_status(b->registry,tokens[i]);
        if(status==A733_CLOCK_BUSY)return 0;
        if(status!=A733_CLOCK_OK){fault(b);return -1;}
    }
    if(b->pck_token.token) {
        enum a733_gpu_resource_result status=a733_gpu_resources_status(&b->shared->manager,h->pck_client,&b->pck_token);
        if(status==A733_GPU_RESOURCE_BUSY)return 0;
        if(status!=A733_GPU_RESOURCE_OK){fault(b);return -1;}
    }
    return a733_npu_startup_ready(b) ? 1 : 0;
}
