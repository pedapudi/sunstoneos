#define DISPLAY_NATIVE_REUSE
#include "display-clock-handoff.c"
#include "a733_display_native.h"
static struct a733_display_native native;
static struct device_memory_pool pool;
static struct a733_iommu iommu;
static unsigned allowed=1, map_ok=1, owner_ok=1, walks, accesses;
static void *pool_storage;
int a733_display_native_owner(void *task){return task==&m&&owner_ok;}
int a733_display_native_mapping(uintptr_t cpu,uint64_t physical,unsigned attr)
{
    (void)physical;assert(attr==0||attr==0x44);
    walks+=attr==0x44&&cpu==native.frame_extent.cpu;accesses+=!attr;return map_ok;
}
int a733_display_native_enter(struct a733_platform_locks *l,uint64_t d)
{assert(l==(void *)&n&&!n.held&&m.now<d);n.held=1;return 1;}
void a733_display_native_leave(struct a733_platform_locks *l)
{assert(l==(void *)&n&&n.held);n.held=0;}
void a733_display_native_retain(struct a733_platform_locks *l)
{assert(l==(void *)&n&&n.held);n.retained=1;}
int a733_display_native_read(uintptr_t a,uint32_t *out)
{return read32(&m,&native.binding.resources,a,out);}
int a733_display_native_write(uintptr_t a,uint32_t value)
{return write32(&m,&native.binding.resources,a,value);}
int a733_display_native_barrier(void){return 1;}
int a733_display_native_rcq_submit(void){assert(!"no replay in this fixture");return -1;}
static int native_auth(void *v,const struct a733_display_native_config *c,uint64_t d)
{assert(v==&m&&c==&native.config&&m.now<d);return allowed;}
static struct a733_display_native_config fixture(void)
{
    restore_sources();memset(&native,0,sizeof(native));memset(&pool,0,sizeof(pool));memset(&iommu,0,sizeof(iommu));
    free(pool_storage);pool_storage=aligned_alloc(16384,4*1024*1024);assert(pool_storage);
    assert(device_memory_pool_init(&pool,(uintptr_t)pool_storage,0x60000000,4*1024*1024));
    struct device_memory_lease frame_lease,table_lease;struct device_memory_extent f,t;
    assert(device_memory_acquire(&pool,4,request_image.frame.mapped_bytes,&frame_lease,&f));
    assert(device_memory_acquire_aligned(&pool,4,A733_IOMMU_TABLE_BYTES,16384,&table_lease,&t));
    struct a733_iommu_span span={request_image.frame.device_address,f.physical,f.bytes,A733_IOMMU_READ};
    assert(a733_iommu_build(&iommu.tables,(void *)t.cpu,t.physical,&span,1)==1);
    request_image.frame.cpu_address=f.cpu;
    device_memory_lookup(&pool,frame_lease)->state=DEVICE_MEMORY_DEVICE;
    device_memory_lookup(&pool,table_lease)->state=DEVICE_MEMORY_DEVICE;
    iommu.state=A733_IOMMU_ACTIVE;iommu.translated_masters=iommu.accessible_masters=0x100;
    bound_scanout=&native.binding.scanout;allowed=map_ok=owner_ok=1;
    return (struct a733_display_native_config){.media=&n.media,.pll=&n.pll,.iommu=&iommu,.pool=&pool,
        .frame=frame_lease,.tables=table_lease,.request=request_image,.locks=(void *)&n,.task=&m,.context=&m,
        .recovery_generation=3,.now_us=now,.authorize=native_auth,.panel=panel};
}
static enum display_scanout_state run_native(void)
{
    enum display_scanout_state s=DISPLAY_SCANOUT_PENDING;
    for(unsigned i=0;i<5000&&s==DISPLAY_SCANOUT_PENDING;++i)s=a733_display_native_poll(&native);
    return s;
}
int main(void)
{
    prepare_sources();struct a733_display_native_config c=fixture();
    assert(a733_display_native_init(&native,&c));
    assert(a733_display_native_prepare(&native,m.now+100000));
    assert(run_native()==DISPLAY_SCANOUT_READY);
    assert(native.binding.transferred&&native.binding.clock.id==101);
    assert(native.binding.scanout.ownership.frame.mapping_generation==c.tables.generation);
    assert(a733_display_native_request(&native,DISPLAY_SCANOUT_START,m.now+100000));
    assert(run_native()==DISPLAY_SCANOUT_ACTIVE);
    assert(a733_display_native_request(&native,DISPLAY_SCANOUT_STOP,m.now+100000));
    assert(run_native()==DISPLAY_SCANOUT_FAULT_RETAINED);
    assert(native.binding.scanout.failed_phase==DISPLAY_DRAIN_DE);
    assert(native.binding.scanout.lcd.state==A733_TCON_ACTIVE);
    assert(device_memory_lookup(&pool,c.frame)->state==DEVICE_MEMORY_RETAINED);
    assert(device_memory_lookup(&pool,c.tables)->state==DEVICE_MEMORY_RETAINED);
    /* The full table/page walk runs once per scanout phase or request, not
     * once per register access. */
    printf("full mapping walks %u for %u register accesses\n",walks,accesses);
    assert(walks<=2*(DISPLAY_REPLAY_DE+1)+3 && accesses>10*walks);
    puts("native provider: actual media/PLL publication, allocator and IOMMU mapping, prepare/start; unsupported drain retains LCD and allocations");
    c=fixture();assert(a733_display_native_init(&native,&c));map_ok=0;
    assert(a733_display_native_prepare(&native,m.now+100000));assert(run_native()==DISPLAY_SCANOUT_FAULT_RETAINED);
    assert(!native.binding.transferred);assert(!native.binding.scanout.phy.initialized);
    c=fixture();assert(a733_display_native_init(&native,&c));
    assert(a733_display_native_prepare(&native,m.now+100000));assert(run_native()==DISPLAY_SCANOUT_READY);
    ++iommu.tables.spans[0].physical;
    assert(a733_display_native_request(&native,DISPLAY_SCANOUT_START,m.now+100000));
    assert(run_native()==DISPLAY_SCANOUT_FAULT_RETAINED&&native.binding.scanout.de.state!=A733_DE_ARMED);
    c=fixture();assert(a733_display_native_init(&native,&c));
    assert(a733_display_native_prepare(&native,m.now+100000));assert(run_native()==DISPLAY_SCANOUT_READY);
    map_ok=0;assert(a733_display_native_request(&native,DISPLAY_SCANOUT_START,m.now+100000));
    assert(run_native()==DISPLAY_SCANOUT_FAULT_RETAINED&&native.binding.scanout.de.state!=A733_DE_ARMED);
    /* In-place table corruption is found by the full walk of the next request. */
    c=fixture();assert(a733_display_native_init(&native,&c));
    assert(a733_display_native_prepare(&native,m.now+100000));assert(run_native()==DISPLAY_SCANOUT_READY);
    ((uint32_t *)native.table_extent.cpu)[A733_IOMMU_TABLE_BYTES/8]^=1;
    assert(a733_display_native_request(&native,DISPLAY_SCANOUT_START,m.now+100000));
    assert(run_native()==DISPLAY_SCANOUT_FAULT_RETAINED&&native.binding.scanout.de.state!=A733_DE_ARMED);
    puts("mapping denial before publication or a later request, changed live IOMMU mapping and corrupted table words before DE start refuse access");
    c=fixture();assert(a733_display_native_init(&native,&c));
    owner_ok=0;unsigned before=m.calls;
    assert(!a733_display_native_prepare(&native,m.now+100000));
    assert(a733_display_native_poll(&native)==DISPLAY_SCANOUT_FAULT_RETAINED&&before==m.calls);
    owner_ok=1;assert(a733_display_native_prepare(&native,m.now+100000));
    assert(run_native()==DISPLAY_SCANOUT_READY);m.fail_write=1;
    assert(a733_display_native_request(&native,DISPLAY_SCANOUT_START,m.now+100000));
    assert(run_native()==DISPLAY_SCANOUT_FAULT_RETAINED&&n.retained&&n.held);
    before=m.calls;assert(a733_display_native_poll(&native)==DISPLAY_SCANOUT_FAULT_RETAINED&&before==m.calls);
    puts("wrong-task admission has no effects; uncertain native write retains the common mutex and terminal polling makes no MMIO");
    free(pool_storage);free(register_checkpoint);return 0;
}
