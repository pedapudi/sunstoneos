/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_camera_native.h"
#include "a733_platform_locks.h"
#include "a733_counter_clock.h"
#include "a733_memory_reservations.h"
#include "aarch64_device_memory.h"
#include "task.h"
#include <string.h>
#define TABLE_BYTES 32768u
#define PARAMETER_BYTES 8192u
#define PLANE_BYTES (A733_CAMERA_NATIVE_PLANE_BYTES+A733_CAMERA_NATIVE_GUARD_BYTES)
#define MCLK1 UINT32_C(0x02003804)
static struct a733_camera_native *instance;
#ifdef SUNSTONE_CAMERA_MODEL
extern int a733_camera_model_current(void *);
extern uint32_t a733_camera_model_load(uintptr_t);
extern void a733_camera_model_store(uintptr_t,uint32_t);
#endif
static uint32_t load(uintptr_t a)
{
#ifdef SUNSTONE_CAMERA_MODEL
    return a733_camera_model_load(a);
#else
    __asm__ volatile("dsb sy" ::: "memory");uint32_t v=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory");return v;
#endif
}
static void store(uintptr_t a,uint32_t v)
{
#ifdef SUNSTONE_CAMERA_MODEL
    a733_camera_model_store(a,v);
#else
    __asm__ volatile("dsb sy" ::: "memory");*(volatile uint32_t *)a=v;
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
static int time_read(void *v,uint64_t *out)
{
    struct a733_camera_native *n=v;
    return n->config.locks->now(n->config.locks->context,out);
}
static int current(struct a733_camera_native *n)
{
#ifdef SUNSTONE_CAMERA_MODEL
    return a733_camera_model_current(n->config.task);
#else
    uint64_t el,flags,nesting,critical;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, daif" : "=r"(el),"=r"(flags));
    if(el!=4 || (flags&0x80))return 0;
    __asm__ volatile("msr daifset, #2" ::: "memory");
#if configNUMBER_OF_CORES > 1
    extern volatile uint64_t ullPortInterruptNestings[],ullCriticalNestings[];
    uint64_t core;__asm__ volatile("mrs %0, tpidr_el1" : "=r"(core));
    nesting=core<configNUMBER_OF_CORES ? ullPortInterruptNestings[core] : 1;
    critical=core<configNUMBER_OF_CORES ? ullCriticalNestings[core] : 1;
#else
    extern uint64_t ullPortInterruptNesting;
    extern volatile uint64_t ullCriticalNesting;
    nesting=ullPortInterruptNesting;critical=ullCriticalNesting;
#endif
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
    return !nesting && !critical && xTaskGetSchedulerState()==taskSCHEDULER_RUNNING &&
        xTaskGetCurrentTaskHandle()==(TaskHandle_t)n->config.task;
#endif
}
static int timely(struct a733_camera_native *n,uint64_t d)
{
    uint64_t t;
    if(d!=n->boot.deadline_us || time_read(n,&t)!=1 || t<n->last_now || t>=d)return 0;
    n->last_now=t;return 1;
}
static int memory_owned(struct a733_camera_native *n)
{
    struct device_memory_extent e;enum device_memory_state s;
    return device_memory_describe(n->pool,n->memory,&e,&s) && s==DEVICE_MEMORY_DEVICE &&
        e.cpu==n->extent.cpu && e.physical==n->extent.physical && e.bytes==n->extent.bytes;
}
/* Pin failure is protected by the same register mutex as PIO writes. The
 * retained pin lease forbids another client from changing these camera pins. */
static int pins_healthy(struct a733_camera_native *n)
{
    if(!n->pin.id)return 1;
    if(n->held)return !n->config.pins->failed;
    if(!a733_platform_lock_enter(n->config.locks,A733_PLATFORM_REGISTER_LOCK,n->boot.deadline_us))return 0;
    int ok=!n->config.pins->failed;
    a733_platform_lock_leave(n->config.locks,A733_PLATFORM_REGISTER_LOCK);return ok;
}
/* Registry BUSY means another task holds a registry's nonblocking metadata
 * lock; it is not a verdict on these leases. Repeat only the status reads,
 * never a hardware access, until the boot deadline. Any other result refuses. */
static int leases_live(struct a733_camera_native *n)
{
    for(uint64_t t;;){
        enum a733_clock_result c=a733_clock_token_status(n->config.clocks,&n->media_clock);
        if(c==A733_CLOCK_OK)c=a733_clock_token_status(n->config.clocks,&n->twi_clock);
        if(c==A733_CLOCK_OK) {
            enum a733_rail_result r=a733_rail_borrow_status(n->config.rails,&n->rail);
            if(r==A733_RAIL_READY)return 1;
            if(r!=A733_RAIL_BUSY)return 0;
        } else if(c!=A733_CLOCK_BUSY)return 0;
        if(time_read(n,&t)!=1 || t<n->last_now || t>=n->boot.deadline_us)return 0;
    }
}
static int authority(struct a733_camera_native *n)
{
    return n && n->self==n && instance==n && n->state!=A733_CAMERA_NATIVE_FAULT &&
        !memcmp(n->config.boot,&n->boot,sizeof(n->boot)) && current(n) && memory_owned(n) && pins_healthy(n) &&
        n->config.media->successor==n && !n->config.media->transferred &&
        a733_media_clock_prepared(n->config.media) &&
        !memcmp(&n->config.media->token,&n->media_clock,sizeof(n->media_clock)) &&
        leases_live(n);
}
static int fail(struct a733_camera_native *n)
{
    if(n->held)configASSERT(a733_platform_lock_retain(n->config.locks,A733_PLATFORM_REGISTER_LOCK)==1);
    n->state=A733_CAMERA_NATIVE_FAULT;return -1;
}
static int io_enter(struct a733_camera_native *n,uint64_t token,uint64_t d)
{
    if(token!=n->boot.lease || !authority(n) || !timely(n,d) || n->held ||
       !a733_platform_lock_enter(n->config.locks,A733_PLATFORM_REGISTER_LOCK,d))return 0;
    n->held=1;
    if(!authority(n) || !timely(n,d)){fail(n);return 0;}
    return 1;
}
static int io_end(struct a733_camera_native *n,uint64_t d)
{
    if(!authority(n) || !timely(n,d))return fail(n);
    a733_platform_lock_leave(n->config.locks,A733_PLATFORM_REGISTER_LOCK);n->held=0;
    return authority(n) && timely(n,d)?1:fail(n);
}
static int read_word(struct a733_camera_native *n,uint64_t token,uintptr_t a,uint32_t *v,uint64_t d)
{
    if(!v || (a&3) || !io_enter(n,token,d))return -1;
    *v=load(a);return io_end(n,d);
}
static int write_word(struct a733_camera_native *n,uint64_t token,uintptr_t a,uint32_t v,uint64_t d)
{
    if((a&3) || !io_enter(n,token,d))return -1;
    store(a,v);return io_end(n,d);
}
static int native_acquire(void *v,const struct a733_camera_capture_config *c,uint64_t d,uint64_t *t)
{
    struct a733_camera_native *n=v;
    if(c!=&n->capture.config || !authority(n) || !timely(n,d) || !n->twi_native.prepared)return -1;
    *t=n->boot.lease;return 1;
}
static int native_handoff(void *v,uint64_t t,enum a733_capture_handoff h,
    const struct a733_camera_capture *c,uint64_t d)
{
    struct a733_camera_native *n=v;
    if(t!=n->boot.lease || c!=&n->capture || !authority(n) || !timely(n,d) ||
       c->analog.state!=A733_CAMERA_PLATFORM_ACTIVE || c->iommu.state!=A733_IOMMU_ACTIVE ||
       c->analog.admission.token!=t || c->iommu.admission.token!=t ||
       c->route.state!=A733_ROUTE_ARMED || c->sensor.state!=OV5640_READY)return -1;
    if(h==A733_CAPTURE_LOCAL)return c->dma.state==A733_CAM_DMA_ARMED && c->receiver.state==A733_CSI_NEW?1:-1;
    return h==A733_CAPTURE_PACKETS && c->local_granted && c->receiver.state==A733_CSI_ARMED &&
        c->receiver.image_mode && c->dma.state==A733_CAM_DMA_START?1:-1;
}
/* The retained path never requests global drain, close or release. Refusal
 * makes an accidental transition explicit instead of manufacturing proof. */
static int no_drain(void *v,uint64_t t,uint64_t d){(void)v;(void)t;(void)d;return -1;}
static int analog_admit(void *v,uint64_t d,struct a733_camera_admission *a)
{
    struct a733_camera_native *n=v;
    if(!authority(n) || !timely(n,d) || n->capture.local_granted)return -1;
    *a=(struct a733_camera_admission){n->boot.lease,n->boot.proof,(enum a733_camera_origin)n->boot.origin};return 1;
}
static int analog_read(void *v,uint64_t t,uint16_t o,uint32_t *x,uint64_t d)
{
    switch(o){case 0:case 4:case 0x100:case 0x200:case 0x300:case 0x1000:case 0x1400:case 0x1800:break;default:return -1;}
    return read_word(v,t,0x05810000u+o,x,d);
}
static int analog_write(void *v,uint64_t t,uint16_t o,uint32_t x,uint64_t d)
{
    struct a733_camera_native *n=v;
    if(n->capture.local_granted || (o!=0 && o!=4) || (o==4 && (x&~0x1f7u)))return -1;
    return write_word(n,t,0x05810000u+o,x,d);
}
static int mmu_admit(void *v,const struct a733_iommu_tables *tables,uint64_t d,struct a733_iommu_admission *a)
{
    struct a733_camera_native *n=v;
    if(!authority(n) || !timely(n,d) || tables!=&n->capture.iommu.tables ||
       tables->words!=(void *)n->extent.cpu || tables->physical!=n->extent.physical)return -1;
    *a=(struct a733_iommu_admission){.token=n->boot.lease,
        .excluded_masters=(uint16_t)n->boot.excluded_masters,.powered_masters=(uint16_t)n->boot.powered_masters,
        .inherited_resolved=1,.transactions_drained=1,.clocks_held=1,
        .recovery_qualified=!!(n->boot.proof&A733_CAMERA_RECOVERY_QUALIFIED),
        .retained_power_removal=!!(n->boot.proof&A733_CAMERA_MANUAL_POWER_REMOVAL)};return 1;
}
static int mmu_read(void *v,uint64_t t,uint32_t o,uint32_t *x,uint64_t d)
{return o<0x20000?read_word(v,t,(uintptr_t)A733_IOMMU_BASE+o,x,d):-1;}
static int mmu_write(void *v,uint64_t t,uint32_t o,uint32_t x,uint64_t d)
{
    struct a733_camera_native *n=v;
    if(n->capture.packets_granted || o>=0x20000)return -1;
    return write_word(n,t,(uintptr_t)A733_IOMMU_BASE+o,x,d);
}
static int cache(struct a733_camera_native *n,enum aarch64_cache_op op,uintptr_t a,size_t bytes,uint64_t d)
{
    return authority(n) && timely(n,d) && aarch64_device_cache(op,a,bytes) &&
        timely(n,d) && authority(n)?1:-1;
}
static int mmu_publish(void *v,uint64_t t,const void *p,size_t bytes,uint64_t d)
{
    struct a733_camera_native *n=v;
    if(t!=n->boot.lease || p!=(void *)n->extent.cpu || bytes!=TABLE_BYTES || n->capture.packets_granted)return -1;
    return cache(n,AARCH64_CACHE_CLEAN,(uintptr_t)p,bytes,d);
}
static int sensor_acquire(void *v,uint64_t d,uint64_t *t)
{
    struct a733_camera_native *n=v;
    if(!authority(n) || !timely(n,d) || !n->twi_native.prepared || n->twi.state!=A733_TWI_READY)return -1;
    *t=n->boot.lease;return 1;
}
static int sensor_line(void *v,uint64_t t,enum ov5640_line line,int asserted,uint64_t d)
{
    struct a733_camera_native *n=v;
    if(t!=n->boot.lease || !authority(n) || !timely(n,d) || (asserted!=0 && asserted!=1))return -1;
    unsigned b,p,value;
    if(line==OV5640_PWDN_LINE){b=A733_PIN_E;p=6;value=(unsigned)asserted;}
    else if(line==OV5640_RESET_LINE && !n->boot.pwdn_only){b=n->boot.reset_bank;p=n->boot.reset_pin;value=!asserted;}
    else return -1;
    return a733_pins_write_outputs(n->config.pins,&n->pin,b,1u<<p,value<<p,d)==A733_PINS_OK &&
        timely(n,d) && authority(n)?1:-1;
}
static int sensor_bus(struct a733_camera_native *n,uint64_t t,uint8_t address,uint16_t reg,
    unsigned reading,uint8_t *data,unsigned count,uint64_t d)
{
    if(t!=n->boot.lease || address!=0x3c || !data || !authority(n) || !timely(n,d))return -1;
    /* Bounded synchronous bridge preserves the sensor controller's current
     * callback contract. One transaction gets at most 10 ms within capture time;
     * a 40-byte block write takes about 4 ms at 100 kHz. */
    uint64_t limit=d;
    if(d-n->last_now>10000)limit=n->last_now+10000;
    if(a733_twi_request_block(&n->twi,address,reg,reading,data,count,limit)<0)return -1;
    int r;
    do {r=a733_twi_poll(&n->twi);}while(r==0);
    return r==1 && a733_twi_take_block(&n->twi,data,count)==1 && timely(n,d) && authority(n)?1:-1;
}
static int sensor_read(void *v,uint64_t t,uint8_t a,uint16_t r,uint8_t *x,uint64_t d)
{uint8_t byte=0;int ok=sensor_bus(v,t,a,r,1,&byte,1,d);if(ok==1)*x=byte;return ok;}
static int sensor_write(void *v,uint64_t t,uint8_t a,uint16_t r,uint8_t x,uint64_t d)
{return sensor_bus(v,t,a,r,0,&x,1,d);}
static int sensor_write_block(void *v,uint64_t t,uint8_t a,uint16_t r,const uint8_t *x,unsigned count,uint64_t d)
{
    uint8_t block[OV5640_BLOCK_BYTES];
    if(!x || count<2 || count>OV5640_BLOCK_BYTES)return -1;
    memcpy(block,x,count);return sensor_bus(v,t,a,r,0,block,count,d);
}
static int sensor_receiver(void *v,uint64_t t,uint64_t rt,const struct ov5640_mode *m,uint64_t d)
{
    struct a733_camera_native *n=v;
    return t==n->boot.lease && rt==t && authority(n) && timely(n,d) &&
        n->capture.packets_granted && n->capture.receiver.state==A733_CSI_ARMED &&
        m->width==640 && m->height==480 && m->lanes==2 && m->xclk_hz==24000000?1:-1;
}
static int route_admit(void *v,const struct a733_route_config *c,uint64_t d,uint64_t *t)
{
    struct a733_camera_native *n=v;
    if(c->parser || c->parameter_address!=n->extent.physical+TABLE_BYTES ||
       c->mapped_bytes!=PARAMETER_BYTES || !authority(n) || !timely(n,d))return -1;
    *t=n->boot.lease;return 1;
}
static int route_publish(void *v,uint64_t t,const uint8_t image[1024],uint64_t d)
{
    struct a733_camera_native *n=v;
    if(t!=n->boot.lease || !authority(n) || !timely(n,d) || n->capture.packets_granted)return -1;
    uintptr_t p=n->extent.cpu+TABLE_BYTES;
    memset((void *)p,0,PARAMETER_BYTES);memcpy((void *)p,image,1024);
    return cache(n,AARCH64_CACHE_CLEAN,p,PARAMETER_BYTES,d);
}
static int route_read(void *v,uint64_t t,enum a733_route_region r,uint16_t o,uint32_t *x,uint64_t d)
{
    if((unsigned)r>A733_ROUTE_VIPP || o>=(r==A733_ROUTE_VIPP?0x140:0x100))return -1;
    return read_word(v,t,(r==A733_ROUTE_TOP?0x05800800u:0x05910000u)+o,x,d);
}
static int route_write(void *v,uint64_t t,enum a733_route_region r,uint16_t o,uint32_t x,uint64_t d)
{
    if((unsigned)r>A733_ROUTE_VIPP || (r==A733_ROUTE_TOP && o!=0x40 && o!=0xa0) ||
       (r==A733_ROUTE_VIPP && o!=0 && o!=0x30 && o!=0x40 && o!=0x50))return -1;
    return write_word(v,t,(r==A733_ROUTE_TOP?0x05800800u:0x05910000u)+o,x,d);
}
static int csi_admit(void *v,const struct a733_csi_config *c,uint64_t d,uint64_t *t)
{
    struct a733_camera_native *n=v;
    if(c->port || c->parser || c->settle!=n->boot.settle || c->deskew!=n->boot.deskew ||
       c->top_hz!=324000000 || !n->capture.local_granted || !authority(n) || !timely(n,d))return -1;
    *t=n->boot.lease;return 1;
}
static uintptr_t csi_address(enum a733_csi_region r,uint16_t o)
{
    static const uintptr_t base[]={0x05810000,0x05810100,0x05811000,0x05820000};
    return (unsigned)r<4 && o<(r==A733_CSI_PORT?0x400:0x100)?base[r]+o:0;
}
static int csi_read(void *v,uint64_t t,enum a733_csi_region r,uint16_t o,uint32_t *x,uint64_t d)
{uintptr_t a=csi_address(r,o);return a?read_word(v,t,a,x,d):-1;}
static int csi_write(void *v,uint64_t t,enum a733_csi_region r,uint16_t o,uint32_t x,uint64_t d)
{
    struct a733_camera_native *n=v;uintptr_t a=csi_address(r,o);
    if(!a || r==A733_CSI_TOP || !n->capture.local_granted)return -1;
    if(n->capture.packets_granted) {
        /* After the first image only status acknowledgement and the SCAP
         * rearm are accepted, both with the sensor stopped. */
        enum a733_capture_state s=n->capture.state;
        unsigned status=(s==A733_CAPTURE_SAMPLE || s==A733_CAPTURE_REARM_SAMPLE) &&
            n->capture.receiver.state==A733_CSI_SAMPLE;
        unsigned rearm=s==A733_CAPTURE_REARM_RECEIVER && n->capture.receiver.state==A733_CSI_REARM;
        if(n->capture.sensor.state!=OV5640_READY ||
           !((status && r==A733_CSI_PORT && o==0x118 && !(x&~0x1fffu)) ||
             (status && r==A733_CSI_PARSER && o==0x44 && !(x&~7u)) ||
             (rearm && r==A733_CSI_PARSER && o==0x0c && (x&0x03030303)==1)))return -1;
    }
    return write_word(v,t,a,x,d);
}
static int dma_admit(void *v,const struct a733_camera_dma_config *c,uint64_t d,uint64_t *t)
{
    struct a733_camera_native *n=v;uint64_t y=n->extent.physical+TABLE_BYTES+PARAMETER_BYTES;
    if(c->block || c->frame_id!=n->boot.lease || c->y.bus_address!=y || c->uv.bus_address!=y+PLANE_BYTES ||
       c->y.mapped_bytes!=PLANE_BYTES || c->uv.mapped_bytes!=PLANE_BYTES ||
       !authority(n) || !timely(n,d))return -1;
    *t=n->boot.lease;
    uintptr_t planes=n->extent.cpu+TABLE_BYTES+PARAMETER_BYTES;
    /* A rearm refills the planes, so a short next frame cannot leave bytes
     * of the previous image. The channel is acknowledged and inactive, the
     * parser fence is clear and the sensor is stopped. */
    if(n->capture.dma.state==A733_CAM_DMA_REARM){
        if(n->capture.state!=A733_CAPTURE_REARM_DMA || n->capture.sensor.state!=OV5640_READY ||
           n->state!=A733_CAMERA_NATIVE_CAPTURE)return -1;
        memset((void *)planes,A733_CAMERA_NATIVE_FILL,2u*PLANE_BYTES);
    }
    return cache(n,AARCH64_CACHE_CLEAN_INVALIDATE,planes,2u*PLANE_BYTES,d);
}
static int dma_read(void *v,uint64_t t,uint16_t o,uint32_t *x,uint64_t d)
{return o<0x1000?read_word(v,t,0x05830000u+o,x,d):-1;}
static int dma_write(void *v,uint64_t t,uint16_t o,uint32_t x,uint64_t d)
{return o<0x1000?write_word(v,t,0x05830000u+o,x,d):-1;}
static int dma_sync(void *v,uint64_t t,uint64_t d)
{
    struct a733_camera_native *n=v;
    if(t!=n->boot.lease || !n->capture.retained_readout || !n->capture.scanned ||
       !n->capture.sampled || !n->capture.fenced || n->capture.route.events!=0x100)return -1;
    return cache(n,AARCH64_CACHE_INVALIDATE,n->extent.cpu+TABLE_BYTES+PARAMETER_BYTES,2u*PLANE_BYTES,d);
}
static int boot_valid(const struct a733_camera_boot_record *b,unsigned supervised)
{
    if(b->version!=(supervised?2u:1u) || b->bytes!=sizeof(*b) || !b->owner || !b->epoch || !b->generation ||
       !b->lease || !b->deadline_us || !(b->build[0]|b->build[1]|b->build[2]|b->build[3]) ||
       !(b->evidence[0]|b->evidence[1]|b->evidence[2]|b->evidence[3]) ||
       b->proof!=(supervised?A733_CAMERA_SUPERVISED_PROOF:A733_CAMERA_REQUIRED_PROOF) ||
       (b->origin!=A733_CAMERA_COLD_RESET && b->origin!=A733_CAMERA_RESOLVED_RESET_HANDOFF) ||
       b->excluded_masters!=A733_IOMMU_REAL_MASTERS || b->powered_masters!=A733_IOMMU_REAL_MASTERS ||
       b->raw_trim!=0xb00 || b->parameter_guard_bytes!=4096 || b->plane_guard_bytes!=4096 ||
       !b->settle || !b->deskew || b->deskew>31 || b->test_pattern>1 || b->pwdn_only>1 ||
       b->discard_frames>A733_CAMERA_NATIVE_DISCARD_LIMIT ||
       b->inherited[11].function!=1)return 0;
    if(b->pwdn_only) {
        const struct a733_pin_config *r=&b->inherited[14];
        if(b->reset_bank || b->reset_pin || r->function || r->pull || r->drive)return 0;
    } else if(b->reset_bank<A733_PIN_B || b->reset_bank>A733_PIN_K || b->reset_pin>=32 ||
       !(a733_pin_bank_mask(b->reset_bank)&(1u<<b->reset_pin)) ||
       (b->reset_bank==A733_PIN_E && b->reset_pin>=5 && b->reset_pin<=8) ||
       (b->reset_bank==A733_PIN_K && b->reset_pin<10) ||
       b->inherited[14].function!=1)return 0;
    for(unsigned i=0;i<3;i++){
        if(!b->programmed_uv[i])return 0;
        switch(b->rails[i]) {
        case A733_RAIL_DCDC1:case A733_RAIL_DCDC2:case A733_RAIL_DCDC4:case A733_RAIL_DCDC8:
        case A733_RAIL_BLDO2:case A733_RAIL_BLDO5:case A733_RAIL_CLDO1:case A733_RAIL_CLDO5:
        case A733_RAIL_DC1SW2:break;
        default:return 0;
        }
        for(unsigned j=0;j<i;j++)if(b->rails[j]==b->rails[i])return 0;
    }
    return 1;
}
extern int a733_smp_device_preparation_admitted(void);
static int initialize(struct a733_camera_native *n,const struct a733_camera_native_config *c,unsigned supervised)
{
    if(!n || n->self || instance || !c || !c->boot || !c->media || !c->clocks || !c->pins ||
       !c->rails || !c->rail_parent || !c->pmic || !c->locks || !c->task ||
       c->pmic->self!=c->pmic || !c->pmic->ops.outside ||
       !c->locks->initialized || c->locks->now!=a733_counter_clock_now || !c->locks->context ||
       c->clocks->now!=c->locks->now || c->clocks->context!=c->locks->context ||
       c->media->registry!=c->clocks || c->media->config.owner!=c->boot->owner ||
       (c->media->config.groups&(A733_MEDIA_VI|A733_MEDIA_IOMMU))!=(A733_MEDIA_VI|A733_MEDIA_IOMMU) ||
       c->media->config.camera_mclk_mask!=2 || c->media->config.deadline_us!=c->boot->deadline_us ||
       c->pmic->clocks!=c->clocks || c->pmic->rails!=c->rails || c->pmic->pins!=c->pins ||
       c->rail_parent->owner!=c->boot->owner ||
       !boot_valid(c->boot,supervised) || !separate(n,sizeof(*n),c,sizeof(*c)) ||
       !separate(n,sizeof(*n),c->boot,sizeof(*c->boot)) ||
       !separate(n,sizeof(*n),c->media,sizeof(*c->media)) ||
       !separate(n,sizeof(*n),c->clocks,sizeof(*c->clocks)) ||
       !separate(n,sizeof(*n),c->pins,sizeof(*c->pins)) ||
       !separate(n,sizeof(*n),c->rails,sizeof(*c->rails)) ||
       !separate(n,sizeof(*n),c->rail_parent,sizeof(*c->rail_parent)) ||
       !separate(n,sizeof(*n),c->pmic,sizeof(*c->pmic)) ||
       !separate(n,sizeof(*n),c->locks,sizeof(*c->locks)) ||
       !separate(n,sizeof(*n),c->locks->context,sizeof(struct a733_counter_clock)) ||
       !a733_smp_device_preparation_admitted() ||
       c->pmic->ops.outside(c->pmic->cookie,n,sizeof(*n))!=1 ||
       c->pmic->ops.outside(c->pmic->cookie,c,sizeof(*c))!=1 ||
       c->pmic->ops.outside(c->pmic->cookie,c->boot,sizeof(*c->boot))!=1)return 0;
    struct device_memory_pool *pool=a733_memory_reservations_init();
    if(!pool || !separate(n,sizeof(*n),(void *)pool->backing.cpu,pool->backing.bytes) ||
       !separate(c,sizeof(*c),(void *)pool->backing.cpu,pool->backing.bytes) ||
       !separate(c->boot,sizeof(*c->boot),(void *)pool->backing.cpu,pool->backing.bytes))return 0;
    *n=(struct a733_camera_native){.self=n,.config=*c,.boot=*c->boot,.pool=pool,.state=A733_CAMERA_NATIVE_CLOCK};
    instance=n;
    if(!device_memory_acquire_aligned(n->pool,n->boot.owner,A733_CAMERA_NATIVE_BYTES,16384,&n->memory,&n->extent))return fail(n);
    struct device_memory_allocation *a=device_memory_lookup(n->pool,n->memory);
    if(!a || a->state!=DEVICE_MEMORY_CPU)return fail(n);
    /* Validate the geometry before any write. A refused extent stays device
     * owned, as it did when this check followed the fill. */
    if(n->extent.cpu!=n->extent.physical || n->extent.physical>=UINT64_C(0x100000000) ||
       n->extent.bytes!=A733_CAMERA_NATIVE_BYTES || n->extent.bytes>UINT64_C(0x100000000)-n->extent.physical){
        a->state=DEVICE_MEMORY_DEVICE;return fail(n);}
    /* Unwritten payload or guard bytes then cannot repeat stale DRAM, such as
     * an earlier image. dma_admit's clean/invalidate publishes this fill. */
    memset((void *)(n->extent.cpu+TABLE_BYTES+PARAMETER_BYTES),A733_CAMERA_NATIVE_FILL,2u*PLANE_BYTES);
    a->state=DEVICE_MEMORY_DEVICE;
    uintptr_t p=n->extent.cpu;
    struct a733_camera_capture_config capture={
        .ops={time_read,native_acquire,native_handoff,no_drain},.context=n,
        .memory={{(void *)(p+TABLE_BYTES),p+TABLE_BYTES,p+TABLE_BYTES,PARAMETER_BYTES},
          {(void *)(p+TABLE_BYTES+PARAMETER_BYTES),p+TABLE_BYTES+PARAMETER_BYTES,p+TABLE_BYTES+PARAMETER_BYTES,PLANE_BYTES},
          {(void *)(p+TABLE_BYTES+PARAMETER_BYTES+PLANE_BYTES),p+TABLE_BYTES+PARAMETER_BYTES+PLANE_BYTES,
            p+TABLE_BYTES+PARAMETER_BYTES+PLANE_BYTES,PLANE_BYTES}},
        .table_words=(void *)p,.table_physical=p,.frame_id=n->boot.lease,
        .receiver={0,0,n->boot.settle,n->boot.deskew,324000000},.mode=ov5640_vga_mode,
        .color_bars=n->boot.test_pattern,
        .sensor={time_read,sensor_acquire,sensor_line,sensor_read,sensor_write,sensor_receiver,no_drain,sensor_write_block},
        .analog={time_read,analog_admit,analog_read,analog_write},
        .iommu={time_read,mmu_admit,mmu_read,mmu_write,mmu_publish},
        .route={time_read,route_admit,route_publish,route_read,route_write,no_drain,no_drain,no_drain},
        .csi={time_read,csi_admit,csi_read,csi_write,no_drain,no_drain,no_drain},
        .dma={time_read,dma_admit,dma_read,dma_write,no_drain,no_drain,no_drain,dma_sync,no_drain}};
    if((supervised?a733_camera_capture_supervised_init(&n->capture,&capture):a733_camera_capture_init(&n->capture,&capture))!=1 ||
       (n->boot.pwdn_only && ov5640_pwdn_only(&n->capture.sensor)!=1))return fail(n);
    return 1;
}
int a733_camera_native_init(struct a733_camera_native *n,const struct a733_camera_native_config *c)
{return initialize(n,c,0);}
int a733_camera_native_supervised_init(struct a733_camera_native *n,const struct a733_camera_native_config *c)
{return initialize(n,c,1);}
/* A PWDN-only module claims the first fourteen pins; RESETB needs no GPIO. */
static int pin_acquire(struct a733_camera_native *n)
{
    struct a733_pin_request_banked r[A733_CAMERA_NATIVE_PIN_COUNT]={0};
    unsigned count=n->boot.pwdn_only?A733_CAMERA_NATIVE_PIN_COUNT-1:A733_CAMERA_NATIVE_PIN_COUNT;
    for(unsigned i=0;i<10;i++){r[i].bank=A733_PIN_K;r[i].pin=i;r[i].desired=(struct a733_pin_config){2,0,0};}
    r[10].bank=A733_PIN_E;r[10].pin=5;r[10].desired=(struct a733_pin_config){2,0,0};
    r[11].bank=A733_PIN_E;r[11].pin=6;r[11].desired=(struct a733_pin_config){1,0,0};r[11].output_value=1;
    for(unsigned i=12;i<14;i++){r[i].bank=A733_PIN_E;r[i].pin=i-5;r[i].desired=(struct a733_pin_config){8,0,0};}
    if(!n->boot.pwdn_only){r[14].bank=n->boot.reset_bank;r[14].pin=n->boot.reset_pin;r[14].desired=(struct a733_pin_config){1,0,0};}
    for(unsigned i=0;i<count;i++)r[i].expected=n->boot.inherited[i];
    struct a733_pin_output_condition condition={0};
    condition.masks[A733_PIN_E]=1u<<6;condition.levels[A733_PIN_E]=1u<<6;
    if(!n->boot.pwdn_only)condition.masks[n->boot.reset_bank]|=1u<<n->boot.reset_pin;
    enum a733_pins_result result=a733_pins_acquire_banked_checked(n->config.pins,n->boot.owner,
        r,count,&condition,&n->pin,n->boot.deadline_us);
    return result==A733_PINS_OK?1:result==A733_PINS_BUSY?0:-1;
}
static int mclk_enable(struct a733_camera_native *n)
{
    uint64_t d=n->boot.deadline_us;
    if(!io_enter(n,n->boot.lease,d))return -1;
    uint32_t v=load(MCLK1);
    if(v&0x87001f1fu)return fail(n); /* Prepared 24 MHz, both dividers 1, gate off. */
    store(MCLK1,v|0x80000000u);
    if(load(MCLK1)!=(v|0x80000000u))return fail(n);
    return io_end(n,d);
}
int a733_camera_native_poll(struct a733_camera_native *n)
{
    if(!n || n->self!=n || instance!=n || n->state==A733_CAMERA_NATIVE_FAULT)return -1;
    if(!current(n))return -1;
    if(n->entered)return fail(n);
    if(memcmp(n->config.boot,&n->boot,sizeof(n->boot)) ||
       !timely(n,n->boot.deadline_us))return fail(n);
    if(n->state==A733_CAMERA_NATIVE_READABLE)return authority(n)?1:fail(n);
    n->entered=1;int r=0;
    switch(n->state) {
    case A733_CAMERA_NATIVE_CLOCK: {
        enum a733_media_clock_result m=a733_media_clock_poll(n->config.media);
        if(m==A733_MEDIA_BUSY || m==A733_MEDIA_PENDING)break;
        if(m!=A733_MEDIA_PREPARED || (n->config.media->successor && n->config.media->successor!=n) ||
           n->config.media->transferred){r=-1;break;}
        n->config.media->successor=n;n->media_clock=n->config.media->token;
        a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC;
        if(n->boot.apb1>>24==3)shared|=A733_CLOCK_PERI0;
        enum a733_clock_result c=a733_clock_acquire(n->config.clocks,n->boot.owner,shared,
            A733_CLOCK_TWI11,n->boot.deadline_us,&n->twi_clock);
        if(c==A733_CLOCK_BUSY)break;
        if(c!=A733_CLOCK_OK){r=-1;break;}n->state=A733_CAMERA_NATIVE_RAIL;break;
    }
    case A733_CAMERA_NATIVE_RAIL: {
        a733_rail_mask_t mask=0;for(unsigned i=0;i<3;i++)mask|=A733_RAIL_BIT(n->boot.rails[i]);
        enum a733_rail_result rr=a733_rail_borrow(n->config.rails,n->config.rail_parent,mask,&n->rail);
        if(rr==A733_RAIL_BUSY)break;
        if(rr!=A733_RAIL_READY){r=-1;break;}n->state=A733_CAMERA_NATIVE_SNAPSHOT;break;
    }
    case A733_CAMERA_NATIVE_SNAPSHOT:
        if(!authority(n)){r=-1;break;}
        if(n->snapshot==0){
            enum a733_pmic_status p=a733_pmic_submit(n->config.pmic,n->boot.owner,n->config.rail_parent,
                n->boot.rails[n->index],n->boot.deadline_us,&n->request);
            if(p==A733_PMIC_BUSY)break;
            if(p!=A733_PMIC_PENDING){r=-1;break;}
            if(n->request.id)n->snapshot=1;break;
        }
        if(n->snapshot==1){
            struct a733_pmic_snapshot s;
            enum a733_pmic_status p=a733_pmic_result(n->config.pmic,&n->request,&s);
            if(p==A733_PMIC_PENDING || p==A733_PMIC_BUSY)break;
            if(p!=A733_PMIC_COMPLETE || !s.enabled || !s.decoded || s.rail!=n->boot.rails[n->index] ||
               s.programmed_uv!=n->boot.programmed_uv[n->index]){r=-1;break;}
            n->snapshot=2;break;
        }
        {enum a733_pmic_status p=a733_pmic_reap(n->config.pmic,&n->request);
        if(p==A733_PMIC_BUSY)break;
        if(p!=A733_PMIC_COMPLETE){r=-1;break;}
        memset(&n->request,0,sizeof(n->request));n->snapshot=0;
        if(++n->index==3)n->state=A733_CAMERA_NATIVE_PINS;break;}
    case A733_CAMERA_NATIVE_PINS:
        r=pin_acquire(n);if(r==1){n->state=A733_CAMERA_NATIVE_TWI;r=0;}break;
    case A733_CAMERA_NATIVE_TWI: {
        struct a733_twi_native_config c={n->config.clocks,n->twi_clock,n->config.pins,n->pin,
            n->config.rails,&n->rail,n->config.locks,n->config.task,n->boot.apb1,n->boot.apb1_hz,
            A733_TWI_MAIN11};
        if(!n->twi_native.self && !a733_twi_native_init(&n->twi_native,&c)){r=-1;break;}
        int ready=a733_twi_native_prepare(&n->twi_native,n->boot.deadline_us);
        if(!ready)break;
        if(ready!=1 || !a733_twi_init(&n->twi,&n->twi_native.operations,&n->twi_native,n->twi_native.divider)){r=-1;break;}
        n->state=A733_CAMERA_NATIVE_MCLK;break;
    }
    case A733_CAMERA_NATIVE_MCLK:
        if(mclk_enable(n)!=1 || a733_camera_capture_open_retained(&n->capture,n->boot.deadline_us)!=1){r=-1;break;}
        n->state=A733_CAMERA_NATIVE_CAPTURE;break;
    case A733_CAMERA_NATIVE_CAPTURE:
        if(!authority(n)){r=-1;break;}
        r=a733_camera_capture_poll(&n->capture);
        if(r==1){
            if(n->capture.state!=A733_CAPTURE_READABLE || n->capture.dma.owner!=A733_CAMERA_DEVICE ||
               !authority(n)){r=-1;break;}
            /* Discarded images were never visible, so no view can remain. */
            if(n->discarded<n->boot.discard_frames){
                r=a733_camera_capture_rearm(&n->capture,n->boot.deadline_us)==1?0:-1;
                ++n->discarded;break;
            }
            n->state=A733_CAMERA_NATIVE_READABLE;
        }break;
    default:r=-1;break;
    }
    n->entered=0;
    if(r<0 || !timely(n,n->boot.deadline_us))return fail(n);
    return n->state==A733_CAMERA_NATIVE_READABLE?1:0;
}
int a733_camera_native_copy(struct a733_camera_native *n,unsigned plane,unsigned offset,
    struct device_memory_lease destination,size_t destination_offset,size_t bytes)
{
    struct device_memory_extent out;enum device_memory_state owner;
    if(!n || n->self!=n || n->state!=A733_CAMERA_NATIVE_READABLE || n->entered || plane>1 ||
       offset>A733_CAMERA_NATIVE_PLANE_BYTES || !bytes || bytes>A733_CAMERA_NATIVE_PLANE_BYTES-offset ||
       destination.owner!=n->boot.owner ||
       !device_memory_describe(n->pool,destination,&out,&owner) || owner!=DEVICE_MEMORY_CPU ||
       destination_offset>out.bytes || bytes>out.bytes-destination_offset ||
       !authority(n) || !timely(n,n->boot.deadline_us))return -1;
    n->entered=1;uintptr_t dst=out.cpu+destination_offset;
    uintptr_t src=n->extent.cpu+TABLE_BYTES+PARAMETER_BYTES+plane*PLANE_BYTES+offset;
    for(size_t done=0;done<bytes;){
        size_t count=4096;if(count>bytes-done)count=bytes-done;
        if(!authority(n) || !timely(n,n->boot.deadline_us)){
            n->entered=0;return fail(n);
        }
        memcpy((void *)(dst+done),(const void *)(src+done),count);done+=count;
    }
    n->entered=0;
    return timely(n,n->boot.deadline_us) && authority(n)?1:fail(n);
}
