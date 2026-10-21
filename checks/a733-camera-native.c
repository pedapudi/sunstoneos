/* SPDX-License-Identifier: GPL-2.0-only */
/* The native binding and real controllers execute against modeled MMIO/cache
 * and task identity. This fixture supplies no physical qualification evidence. */
#define _GNU_SOURCE
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include "a733_camera_native.h"
#include "a733_platform_locks.h"
#include "a733_counter_clock.h"
#include "a733_memory_reservations.h"
#include "aarch64_device_memory.h"
#include "task.h"
#include <sys/mman.h>
#include <stdlib.h>

static struct pmic_model pm;
static struct a733_camera_native camera;
static struct a733_media_clock media;
static struct a733_platform_locks locks;
static struct a733_counter_clock counter;
static struct a733_rail_token supply;
static struct a733_camera_boot_record boot;
static struct device_memory_pool pool;
static uint32_t common[0x2000/4],parser[0x1000/4],route[0x100/4],vipp[0x140/4],dma[0x1000/4],mmu[0x20000/4];
static uint8_t sensor[65536],device_image[2*(307200+4096)];
static unsigned wire,selected,frames,cache_syncs,io_count,stops,mode,fills,busy_seen;
static uint64_t pwdn_released,identity_read,busy_until;
static void *const camera_task=(void *)0x1234;
static unsigned preparation=1,reject_camera;
#define WORD(a) (*pmic_model_word(&pm,(a)))

/* Modes 12/13 model another task inside the clock and rail registries. Mode 12
 * leaves after a bounded interval; mode 13 never leaves before the deadline. */
