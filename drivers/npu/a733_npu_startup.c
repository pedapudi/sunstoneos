#include "a733_npu_startup.h"
#include "a733_npu_platform_native.h"

#define READY A733_NPU_PLATFORM_READY
#define BUSY A733_NPU_PLATFORM_BUSY
#define PENDING A733_NPU_PLATFORM_PENDING
#define RETAINED A733_NPU_PLATFORM_RETAINED
enum { CLOCK, RAIL, PCK, FIRST_SNAPSHOT, SETTLE, SECOND_SNAPSHOT, RESOURCES_READY };
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
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
        !separate(s,sizeof(*s),clock,sizeof(*clock)) || !separate(s,sizeof(*s),locks,sizeof(*locks)) ||
        !separate(clock,sizeof(*clock),locks,sizeof(*locks)) ||
        locks->now!=a733_counter_clock_now || locks->context!=clock)return 0;
    s->clock=clock;s->locks=locks;
    if(!a733_gpu_resources_native_init(&s->native,&locks->callbacks))return 0;
    const struct a733_gpu_resource_ops ops={resource_now,resource_read,resource_update,resource_rail};
    if(!a733_gpu_resources_init(&s->manager,&ops,s))return 0;
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
static int rail_snapshot(struct a733_npu_startup *b)
{
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
    out->clock=b->clock_token;out->rail=b->rail_token.id;out->pck=b->pck_token.token;
    out->supply_uv=b->stage==RESOURCES_READY ? A733_NPU_INITIAL_UV : 0;
}
static enum a733_npu_platform_result acquire(void *v,const struct a733_npu_platform_config *c,
    uint32_t shared,struct a733_npu_platform_resources *out)
{
    struct a733_npu_startup *b=v;uint32_t expected;
    if(!b || !b->initialized)return RETAINED;
    if(!task_owner(b) || b->register_held || b->i2c_held || !c || !out ||
        !exact_config(c,&b->handoff.allowed) || !a733_npu_platform_scopes(c,&expected) || shared!=expected ||
        !same_clock(&out->clock,&b->clock_token) || out->rail!=b->rail_token.id || out->pck!=b->pck_token.token ||
        b->handoff.evidence!=c->handoff || b->handoff.established!=A733_NPU_HANDOFF_REQUIRED)return fault(b);
    if(!timely(b,0))return fault(b);
    enum a733_clock_result cr;
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
        if(!rail_snapshot(b))return fault(b);
        if(b->stage==FIRST_SNAPSHOT) {
            uint64_t now;if(!timely(b,&now) || now>UINT64_MAX-A733_NPU_RAIL_SETTLE_US)return fault(b);
            b->settle_until=now+A733_NPU_RAIL_SETTLE_US;++b->stage;
        } else b->stage=RESOURCES_READY;
    } else if(b->stage==SETTLE) {
        uint64_t now;if(!timely(b,&now))return fault(b);
        if(now>=b->settle_until)++b->stage;
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
static int now(void *v,uint64_t *out)
{struct a733_npu_startup *b=v;return task_owner(b) && resource_now(b->shared,out);}
static int read_reg(void *v,uintptr_t address,uint32_t *out)
{
    struct a733_npu_startup *b=v;
    if(!task_owner(b) || !b->register_held)return 0;
    int ok=a733_npu_platform_native_read(0,address,out);if(!ok)(void)fault(b);return ok;
}
static int write_reg(void *v,uintptr_t address,uint32_t value)
{
    struct a733_npu_startup *b=v;
    if(!task_owner(b) || !b->register_held)return 0;
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
    uint32_t shared;
    if(!privileged() || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED || !b || b->initialized ||
        !h || !s || !s->initialized || !registry || !registry->initialized ||
        !separate(b,sizeof(*b),h,sizeof(*h)) || !separate(b,sizeof(*b),s,sizeof(*s)) ||
        !separate(b,sizeof(*b),registry,sizeof(*registry)) || !separate(s,sizeof(*s),registry,sizeof(*registry)) ||
        !separate(h,sizeof(*h),s,sizeof(*s)) || !separate(h,sizeof(*h),registry,sizeof(*registry)) ||
        !separate(b,sizeof(*b),s->clock,sizeof(*s->clock)) || !separate(b,sizeof(*b),s->locks,sizeof(*s->locks)) ||
        !separate(h,sizeof(*h),s->clock,sizeof(*s->clock)) || !separate(h,sizeof(*h),s->locks,sizeof(*s->locks)) ||
        !separate(registry,sizeof(*registry),s->clock,sizeof(*s->clock)) ||
        !separate(registry,sizeof(*registry),s->locks,sizeof(*s->locks)) ||
        registry->now!=a733_counter_clock_now || registry->context!=s->clock ||
        s->manager.ops.now!=resource_now || s->manager.context!=s ||
        h->controller!=0x03600000 || h->evidence!=h->allowed.handoff ||
        h->established!=A733_NPU_HANDOFF_REQUIRED || h->pck_client>=A733_GPU_RESOURCE_CLIENTS ||
        !a733_npu_platform_scopes(&h->allowed,&shared) || !valid_backend(h))return 0;
    b->handoff=*h;b->shared=s;b->registry=registry;
    b->operations=(struct a733_npu_platform_operations){acquire,enter,leave,now,read_reg,write_reg};
    if(!a733_npu_platform_init(&b->platform,&b->handoff.allowed,&b->operations,b))return 0;
    b->initialized=1;return 1;
}
int a733_npu_startup_bind_task(struct a733_npu_startup *b,TaskHandle_t task)
{
    if(!b || !b->initialized || b->failed || b->task || !task || !privileged() ||
        xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    b->task=task;return 1;
}
enum a733_npu_platform_result a733_npu_startup_poll(struct a733_npu_startup *b)
{
    if(!task_owner(b))return RETAINED;
    enum a733_npu_platform_result r=a733_npu_platform_poll(&b->platform);
    if(r==RETAINED)(void)fault(b);
    return r;
}
int a733_npu_startup_ready(const struct a733_npu_startup *b)
{return b && b->initialized && !b->failed && a733_npu_platform_ready(&b->platform);}
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
    if(!same_clock(&r->clock,&b->clock_token) || r->rail!=b->rail_token.id || r->pck!=b->pck_token.token){fault(b);return -1;}
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
