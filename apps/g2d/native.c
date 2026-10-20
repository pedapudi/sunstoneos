#include "g2d.h"
#include "a733_clock_reference.h"
#include "a733_storage_digest.h"
#include "aarch64_device_memory.h"
#include "boot.h"
#include <string.h>
struct g2d_application g2d;
#define CCU UINT32_C(0x02002000)
#define CLOCK_SHARED (A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC|A733_POWER_DCDC2)
#define CLOCK_EXCLUSIVE (A733_CLOCK_DE_SYS|A733_CLOCK_IOMMU_SYS|A733_CLOCK_VI|A733_CLOCK_VE_DEC|A733_CLOCK_VE_ENC)
static uint64_t deadline(void){return board.session.deadline_us-10000;}
static int now(void *v,uint64_t *out)
{return v==&g2d&&board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,out)==1;}
static int live(void)
{
    uint64_t t;if(now(&g2d,&t)!=1||t<g2d.last_now||t>=deadline())return 0;
    g2d.last_now=t;return 1;
}
static int device(uintptr_t a)
{struct aarch64_translation t;return aarch64_device_translate(a,1,&t)&&t.physical==a&&!t.attributes&&t.shareability==2;}
static int load(uintptr_t a,uint32_t *v)
{
    if(((a>=A733_IOMMU_BASE&&a<A733_IOMMU_BASE+0x20000)||
        (a>=A733_G2D_BASE&&a<A733_G2D_BASE+0x40000))&&
        !a733_de_sys_power_ready(&g2d.domain))return 0;
    if(!v||!live()||!device(a))return 0;
    __asm__ volatile("dsb sy":::"memory");*v=*(volatile uint32_t *)a;
    __asm__ volatile("dsb sy":::"memory");return live();
}
static int store(uintptr_t a,uint32_t v)
{
    if(((a>=A733_IOMMU_BASE&&a<A733_IOMMU_BASE+0x20000)||
        (a>=A733_G2D_BASE&&a<A733_G2D_BASE+0x40000))&&
        !a733_de_sys_power_ready(&g2d.domain))return 0;
    if(!live()||!device(a))return 0;
    *(volatile uint32_t *)a=v;__asm__ volatile("dsb sy":::"memory");return live();
}
static int bits(uintptr_t a,uint32_t mask,uint32_t value)
{uint32_t v;return load(a,&v)&&(v&mask)==value;}
static int update(uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t v,r;return load(a,&v)&&store(a,(v&~mask)|value)&&load(a,&r)&&r==((v&~mask)|value);
}
static int enter(void)
{return a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,deadline());}
static void leave(void){a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static int domain_now(void *v,uint64_t *out)
{return v==&g2d&&g2d.handle&&xTaskGetCurrentTaskHandle()==g2d.handle&&now(v,out);}
static int domain_authorize(void *v)
{
    if(v!=&g2d||!g2d.handle||xTaskGetCurrentTaskHandle()!=g2d.handle||!live()||
       g2d.domain.deadline!=deadline()||!g2d.domain_clock.id||
       g2d.domain_clock.owner!=g2d.session.owner||
       g2d.domain_clock.shared!=CLOCK_SHARED||g2d.domain_clock.exclusive!=CLOCK_EXCLUSIVE||
       g2d.clock.id!=g2d.domain_clock.id||g2d.clock.owner!=g2d.domain_clock.owner||
       g2d.clock.shared!=g2d.domain_clock.shared||g2d.clock.exclusive!=g2d.domain_clock.exclusive||
       !g2d.domain_rail.id||g2d.domain_rail.owner!=g2d.session.owner||
       g2d.domain_rail.shared!=A733_RAIL_BIT(A733_RAIL_DCDC2)||g2d.domain_rail.exclusive||
       g2d.rail.id!=g2d.domain_rail.id||g2d.rail.owner!=g2d.domain_rail.owner||
       g2d.rail.shared!=g2d.domain_rail.shared||g2d.rail.exclusive!=g2d.domain_rail.exclusive||
       !g2d.domain_pck.token||g2d.domain_pck.token!=g2d.power_token||
       g2d.domain_pck.mask!=(A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK)||
       g2d.domain_pck.mode!=A733_GPU_RESOURCE_STABLE)return -1;
    enum a733_clock_result c=a733_clock_token_status(&providers.shared.clocks,&g2d.domain_clock);
    if(c==A733_CLOCK_BUSY)return 0;if(c!=A733_CLOCK_OK)return -1;
    enum a733_rail_result r=a733_rail_status(&providers.shared.rails,&g2d.domain_rail);
    if(r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_READY)return -1;
    enum a733_gpu_resource_result p=a733_gpu_resources_status(&providers.shared.gpu,
        (unsigned)g2d.session.pck_client,&g2d.domain_pck);
    return p==A733_GPU_RESOURCE_OK?1:p==A733_GPU_RESOURCE_BUSY?0:-1;
}
static int domain_enter(void *v,uint64_t d)
{
    uint64_t bound=g2d.domain.wait_deadline?g2d.domain.wait_deadline:deadline();
    return v==&g2d&&d==bound&&d<=deadline()?
        a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;
}
static void domain_leave(void *v){if(v==&g2d)leave();}
static int domain_address(uintptr_t a)
{return a==A733_DE_SYS_PCK_BASE||a==A733_DE_SYS_PCK_BASE+8||
    a==A733_DE_SYS_PCK_BASE+0x170||a==A733_DE_SYS_PCK_BASE+0x174||
    a==A733_DE_SYS_PCK_BASE+0xc00||a==A733_DE_SYS_PCK_BASE+0xc04||a==A733_DE_SYS_PCK_BASE+0xc10;}
static int domain_read(void *v,uintptr_t a,uint32_t *out)
{return v==&g2d&&domain_address(a)&&g2d.domain.held&&load(a,out);}
static int domain_write(void *v,uintptr_t a,uint32_t value)
{return v==&g2d&&a!=A733_DE_SYS_PCK_BASE+8&&domain_address(a)&&g2d.domain.held&&store(a,value);}
static int iommu_read(void *v,uint64_t token,uint32_t off,uint32_t *out,uint64_t d)
{return v==&g2d&&token==g2d.session.authority&&d==deadline()&&off<0x20000&&!(off&3)&&load(A733_IOMMU_BASE+off,out)?1:-1;}
static int iommu_write(void *v,uint64_t token,uint32_t off,uint32_t value,uint64_t d)
{
    uint32_t ignored;if(iommu_read(v,token,off,&ignored,d)!=1||enter()!=1)return -1;
    if(!store(A733_IOMMU_BASE+off,value))return -1;leave();return 1;
}
static int publish(void *v,uint64_t token,const void *p,size_t n,uint64_t d)
{
    if(v!=&g2d||token!=g2d.session.authority||p!=g2d.table||n!=32768||d!=deadline())return -1;
    __asm__ volatile("dsb sy":::"memory");return live()?1:-1;
}
static int admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
    if(v!=&g2d||t->words!=g2d.table||d!=deadline()||!live()||
       a733_clock_token_status(&providers.shared.clocks,&g2d.clock)!=A733_CLOCK_OK)return -1;
    *a=(struct a733_iommu_admission){.token=g2d.session.authority,
        .excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=0x400,
        .inherited_resolved=1,.transactions_drained=1,.clocks_held=1,.retained_power_removal=1};
    /* The sealed initial-isolation evidence supplies drain/exclusion. Neither
     * register idle readback nor a CPU barrier establishes these facts. */
    return 1;
}
static int parents(void)
{
    uint32_t xo,ref,peri;
    if(!a733_de_sys_power_ready(&g2d.domain))return 0;
    if(!load(0x07090160,&xo)||!load(CCU,&ref)||!a733_clock_ref24_valid(xo,ref)||
       !load(CCU+0xa0,&peri)||(peri&0xff000002)!=0xfe000000||
       !bits(CCU+0xa8,0x80000000,0)||!bits(CCU+0xac,0x8e000000,0))return 0;
    unsigned n=((peri>>8)&255)+1,div=(((peri>>20)&7)+1)*3;
    if(n<53||n>105||24*n!=400*div||!bits(CCU+0x1908,0x8060806,0x8060806)||
       !bits(CCU+0x1988,0x8060000,0x8060000))return 0;
    /* Domain1 is adopted or requested ON by the fixed retained provider.
     * Fabric leaf gates remain inherited under the sealed authority. */
    if(!bits(0x07061000,15,8)||!bits(0x07061008,15,8)||
       !bits(CCU+0x5c0,1u<<5,1u<<5)||!bits(CCU+0x5e0,1u<<11,1u<<11))return 0;
    static const unsigned offsets[]={0x500,0x580,0x588};
    static const unsigned masks[]={0x0300001f,0x8700001f,0x8700001f};
    for(unsigned i=0;i<3;++i)if(!bits(CCU+offsets[i],masks[i],g2d.session.board.fabric[i]))return 0;
    for(unsigned i=0;i<2;++i)if(!bits(CCU+(i?0x5b4:0x58c),0x10007,g2d.session.iommu_gates[i]))return 0;
    return 1;
}
static int clocks(void)
{
    if(!live()||!parents())return -1;
    int r=enter();if(!r)return 0;if(r!=1)return -1;
    /* Every uncertain write retains the shared register mutex. No reset is
     * asserted; initial inherited traffic has already been drained/excluded. */
    static const struct {unsigned offset,mask,value;} changes[]={
      {0xa74,0x10000,0x10000},{0xa44,0x10000,0x10000},
      {0xa40,0x0700001f,A733_G2D_MODULE},{0xa44,1,1},{0xa40,0x80000000,0x80000000}};
    if(!g2d.clock_step&&!bits(CCU+0xa40,0x80000000,0))return -1;
    if(!parents()||!update(CCU+changes[g2d.clock_step].offset,changes[g2d.clock_step].mask,
       changes[g2d.clock_step].value))return -1;
    ++g2d.clock_step;leave();return g2d.clock_step==5?1:0;
}
static int setup(void)
{
    enum a733_rail_result rr;
    if(!g2d.rail.id)rr=a733_rail_acquire(&providers.shared.rails,g2d.session.owner,
       A733_RAIL_BIT(A733_RAIL_DCDC2),0,deadline(),&g2d.rail);
    else rr=a733_rail_poll(&providers.shared.rails,&g2d.rail);
    if(rr==A733_RAIL_BUSY||rr==A733_RAIL_PENDING)return 0;if(rr!=A733_RAIL_READY)return -1;
    enum a733_pmic_status pr;
    if(!g2d.request.id){pr=a733_pmic_submit(&providers.shared.pmic,g2d.session.owner,&g2d.rail,
      A733_RAIL_DCDC2,deadline(),&g2d.request);return pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY?0:-1;}
    pr=a733_pmic_result(&providers.shared.pmic,&g2d.request,&g2d.supply);
    if(pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY)return 0;
    if(pr!=A733_PMIC_COMPLETE||g2d.supply.rail!=A733_RAIL_DCDC2||g2d.supply.page_before||
       g2d.supply.page_after||g2d.supply.chip!=3||g2d.supply.revision||!g2d.supply.enabled||
       !g2d.supply.decoded||g2d.supply.programmed_uv!=800000||
       !(g2d.supply.enable&2)||(g2d.supply.selector&127)!=30)return -1;
    if(!g2d.clock.id){enum a733_clock_result r=a733_clock_acquire(&providers.shared.clocks,g2d.session.owner,
       CLOCK_SHARED,CLOCK_EXCLUSIVE,
       deadline(),&g2d.clock);if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!g2d.power_token){enum a733_gpu_resource_result r=a733_gpu_resources_acquire(&providers.shared.gpu,
       (unsigned)g2d.session.pck_client,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,deadline(),
       &g2d.power_token,&g2d.power_ready);if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return -1;}
    if(!g2d.domain.self){
        g2d.domain_clock=g2d.clock;g2d.domain_rail=g2d.rail;
        g2d.domain_pck=(struct a733_gpu_resource_lease){g2d.power_token,
            A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE};
        const struct a733_de_sys_power_ops ops={domain_authorize,domain_now,domain_enter,
            domain_leave,domain_read,domain_write};
        if(!a733_de_sys_power_init(&g2d.domain,&ops,&g2d,deadline()))return -1;
    }
    int power=a733_de_sys_power_poll(&g2d.domain);if(power!=1)return power;
    int r=clocks();if(r!=1)return r;
    struct a733_iommu_ops ops={now,admit,iommu_read,iommu_write,publish};
    if(!g2d_allocate()||a733_iommu_init_g2d(&g2d.iommu,&ops,&g2d,&g2d.tables)!=1||
       a733_iommu_open(&g2d.iommu,deadline())!=1)return -1;
    return 1;
}
static int launch(void)
{
    uint32_t status;
    if(!g2d_guards()||!bits(A733_G2D_BASE+0x28,1,0)||
       !bits(A733_G2D_BASE+0x100,0x80000000,0))return -1;
    if(!update(A733_G2D_BASE,3,3)||!update(A733_G2D_BASE+4,3,3)||
       !update(A733_G2D_BASE+8,3,3)||!update(A733_G2D_BASE+0x18,1,1)||
       !store(A733_G2D_BASE+0x20,0)||!store(A733_G2D_BASE+0x24,5)||
       !load(A733_G2D_BASE+0x24,&status)||(status&5))return -1;
    g2d.baseline=status&0xff00;
    if(!store(A733_G2D_BASE+0x28,0)||!store(A733_G2D_BASE+0x2c,A733_G2D_RCQ_IOVA)||
       !store(A733_G2D_BASE+0x30,0)||!store(A733_G2D_BASE+0x34,128)||
       !store(A733_G2D_BASE+0x20,0x10)||!store(A733_G2D_BASE+0x28,1))return -1;
    return 1;
}
static int stable(void)
{
    uint32_t status;
    return live()&&g2d_guards()&&load(A733_G2D_BASE+0x24,&status)&&
       (status&0xff01)==(g2d.rcq_status&0xff01)&&
       (status&1);
}
static int step(void)
{
    if(!g2d.stage){int r=setup();if(r==1)g2d.stage=1;return r<0?-1:0;}
    if(g2d.stage==1){int r=a733_iommu_poll(&g2d.iommu);if(r!=1)return r;
       if(launch()!=1)return -1;g2d.stage=2;return 0;}
    if(g2d.stage==2){uint32_t status;if(!load(A733_G2D_BASE+0x24,&status))return -1;
       if(!(status&1))return 0;
       g2d.rcq_status=status;g2d.complete=1;
       if(a733_iommu_scan(&g2d.iommu,deadline())!=1)return -1;g2d.stage=3;return 0;}
    int r=a733_iommu_poll(&g2d.iommu);if(r!=1)return r;
    if(g2d.iommu.fault.scanned_masters!=0x400||!stable())return -1;
    if(g2d.copied<g2d.bytes){size_t n=g2d.bytes-g2d.copied;if(n>32768)n=32768;
       __asm__ volatile("dsb sy":::"memory");memcpy(g2d.payload+g2d.copied,g2d.output+g2d.copied,n);
       __asm__ volatile("dsb sy":::"memory");if(!stable())return -1;g2d.copied+=n;
       if(a733_iommu_scan(&g2d.iommu,deadline())!=1)return -1;return 0;}
    uint8_t hash[32];storage_digest_bytes(g2d.input,g2d.bytes,hash);
    if(memcmp(hash,g2d.session.fixture,32)||!stable())return -1;
    storage_digest_bytes(g2d.payload,g2d.bytes,g2d.digest);
    if(!stable())return -1;g2d.guards_ok=1;return 1;
}
void g2d_task(void *unused)
{
    (void)unused;for(;;){
        if(!atomic_load_explicit(&g2d.result,memory_order_acquire)){
            if(!live())atomic_store_explicit(&g2d.result,2,memory_order_release);
            else if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)&&g2d_input_ready(&g2d.supplied)){
                int r=step();if(r)atomic_store_explicit(&g2d.result,r==1?1:2,memory_order_release);
            }
        }vTaskDelay(1);
    }
}