int a733_counter_clock_now(void *context,uint64_t *out)
{
    assert(context==&counter);*out=++pm.now;
    if(busy_until && pm.now>=busy_until)atomic_store(&pm.clocks.busy,0);
    if(busy_until && pm.now>=busy_until+20){atomic_store(&pm.rails.busy,0);busy_until=0;}
    return 1;
}
int a733_smp_device_preparation_admitted(void){return preparation;}
struct device_memory_pool *a733_memory_reservations_init(void){return &pool;}
int a733_camera_model_current(void *task){return task==camera_task && !pm.worker;}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t q)
{
    unsigned held=q==locks.mutex[0]?pm.register_held:pm.i2c_held;
    return held?(TaskHandle_t)camera_task:NULL;
}
_Noreturn void platform_panic(const char *file,unsigned line){fprintf(stderr,"assert %s:%u\n",file,line);abort();}
int a733_platform_lock_enter(struct a733_platform_locks *p,enum a733_platform_lock which,uint64_t d)
{
    assert(p==&locks && !pm.worker && d>pm.now);
    if(atomic_load(&locks.retained) || pm.register_held || pm.i2c_held)return 0;
    if(which==A733_PLATFORM_REGISTER_LOCK)pm.register_held=1;else pm.i2c_held=1;return 1;
}
void a733_platform_lock_leave(struct a733_platform_locks *p,enum a733_platform_lock which)
{
    assert(p==&locks);
    if(which==A733_PLATFORM_REGISTER_LOCK){assert(pm.register_held);pm.register_held=0;}
    else {assert(pm.i2c_held);pm.i2c_held=0;}
}
int a733_platform_lock_retain(struct a733_platform_locks *p,enum a733_platform_lock which)
{assert(p==&locks);atomic_fetch_or(&p->retained,1u<<which);return 1;}
static uint32_t *camera_word(uintptr_t a)
{
    if(a>=0x05810000 && a<0x05812000)return &common[(a-0x05810000)/4];
    if(a>=0x05820000 && a<0x05821000)return &parser[(a-0x05820000)/4];
    if(a>=0x05800800 && a<0x05800900)return &route[(a-0x05800800)/4];
    if(a>=0x05910000 && a<0x05910140)return &vipp[(a-0x05910000)/4];
    if(a>=0x05830000 && a<0x05831000)return &dma[(a-0x05830000)/4];
    if(a>=A733_IOMMU_BASE && a<A733_IOMMU_BASE+0x20000)return &mmu[(a-A733_IOMMU_BASE)/4];
    return pmic_model_word(&pm,a);
}
static void emit(void)
{
    assert(camera.capture.packets_granted && parser[3]==1 && !frames);
    assert((route[0x40/4]&15)==0 && (route[0xa0/4]&15)==4);
    assert((vipp[0]&3)==3 && (vipp[0x40/4]&1));
    assert((dma[0]&1) && (dma[0x200/4]&1));
    assert(mmu[0x14/4]==1 && mmu[0x10014/4]==1 && !mmu[0x604/4]);
    uint64_t physical;
    for(unsigned plane=0;plane<2;plane++){
        uint64_t address=(uint64_t)dma[(plane?0x228:0x220)/4]<<2;
        assert(a733_iommu_translate(&camera.capture.tables,address,A733_IOMMU_WRITE,&physical)==1);
        assert(physical==camera.capture.config.memory[plane+1].physical);
        assert(a733_iommu_translate(&camera.capture.tables,address+307199,A733_IOMMU_WRITE,&physical)==1);
        /* Mode 14 models a short frame without any DMA error status. */
        for(unsigned i=0;i<(mode==14?400u*640:307200u);i++)device_image[plane*(307200+4096)+i]=(uint8_t)(i*17+plane*31);
    }
    ++frames;if(mode!=3)parser[3]=0;
    parser[0x44/4]=1;common[0x1118/4]=3;vipp[0x30/4]=0x100;vipp[0x40/4]&=~4u;
    dma[0x254/4]=mode==2?7:3;
    if(mode==6)mmu[0xb0/4]=1;
}
uint32_t a733_camera_model_load(uintptr_t a)
{
    assert(!(a&3));++io_count;
    if((mode==12 || mode==13) && !busy_seen && camera.state==A733_CAMERA_NATIVE_CAPTURE &&
       a>=0x05830000 && a<0x05831000){
        atomic_store(&pm.clocks.busy,1);atomic_store(&pm.rails.busy,1);
        busy_seen=1;busy_until=mode==12?pm.now+40:0;
    }
    if(a==0x05810004 && (common[1]&2))common[1]=(common[1]&0x1f7)|8|((common[1]&0x1f0)<<12);
    return *camera_word(a);
}
void a733_camera_model_store(uintptr_t a,uint32_t value)
{
    assert(!(a&3));++io_count;
    if(a==A733_TWI11_BASE+0xc && value!=0x40){
        assert(pm.i2c_held && !pm.register_held);
        unsigned data=WORD(A733_TWI11_BASE+8),status=0;
        if(value==0x68){assert(wire==0 || wire==4);status=wire?0x10:8;++wire;}
        else if(value==0x48){
            switch(wire){
            case 1:assert(data==0x78);status=0x18;break;
            case 2:selected=data<<8;status=0x28;break;
            case 3:selected|=data;status=0x28;break;
            case 4:sensor[selected]=(uint8_t)data;status=0x28;
                if(selected==0x4202 && !data && sensor[0x300e]==0x45)emit();break;
            case 5:assert(data==0x79);status=0x40;break;
            case 6:WORD(A733_TWI11_BASE+8)=sensor[selected];status=0x58;
                if(selected==0x300a && !identity_read)identity_read=pm.now;break;
            default:assert(0);
            }++wire;
        }else{
            assert(value==0x58 && (wire==5 || wire==7));++stops;wire=0;
            WORD(a)=0x40;WORD(A733_TWI11_BASE+0x10)=0xf8;return;
        }
        WORD(a)=0x48;WORD(A733_TWI11_BASE+0x10)=mode==1 && wire==2?0x38:status;
        if(mode==8 && wire==2)atomic_store(&pm.clocks.failed,1);return;
    }
    if(a==0x05830000+0x254 || a==0x05910030 || a==0x05811118 || a==0x05820044)
        *camera_word(a)&=~value;
    else *camera_word(a)=value;
}
int aarch64_device_cache(enum aarch64_cache_op op,uintptr_t a,size_t bytes)
{
    assert(camera.memory.generation && bytes && !(a&63) && !(bytes&63));
    assert(a>=camera.extent.cpu && bytes<=camera.extent.cpu+camera.extent.bytes-a);
    if(op==AARCH64_CACHE_INVALIDATE){
        assert(frames==1 && camera.capture.scanned && camera.capture.fenced);
        assert(a==camera.extent.cpu+32768+8192 && bytes==sizeof(device_image));++cache_syncs;
        if(mode==4)return 0;
        memcpy((void *)a,device_image,bytes);
        if(mode==5)atomic_store(&pm.clocks.failed,1);
        if(mode==9)pm.pins.failed=1; /* Model another PIO writer, before post-cache lock. */
    }else{
        assert(!frames);
        if(op==AARCH64_CACHE_CLEAN_INVALIDATE && a==camera.extent.cpu+32768+8192){
            /* The fill reaches memory before publication; the device starts from it. */
            assert(bytes==sizeof(device_image));
            for(size_t i=0;i<bytes;i++)assert(((uint8_t *)a)[i]==A733_CAMERA_NATIVE_FILL);
            memcpy(device_image,(void *)a,bytes);++fills;
        }
    }
    return 1;
}
static int pin_read(void *v,uintptr_t a,uint32_t *out)
{assert(v==&pm && pm.register_held);*out=WORD(a);return 1;}
static int pin_write(void *v,uintptr_t a,uint32_t x)
{
    assert(v==&pm && pm.register_held);
    for(unsigned b=A733_PIN_B;b<=A733_PIN_K;b++){
        uintptr_t base=A733_PINS_BASE+0x80+b*0x80;
        if(a==base+0x14){WORD(base+0x10)|=x;goto done;}
        if(a==base+0x18){WORD(base+0x10)&=~x;goto done;}
    }
    WORD(a)=x;
done:
    if(camera.state==A733_CAMERA_NATIVE_CAPTURE && !pwdn_released &&
       !(WORD(A733_PINS_BASE+0x80+A733_PIN_E*0x80+0x10)&(1u<<6)))pwdn_released=pm.now;
    return 1;
}
static int generic_read(void *v,uintptr_t a,uint32_t *out)
{assert(v==&pm);*out=a733_camera_model_load(a);return 1;}
static int generic_write(void *v,uintptr_t a,uint32_t x)
{assert(v==&pm && pm.register_held);a733_camera_model_store(a,x);return 1;}
static int media_auth(void *v,const struct a733_media_clock_config *c,const struct a733_clock_token *t,
    const struct a733_gpu_resource_lease *p)
{
    assert(v==&pm && c->handoff==boot.lease && c->owner==boot.owner && p->token==pm.gpu.leases[2].token);
    assert(a733_clock_token_status(&pm.clocks,t)==A733_CLOCK_OK);return 1;
}
static void media_retain(void *v){assert(v==&pm && pm.register_held);atomic_fetch_or(&locks.retained,1);}
static void pin_config(unsigned bank,unsigned pin,struct a733_pin_config c)
{
    struct a733_pin_fields f;assert(a733_pin_fields_banked(bank,pin,&f));
    WORD(f.mux_address)=(WORD(f.mux_address)&~(15u<<f.mux_shift))|((uint32_t)c.function<<f.mux_shift);
    WORD(f.pull_address)=(WORD(f.pull_address)&~(3u<<f.pull_shift))|((uint32_t)c.pull<<f.pull_shift);
    WORD(f.drive_address)=(WORD(f.drive_address)&~(3u<<f.drive_shift))|((uint32_t)c.drive<<f.drive_shift);
}
static int provider_outside(void *v,const void *p,size_t bytes)
{return !(reject_camera && p==&camera) && pmic_model_outside(v,p,bytes);}
static void setup(void)
{
    pm.now=100;
    WORD(0x07090160)=0x10000002;WORD(0x070101ac)=1;
    WORD(0x02002000)=0xf8000000u|(99u<<8)|(99u<<16);
    WORD(0x020020a0)=0xfe000000u|(99u<<8)|(1u<<20)|(2u<<16)|(4u<<2);
    WORD(0x02002120)=0xf8000000u|(53u<<8)|(3u<<20);
    WORD(0x02003908)=0x0fff0fff;WORD(0x02003988)=0x0fff0000;
    WORD(0x02003910)=0x00770077;WORD(0x02003990)=0x00770000;
    WORD(0x02002500)=0x03000003;WORD(0x02002580)=0x83000001;WORD(0x02002588)=0x81000001;
    WORD(0x020025c0)=0x010000ff;WORD(0x020025e0)=0x41055803;WORD(0x020025e4)=0x40302;
    for(unsigned i=0;i<10;i++){WORD(0x07060000+i*0x1000)=8;WORD(0x07060008+i*0x1000)=8;}
    WORD(0x0701019c)=0x10001;WORD(0x07025000)=0x22;WORD(0x07025014)=0x11;WORD(0x07025024)=5;
    WORD(0x0708300c)=0x40;WORD(0x07083010)=0xf8;WORD(0x07083014)=0x28;WORD(0x07083020)=0x30;
    WORD(0x07083200)=0xf81000;WORD(0x07083210)=0x80c0;WORD(0x07083218)=0x100010;
    pm.pmic[0x0e]=3;pm.pmic[0x10]=0xff;pm.pmic[0x21]=0xff;
    pm.pmic[0x12]=23;pm.pmic[0x19]=100;pm.pmic[0x2e]=13;
    WORD(A733_TWI11_BASE+0x10)=0xf8;WORD(A733_TWI11_BASE+0x20)=0x30;
    WORD(A733_TWI11_BASE+0x200)=0xf81000;WORD(A733_TWI11_BASE+0x210)=0x80c0;WORD(A733_TWI11_BASE+0x218)=0x100010;
    sensor[0x300a]=0x56;sensor[0x300b]=0x40;common[0]=0x360500;common[1]=0xa5;
    for(unsigned i=1;i<=3;i++)common[i*0x100/4]=0x40000;
    locks.initialized=1;locks.now=a733_counter_clock_now;locks.context=&counter;
    locks.mutex[0]=(void *)1;locks.mutex[1]=(void *)2;
    assert(a733_clock_leases_init(&pm.clocks,a733_counter_clock_now,&counter));
    struct a733_gpu_resource_ops gpu={pmic_model_now,generic_read,pmic_model_gpu_write,pmic_model_gpu_rail};
    assert(a733_gpu_resources_init(&pm.gpu,&gpu,&pm));
    struct a733_rail_binding binding={&pm.clocks,&pm.gpu,900,a733_counter_clock_now,&counter};
    assert(a733_rail_leases_init(&pm.rails,&binding));
    struct a733_pins_operations pins={pmic_model_pin_enter,pmic_model_pin_leave,pmic_model_now,pin_read,pin_write};
    struct a733_pin_handoff handoff={0};
    for(unsigned b=1;b<A733_PIN_BANKS;b++)handoff.authorized[b]=a733_pin_bank_mask(b);
    for(unsigned i=0;i<2;i++)handoff.latch[i]=(struct a733_pin_latch_handoff){100+i,900,a733_pin_bank_mask(A733_PIN_L+i),0};
    assert(a733_pins_init_banks(&pm.pins,&pins,&pm,&handoff));
    struct a733_pmic_config pc={777,888,1000000,{{2,1,1},{2,1,1}}};
    struct a733_pmic_ops po={pmic_model_now,pmic_model_current,pmic_model_enter,pmic_model_leave,
        pmic_model_retain,pmic_model_read,pmic_model_write,provider_outside};
    assert(a733_pmic_init(&pm.provider,&pc,&po,&pm,&pm.clocks,&pm.rails,&pm.pins));
    assert(a733_pmic_identity(&pm.provider,&pm.lifetime)==A733_PMIC_READY);
    while(a733_pmic_live(&pm.provider,&pm.lifetime)!=A733_PMIC_READY)assert(pmic_model_step(&pm)!=A733_PMIC_RETAINED);
    boot=(struct a733_camera_boot_record){.version=1,.bytes=sizeof(boot),.owner=50,.epoch=2,.generation=3,
        .lease=99,.deadline_us=1000000,.build={1,2,3,4},.evidence={5,6,7,8},.proof=A733_CAMERA_REQUIRED_PROOF,
        .origin=A733_CAMERA_COLD_RESET,.excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=A733_IOMMU_REAL_MASTERS,
        .raw_trim=0xb00,.parameter_guard_bytes=4096,.plane_guard_bytes=4096,.apb1=0,.apb1_hz=24000000,
        .settle=20,.deskew=2,.reset_bank=A733_PIN_G,.reset_pin=10,.test_pattern=mode==10,
        .rails={A733_RAIL_DCDC1,A733_RAIL_DCDC8,A733_RAIL_BLDO5},.programmed_uv={3300000,1800000,1800000}};
    for(unsigned i=0;i<15;i++)boot.inherited[i]=(struct a733_pin_config){15,0,0};
    boot.inherited[11].function=boot.inherited[14].function=1;
    for(unsigned i=0;i<10;i++)pin_config(A733_PIN_K,i,boot.inherited[i]);
    for(unsigned i=10;i<14;i++)pin_config(A733_PIN_E,i-5,boot.inherited[i]);
    pin_config(A733_PIN_G,10,boot.inherited[14]);WORD(A733_PINS_BASE+0x80+A733_PIN_E*0x80+0x10)=1u<<6;
    if(mode==11){boot.pwdn_only=1;boot.reset_bank=boot.reset_pin=0;boot.inherited[14]=(struct a733_pin_config){0};}
    a733_rail_mask_t rails=0;for(unsigned i=0;i<3;i++)rails|=A733_RAIL_BIT(boot.rails[i]);
    assert(a733_rail_acquire(&pm.rails,boot.owner,rails,0,boot.deadline_us,&supply)==A733_RAIL_PENDING);
    while(a733_rail_poll(&pm.rails,&supply)!=A733_RAIL_READY){}
    uint64_t token;struct a733_gpu_power_resources ready;
    assert(a733_gpu_resources_acquire(&pm.gpu,2,A733_GPU_RESOURCE_PCK,boot.deadline_us,&token,&ready)==A733_GPU_RESOURCE_OK);
    struct a733_gpu_resource_lease lease=pm.gpu.leases[2];
    struct a733_media_clock_config mc={.owner=boot.owner,.handoff=boot.lease,.deadline_us=boot.deadline_us,
        .groups=A733_MEDIA_VI|A733_MEDIA_IOMMU,.camera_mclk_mask=2,
        .fabric={0x03000003,0x83000001,0x81000001},.rate_hz={150000000,300000000,300000000}};
    struct a733_media_clock_ops mo={pmic_model_now,media_auth,pmic_model_pin_enter,pmic_model_pin_leave,
        media_retain,generic_read,generic_write};
    assert(a733_media_clock_init(&media,&mc,&mo,&pm,&pm.clocks,&pm.gpu,2,&lease));
    void *ram=mmap((void *)0x48000000,0x200000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(ram==(void *)0x48000000);assert(device_memory_pool_init(&pool,(uintptr_t)ram,(uintptr_t)ram,0x200000));
    struct a733_camera_native_config config={&boot,&media,&pm.clocks,&pm.pins,&pm.rails,&supply,&pm.provider,&locks,camera_task};
    struct device_memory_pool old_pool=pool;
    assert(a733_camera_native_init(ram,&config)==0 && !memcmp(&pool,&old_pool,sizeof(pool)));
    /* Out-of-range selectors and a PWDN-only record naming reset wiring refuse
     * before any allocation; so does a reset profile without its reset pin. */
    struct a733_camera_boot_record good=boot;
    boot.test_pattern=2;assert(a733_camera_native_init(&camera,&config)==0 && !camera.self);boot=good;
    boot.pwdn_only=2;assert(a733_camera_native_init(&camera,&config)==0 && !camera.self);boot=good;
    if(boot.pwdn_only){
        boot.reset_bank=A733_PIN_G;boot.reset_pin=10;assert(a733_camera_native_init(&camera,&config)==0);boot=good;
        boot.inherited[14].function=1;assert(a733_camera_native_init(&camera,&config)==0);boot=good;
    }else{
        boot.reset_bank=boot.reset_pin=0;assert(a733_camera_native_init(&camera,&config)==0);boot=good;
    }
    assert(!memcmp(&pool,&old_pool,sizeof(pool)) && !camera.self);
    for(unsigned i=0;i<sizeof(camera);i++)assert(!((uint8_t *)ram)[i]);
    reject_camera=1;assert(a733_camera_native_init(&camera,&config)==0 && !camera.self);
    reject_camera=0;assert(a733_camera_native_init(&camera,&config)==1);preparation=0;
    pm.worker=1;assert(a733_camera_native_poll(&camera)==-1 && camera.state==A733_CAMERA_NATIVE_CLOCK && !camera.entered);pm.worker=0;
}
/* The second concrete consumer uses S_TWI1 and an eight-bit register address.
 * This small bus model exercises only that changed width and cancellation. */
struct short_bus { uint32_t words[0x308/4]; uint64_t now; unsigned held,current,retained,step,stops,busy; };
static int short_now(void *v,uint64_t *t){*t=++((struct short_bus *)v)->now;return 1;}
static int short_current(void *v){return ((struct short_bus *)v)->current;}
static int short_auth(void *v){return !((struct short_bus *)v)->retained;}
static int short_enter(void *v,uint64_t d){struct short_bus *b=v;assert(!b->held && b->now<d);if(b->busy){--b->busy;return 0;}b->held=1;return 1;}
static void short_leave(void *v){struct short_bus *b=v;assert(b->held);b->held=0;}
static void short_retain(void *v){++((struct short_bus *)v)->retained;}
static int short_read(void *v,uintptr_t a,uint32_t *x){struct short_bus *b=v;assert(b->held && a>=0x07084000 && a<0x07084308);*x=b->words[(a-0x07084000)/4];return 1;}
static int short_write(void *v,uintptr_t a,uint32_t x)
{
    struct short_bus *b=v;assert(b->held && a>=0x07084000 && a<0x07084010);
    unsigned o=(unsigned)(a-0x07084000);
    if(o==8){b->words[2]=x;return 1;}
    assert(o==12);
    static const unsigned data[]={0,0x44,0x40,0,0x45,0};
    static const unsigned command[]={0x68,0x48,0x48,0x68,0x48,0x48};
    static const unsigned status[]={8,0x18,0x28,0x10,0x40,0x58};
    if(x==0x58){assert(b->step==6);b->words[3]=0x40;b->words[4]=0xf8;++b->stops;return 1;}
    assert(b->step<6 && x==command[b->step]);
    if(data[b->step])assert(b->words[2]==data[b->step]);
    b->words[3]=0x48;b->words[4]=status[b->step++];
    if(b->step==6)b->words[2]=0xa5;return 1;
}
static void short_case(void)
{
    struct short_bus b={.current=1,.busy=1};b.words[3]=0x40;b.words[4]=0xf8;b.words[5]=0x28;b.words[8]=0x30;
    struct a733_twi s={0};struct a733_twi_ops o={short_now,short_auth,short_enter,short_leave,short_retain,short_read,short_write,short_current};
    assert(a733_twi_init_controller(&s,&o,&b,0x28,A733_TWI_R1,1));
    assert(a733_twi_request(&s,0x22,0x100,1,0,1000)==-1 && s.state==A733_TWI_READY);
    assert(a733_twi_request(&s,0x22,0x40,1,0,1000)==0);
    assert(a733_twi_poll(&s)==0 && s.state==A733_TWI_BEGIN && !b.held && !b.retained);
    int result=0;for(unsigned i=0;i<30 && !result;i++)result=a733_twi_poll(&s);
    uint8_t x=0;assert(result==1 && b.stops==1 && !b.held);
    assert(a733_twi_take(&s,&x)==1 && x==0xa5);
    b.step=0;assert(a733_twi_request(&s,0x22,0x40,1,0,2000)==0);
    assert(a733_twi_poll(&s)==0 && b.held);assert(a733_twi_poll(&s)==0);
    b.current=0;enum a733_twi_state before=s.state;
    assert(a733_twi_retain(&s)==-1 && s.state==before && !b.retained);
    assert(a733_twi_poll(&s)==-1 && s.state==before);
    b.current=1;assert(a733_twi_retain(&s)==-1 && s.state==A733_TWI_FAULT && b.retained==1 && b.held);
    assert(a733_twi_retain(&s)==-1 && a733_twi_poll(&s)==-1 && b.retained==1 && b.stops==1);
    puts("PASS S_TWI1 byte address, copied result, wrong-task refusal and retained cancellation");
}
int main(int argc,char **argv)
{
    assert(argc==2);mode=(unsigned)strtoul(argv[1],NULL,10);assert(mode<=14);if(mode==7){short_case();return 0;}setup();
    int r=0;
    for(unsigned polls=0;r==0 && polls<20000;polls++){
        assert(!pm.worker);
        if(camera.state==A733_CAMERA_NATIVE_TWI && !camera.twi_native.self){
            struct a733_twi_native_config c={&pm.clocks,camera.twi_clock,&pm.pins,camera.pin,
                &pm.rails,&camera.rail,&locks,camera_task,boot.apb1,boot.apb1_hz};
            assert(a733_twi_native_init(&camera.twi_native,&c));
            pm.worker=1;assert(a733_twi_native_prepare(&camera.twi_native,boot.deadline_us)==-1 && !camera.twi_native.deadline);pm.worker=0;
        }
        unsigned prior=camera.state;r=a733_camera_native_poll(&camera);
        if(r<0)fprintf(stderr,"failed prior%u snapshot%u rail%u pinsfailed%u twifault%u held%u\n",prior,camera.snapshot,camera.index,pm.pins.failed,camera.twi_native.fault,camera.twi_native.held);
        if((!pm.i2c_held || pm.provider.held) && r>=0)assert(pmic_model_step(&pm)!=A733_PMIC_RETAINED);
        pm.now+=10;
        /* Advance the model clock over the source-defined sensor delay. No
         * controller operation is skipped and no physical timing is inferred. */
        if(r==0 && camera.capture.sensor.wait_until>pm.now)pm.now=camera.capture.sensor.wait_until;
    }
    fprintf(stderr,"camera result%d state%u capture%u analog%u iommu%u sensor%u route%u receiver%u dma%u TWI%u/status%x time%llu frames%u\n",r,camera.state,camera.capture.state,camera.capture.analog.state,camera.capture.iommu.state,camera.capture.sensor.state,camera.capture.route.state,camera.capture.receiver.state,camera.capture.dma.state,camera.twi.state,camera.twi.last_status,(unsigned long long)pm.now,frames);
    unsigned nominal=!mode || mode==10 || mode==11 || mode==12 || mode==14;
    assert(sensor[0x503d]==(mode==10?0x84:0) && busy_seen==(mode==12 || mode==13));
    if(nominal){
        /* PWDN release to identity keeps 1 ms + 20 ms with or without RESETB. */
        assert(fills==1 && pwdn_released && identity_read && identity_read-pwdn_released>=21000);
        unsigned claimed=0;for(unsigned b=0;b<A733_PIN_BANKS;b++)claimed+=(unsigned)__builtin_popcount(camera.pin.masks[b]);
        assert(claimed==(mode==11?14u:15u) && !camera.pin.masks[A733_PIN_G]==(mode==11));
    }
    if(!nominal){
        assert(r==-1 && camera.state==A733_CAMERA_NATIVE_FAULT);
        unsigned before=io_count;assert(a733_camera_native_poll(&camera)==-1 && io_count==before);
        assert(camera.capture.state!=A733_CAPTURE_READABLE);
        if(mode==1 || mode==8)assert(pm.i2c_held && !stops && !frames);
        if(mode==2)assert(frames==1 && !cache_syncs && camera.capture.dma.state==A733_CAM_DMA_FAULT);
        if(mode==3)assert(frames==1 && !cache_syncs && !camera.capture.fenced && parser[3]==1);
        if(mode==4 || mode==5 || mode==9)assert(frames==1 && cache_syncs==1);
        if(mode==6)assert(frames==1 && !cache_syncs && camera.capture.iommu.state==A733_IOMMU_FAULT);
        if(mode==13)assert(!frames && pm.now>=boot.deadline_us && atomic_load(&pm.clocks.busy));
    }else{
        assert(r==1 && frames==1 && cache_syncs==1 && camera.capture.state==A733_CAPTURE_READABLE);
        struct device_memory_extent e;enum device_memory_state state;
        assert(device_memory_describe(&pool,camera.memory,&e,&state) && state==DEVICE_MEMORY_DEVICE);
        assert(!device_memory_release(&pool,camera.memory));
        assert(a733_camera_capture_open_retained(&camera.capture,boot.deadline_us)==-1);
        assert(a733_camera_dma_close(&camera.capture.dma,boot.deadline_us)==-1);
        struct device_memory_lease output;struct device_memory_extent payload;
        assert(device_memory_acquire(&pool,boot.owner,614400,&output,&payload));
        assert(a733_camera_native_copy(&camera,0,0,output,0,307200)==1);
        assert(a733_camera_native_copy(&camera,1,0,output,307200,307200)==1);
        /* A short frame leaves the fill, never earlier memory, in unwritten rows. */
        unsigned written=mode==14?400u*640:307200u;
        for(unsigned p=0;p<2;p++)for(unsigned i=0;i<307200;i++)
            assert(((uint8_t *)payload.cpu)[p*307200+i]==(i<written?(uint8_t)(i*17+p*31):A733_CAMERA_NATIVE_FILL));
        if(mode==12)assert(!busy_until && !atomic_load(&pm.clocks.busy) && !atomic_load(&pm.rails.busy));
        assert(a733_camera_native_copy(&camera,0,307199,output,0,2)==-1);
        assert(a733_camera_native_copy(&camera,0,0,camera.memory,0,1)==-1);
        assert(camera.capture.route.state==A733_ROUTE_ARMED && camera.capture.receiver.state==A733_CSI_ARMED);
        assert(camera.capture.iommu.state==A733_IOMMU_ACTIVE && camera.capture.sensor.state==OV5640_READY);
    }
    printf("PASS native camera scenario%u, frames%u, cache acquisitions%u\n",mode,frames,cache_syncs);
    return 0;
}
