#include "deinterlace.h"
#include "a733_clock_reference.h"
#include "a733_storage_digest.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "a733_range.h"
#include "boot.h"
#include <string.h>
struct deinterlace_application deinterlace;
#define CCU UINT32_C(0x02002000)
static uint64_t deadline(void){return board.session.deadline_us-10000;}
static int now(void *v,uint64_t *out)
{return v==&deinterlace&&board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,out)==1;}
static int live(void)
{
    uint64_t t;return now(&deinterlace,&t)==1&&a733_deadline_admit(t,&deinterlace.last_now,deadline());
}
static int device(uintptr_t a)
{
    /* Device identity mapping; the write translation must be outer shareable. */
    static const struct a733_mapping_rule rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
    return a733_mapping_check(aarch64_device_translate,a,a,&rule);
}
static int di_address(uintptr_t address,unsigned writing,uint32_t value)
{
 if(address<A733_DEINTERLACE_BASE||address>=A733_DEINTERLACE_BASE+0x40000)return 1;
 unsigned off=(unsigned)(address-A733_DEINTERLACE_BASE);
 if(off==0x10)return !writing||value==1;
 if(off==0x14)return !writing||!(value&1);
 if(off==0x18)return !writing||value==1;
 size_t n;const struct a733_deinterlace_word *w=deinterlace.session.mode==A733_DEINTERLACE_TNR?
  a733_deinterlace_tnr_words(&n):a733_deinterlace_words(&n);
 for(size_t i=0;i<n;i++)if(off==w[i].offset)return 1;
 return 0;
}
static int load(uintptr_t a,uint32_t *v)
{
    if(!v||!di_address(a,0,0)||!live()||!device(a))return 0;
    *v=a733_mmio_read32_dsb_sy_both(a);return live();
}
static int store(uintptr_t a,uint32_t v)
{
    if(!di_address(a,1,v)||!live()||!device(a))return 0;
    a733_mmio_write32_dsb_sy_after(a,v);return live();
}
static int bits(uintptr_t a,uint32_t mask,uint32_t value)
{uint32_t v;return load(a,&v)&&(v&mask)==value;}
static int update(uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t v,r;return load(a,&v)&&store(a,(v&~mask)|(value&mask))&&load(a,&r)&&r==((v&~mask)|(value&mask));
}
static int enter(void)
{return a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,deadline());}
static void leave(void){a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static int iommu_read(void *v,uint64_t token,uint32_t off,uint32_t *out,uint64_t d)
{return v==&deinterlace&&token==deinterlace.session.authority&&d==deadline()&&off<0x20000&&!(off&3)&&load(A733_IOMMU_BASE+off,out)?1:-1;}
static int iommu_write(void *v,uint64_t token,uint32_t off,uint32_t value,uint64_t d)
{
    uint32_t ignored;if(iommu_read(v,token,off,&ignored,d)!=1||enter()!=1)return -1;
    if(!store(A733_IOMMU_BASE+off,value))return -1;leave();return 1;
}
static int publish(void *v,uint64_t token,const void *p,size_t n,uint64_t d)
{
    if(v!=&deinterlace||token!=deinterlace.session.authority||p!=deinterlace.table||n!=32768||d!=deadline())return -1;
    __asm__ volatile("dsb sy":::"memory");return live()?1:-1;
}
static int admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
    if(v!=&deinterlace||t->words!=deinterlace.table||d!=deadline()||!live()||
       a733_clock_token_status(&providers.shared.clocks,&deinterlace.clock)!=A733_CLOCK_OK)return -1;
    *a=(struct a733_iommu_admission){.token=deinterlace.session.authority,
        .excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=0x200,
        .inherited_resolved=1,.transactions_drained=1,.clocks_held=1,.retained_power_removal=1};
    /* The sealed initial-isolation evidence supplies drain/exclusion. Neither
     * register idle readback nor a CPU barrier establishes these facts. */
    return 1;
}
static int parents(void)
{
    uint32_t xo,ref,peri;
    if(!load(0x07090160,&xo)||!load(CCU,&ref)||!a733_clock_ref24_valid(xo,ref)||
       !load(CCU+0xa0,&peri)||(peri&0xff000002)!=0xfe000000||
       !bits(CCU+0xa8,0x80000000,0)||!bits(CCU+0xac,0x8e000000,0))return 0;
    unsigned n=((peri>>8)&255)+1,div=(((peri>>20)&7)+1)*2;
    if(n<53||n>105||24*n!=600*div||!bits(CCU+0x1908,0x0a000a00,0x0a000a00)||
       !bits(CCU+0x1988,0x0a000000,0x0a000000))return 0;
    /* Stable domain1 and fabric leaf gates are inherited under authority. */
    if(!bits(0x07061000,15,8)||!bits(0x07061008,15,8)||
       !bits(CCU+0x5c0,1u<<5,1u<<5)||!bits(CCU+0x5e0,1u<<11,1u<<11))return 0;
    static const unsigned offsets[]={0x500,0x580,0x588};
    static const unsigned masks[]={0x0300001f,0x8700001f,0x8700001f};
    for(unsigned i=0;i<3;++i)if(!bits(CCU+offsets[i],masks[i],deinterlace.session.board.fabric[i]))return 0;
    for(unsigned i=0;i<2;++i)if(!bits(CCU+(i?0x5b4:0x58c),0x10007,deinterlace.session.iommu_gates[i]))return 0;
    return 1;
}
static int clocks(void)
{
    if(!live()||!parents())return -1;
    int r=enter();if(!r)return 0;if(r!=1)return -1;
    /* Every uncertain write retains the shared register mutex. No reset is
     * asserted; initial inherited traffic has already been drained/excluded. */
    static const struct {unsigned offset,mask,value;} changes[]={
      {0xa74,0x10000,0x10000},{0xa24,0x10000,0x10000},
      {0xa20,0x0700001f,A733_DEINTERLACE_MODULE},{0xa24,1,1},{0xa20,0x80000000,0x80000000}};
    if(!deinterlace.clock_step){
      if(!bits(CCU+0xa20,0x80000000,0))return -1;
      /* MOTION admits the reset-held lifetime used by the pinned Linux
       * disable/enable sequence. Never assert/reset an inherited live engine. */
      if(deinterlace.session.mode!=A733_DEINTERLACE_WEAVE&&
         !bits(CCU+0xa24,0x10001,0))return -1;
    }
    if(!parents()||!update(CCU+changes[deinterlace.clock_step].offset,changes[deinterlace.clock_step].mask,
       changes[deinterlace.clock_step].value))return -1;
    ++deinterlace.clock_step;leave();return deinterlace.clock_step==5?1:0;
}
static int setup(void)
{
    enum a733_rail_result rr;
    if(!deinterlace.rail.id)rr=a733_rail_acquire(&providers.shared.rails,deinterlace.session.owner,
       A733_RAIL_BIT(A733_RAIL_DCDC2),0,deadline(),&deinterlace.rail);
    else rr=a733_rail_poll(&providers.shared.rails,&deinterlace.rail);
    if(rr==A733_RAIL_BUSY||rr==A733_RAIL_PENDING)return 0;if(rr!=A733_RAIL_READY)return -1;
    enum a733_pmic_status pr;
    if(!deinterlace.request.id){pr=a733_pmic_submit(&providers.shared.pmic,deinterlace.session.owner,&deinterlace.rail,
      A733_RAIL_DCDC2,deadline(),&deinterlace.request);return pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY?0:-1;}
    pr=a733_pmic_result(&providers.shared.pmic,&deinterlace.request,&deinterlace.supply);
    if(pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY)return 0;
    if(pr!=A733_PMIC_COMPLETE||deinterlace.supply.rail!=A733_RAIL_DCDC2||deinterlace.supply.page_before||
       deinterlace.supply.page_after||deinterlace.supply.chip!=3||deinterlace.supply.revision||!deinterlace.supply.enabled||
       !deinterlace.supply.decoded||deinterlace.supply.programmed_uv!=800000||
       !(deinterlace.supply.enable&2)||(deinterlace.supply.selector&127)!=30)return -1;
    if(!deinterlace.clock.id){enum a733_clock_result r=a733_clock_acquire(&providers.shared.clocks,deinterlace.session.owner,
       A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC|A733_POWER_DCDC2,
       A733_CLOCK_DE_SYS|A733_CLOCK_IOMMU_SYS|A733_CLOCK_VI|A733_CLOCK_VE_DEC|A733_CLOCK_VE_ENC,
       deadline(),&deinterlace.clock);if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!deinterlace.power_token){enum a733_gpu_resource_result r=a733_gpu_resources_acquire(&providers.shared.gpu,
       (unsigned)deinterlace.session.pck_client,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,deadline(),
       &deinterlace.power_token,&deinterlace.power_ready);if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return -1;}
    int r=clocks();if(r!=1)return r;
    struct a733_iommu_ops ops={now,admit,iommu_read,iommu_write,publish};
    if(!deinterlace_allocate()||a733_iommu_init_deinterlace(&deinterlace.iommu,&ops,&deinterlace,&deinterlace.tables)!=1||
       a733_iommu_open(&deinterlace.iommu,deadline())!=1)return -1;
    return 1;
}
static int launch(void)
{
 if(!deinterlace_guards()||!a733_deinterlace_plan_valid(&deinterlace.plan)||!parents())return -1;
 int r=enter();if(r!=1)return r<0?-1:0;
 size_t count;const struct a733_deinterlace_word *w=a733_deinterlace_words(&count);
 if(deinterlace.session.mode==A733_DEINTERLACE_WEAVE)count=A733_DEINTERLACE_WEAVE_WORDS;
 if(deinterlace.session.mode==A733_DEINTERLACE_TNR)w=a733_deinterlace_tnr_words(&count);
 if(!deinterlace.command_step){
  /* Source disabled, inherited work excluded by admission. Clear only stale
   * finish before arming this one operation; never reset or infer bus drain. */
  if(!bits(A733_DEINTERLACE_BASE+0x18,0x100,0)||
   !bits(A733_DEINTERLACE_BASE+0x10,1,0)||
   !update(A733_DEINTERLACE_BASE+0x14,1,0)||
   !store(A733_DEINTERLACE_BASE+0x18,1)||
   !bits(A733_DEINTERLACE_BASE+0x18,0x101,0))return -1;
 }
 if(deinterlace.command_step<count){
  const struct a733_deinterlace_word *row=w+deinterlace.command_step;
  if(!update(A733_DEINTERLACE_BASE+row->offset,row->mask,deinterlace.plan.words[row->offset/4]))return -1;
  ++deinterlace.command_step;leave();return 0;
 }
 if(!bits(A733_DEINTERLACE_BASE+0x14,1,0)||
  !bits(A733_DEINTERLACE_BASE+0x18,0x101,0)||
  !store(A733_DEINTERLACE_BASE+0x10,1))return -1;
 deinterlace.armed=1;leave();return 1;
}
static int stable(void)
{
 uint32_t status;
 return deinterlace.armed&&deinterlace.complete&&live()&&deinterlace_guards()&&
  bits(A733_DEINTERLACE_BASE+0x14,1,0)&&load(A733_DEINTERLACE_BASE+0x18,&status)&&
  (status&0x101)==1;
}
static int step(void)
{
 if(!deinterlace.stage){int r=setup();if(r==1)deinterlace.stage=1;return r<0?-1:0;}
 if(deinterlace.stage==1){int r=a733_iommu_poll(&deinterlace.iommu);if(r!=1)return r;
  deinterlace.stage=2;return 0;}
 if(deinterlace.stage==2){int r=launch();if(r<0)return -1;if(r==1)deinterlace.stage=3;return 0;}
 if(deinterlace.stage==3){uint32_t status;if(!load(A733_DEINTERLACE_BASE+0x18,&status))return -1;
  if(!(status&1))return 0;
  if(!deinterlace.armed||(status&0x100))return -1;
  deinterlace.status=status;deinterlace.complete=1;
  if(a733_iommu_scan(&deinterlace.iommu,deadline())!=1)return -1;deinterlace.stage=4;return 0;}
 int r=a733_iommu_poll(&deinterlace.iommu);if(r!=1)return r;
 if(deinterlace.iommu.fault.scanned_masters!=0x200||!stable())return -1;
 if(deinterlace.copied<deinterlace.bytes){size_t n=deinterlace.bytes-deinterlace.copied;if(n>32768)n=32768;
  __asm__ volatile("dsb sy":::"memory");memcpy(deinterlace.payload+deinterlace.copied,deinterlace.output+deinterlace.copied,n);
  __asm__ volatile("dsb sy":::"memory");if(!stable())return -1;deinterlace.copied+=n;
  if(a733_iommu_scan(&deinterlace.iommu,deadline())!=1)return -1;return 0;}
 /* Recheck both immutable DMA inputs against the retained authenticated
  * upload; neither source span may be CPU-written after publication. */
 if(memcmp(deinterlace.input,deinterlace.supplied.bytes,deinterlace.bytes)||
    (deinterlace.session.mode!=A733_DEINTERLACE_WEAVE&&
     memcmp(deinterlace.current,deinterlace.supplied.bytes+deinterlace.bytes,deinterlace.bytes)))return -1;
 uint8_t hash[32];storage_digest_bytes(deinterlace.supplied.bytes,
   a733_deinterlace_input_bytes(&deinterlace.session),hash);
 if(memcmp(hash,deinterlace.session.fixture,32)||!stable())return -1;
 storage_digest_bytes(deinterlace.payload,deinterlace.bytes,deinterlace.digest);
 if(!stable())return -1;deinterlace.guards_ok=1;return 1;
}
void deinterlace_task(void *unused)
{
    (void)unused;for(;;){
        if(!atomic_load_explicit(&deinterlace.result,memory_order_acquire)){
            if(!live())atomic_store_explicit(&deinterlace.result,2,memory_order_release);
            else if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)&&deinterlace_input_ready(&deinterlace.supplied)){
                int r=step();if(r)atomic_store_explicit(&deinterlace.result,r==1?1:2,memory_order_release);
            }
        }vTaskDelay(1);
    }
}
