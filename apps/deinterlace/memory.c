#include "deinterlace.h"
#include "a733_memory_uncached.h"
#include "a733_memory_reservations.h"
#include "a733_storage_digest.h"
#include <string.h>
static unsigned motion(void){return deinterlace.session.mode!=A733_DEINTERLACE_WEAVE;}
static size_t md_bytes(void){return deinterlace.session.mode==A733_DEINTERLACE_TNR?A733_DEINTERLACE_TNR_MD_BYTES:A733_DEINTERLACE_MD_BYTES;}
static size_t md_rounded(void){return (md_bytes()+4095)&~(size_t)4095;}
static unsigned allocations(void){return motion()?6:3;}
static uint8_t *image(unsigned i)
{return i==1?deinterlace.input:i==2?deinterlace.output:i==3?deinterlace.current:i==4?deinterlace.flag_input:deinterlace.flag_output;}
/* WEAVE keeps every frame in its one input allocation. The temporal modes
 * alternate even frames in the source and odd frames in the current one. */
static unsigned slot(unsigned frame){return motion()&&(frame&1)?3:1;}
static size_t frames(void){return a733_deinterlace_input_bytes(&deinterlace.session)/deinterlace.bytes;}
int deinterlace_upload_allocate(void)
{
    if(deinterlace.wb_pool||!a733_deinterlace_geometry_valid(&deinterlace.session))return 0;
    deinterlace.bytes=A733_DEINTERLACE_FRAME_BYTES;
    deinterlace.rounded=(deinterlace.bytes+4095)&~(size_t)4095;
    size_t outputs=a733_deinterlace_jobs(&deinterlace.session)*deinterlace.bytes;
    deinterlace.wb_pool=a733_memory_reservations_init();if(!deinterlace.wb_pool)return 0;
    for(unsigned i=0;i<2;++i){struct device_memory_extent e;
        size_t n=((i?outputs:a733_deinterlace_input_bytes(&deinterlace.session))+4095)&~(size_t)4095;
        if(!device_memory_acquire_aligned(deinterlace.wb_pool,deinterlace.session.owner,n,4096,
           &deinterlace.wb_leases[i],&e)||e.bytes!=n||e.cpu%4096)return 0;
        memset((void *)e.cpu,0,e.bytes);
        if(i)deinterlace.payload=(void *)e.cpu;
        else if(!deinterlace_input_bind(&deinterlace.supplied,(void *)e.cpu,e.bytes))return 0;
    }
    storage_digest_bytes(&deinterlace.session,sizeof(deinterlace.session),deinterlace.session_digest);
    return 1;
}
int deinterlace_guards(void)
{
    for(unsigned i=1;i<allocations();++i){struct device_memory_extent e;enum device_memory_state state;
        size_t size=i<4?deinterlace.rounded:md_rounded();
        size_t used=i<4?deinterlace.bytes:md_bytes();
        uint8_t *expected=image(i);
        if(!device_memory_describe(deinterlace.pool,deinterlace.leases[i],&e,&state)||
           state!=DEVICE_MEMORY_DEVICE||e.bytes!=size+8192||e.cpu+4096!=(uintptr_t)expected)return 0;
        const volatile uint8_t *p=(const void *)e.cpu;
        for(unsigned n=0;n<4096;++n)if(p[n]!=0xa5||p[4096+size+n]!=0xa5)return 0;
        for(size_t n=used;n<size;++n)if(expected[n]!=0xa5)return 0;
    }
    return 1;
}
__attribute__((noinline)) int deinterlace_allocate(void)
{
    if(deinterlace.pool||!deinterlace_input_ready(&deinterlace.supplied))return 0;
    deinterlace.pool=a733_memory_uncached_init();if(!deinterlace.pool)return 0;
    for(unsigned i=0;i<allocations();++i){struct device_memory_extent e;
        size_t size=!i?32768:(i<4?deinterlace.rounded:md_rounded())+8192;
        if(!device_memory_acquire_aligned(deinterlace.pool,deinterlace.session.owner,size,i?4096:16384,
           &deinterlace.leases[i],&e)||e.bytes!=size||e.physical!=e.cpu||e.cpu%(i?4096:16384))return 0;
        if(!i)deinterlace.table=(void *)e.cpu;
        else{memset((void *)e.cpu,0xa5,size);uint8_t *p=(void *)(e.cpu+4096);
            if(i==1)deinterlace.input=p;else if(i==2)deinterlace.output=p;else if(i==3)deinterlace.current=p;
            else if(i==4)deinterlace.flag_input=p;else deinterlace.flag_output=p;}
    }
    memcpy(deinterlace.input,deinterlace.supplied.bytes,deinterlace.bytes);memset(deinterlace.output,0,deinterlace.bytes);
    if(motion()){
        memcpy(deinterlace.current,deinterlace.supplied.bytes+deinterlace.bytes,deinterlace.bytes);
        /* Pinned di_client_alloc_mbuf zeroes both initial flag buffers. */
        memset(deinterlace.flag_input,0,md_bytes());
        memset(deinterlace.flag_output,0,md_bytes());
    }
    if(!a733_deinterlace_commands(&deinterlace.plan,&deinterlace.session))return 0;
    struct a733_iommu_span spans[5]={
        {A733_DEINTERLACE_SOURCE_IOVA,(uintptr_t)deinterlace.input,deinterlace.rounded,A733_IOMMU_READ},
        {A733_DEINTERLACE_OUTPUT_IOVA,(uintptr_t)deinterlace.output,deinterlace.rounded,A733_IOMMU_READ|A733_IOMMU_WRITE}};
    if(motion()){
        spans[2]=(struct a733_iommu_span){A733_DEINTERLACE_CURRENT_IOVA,(uintptr_t)deinterlace.current,deinterlace.rounded,A733_IOMMU_READ};
        /* Repeated jobs alternate which MD flag buffer DI writes. */
        spans[3]=(struct a733_iommu_span){A733_DEINTERLACE_FLAG_IN_IOVA,(uintptr_t)deinterlace.flag_input,md_rounded(),
            A733_IOMMU_READ|(deinterlace.session.repeats?A733_IOMMU_WRITE:0)};
        spans[4]=(struct a733_iommu_span){deinterlace.session.mode==A733_DEINTERLACE_TNR?A733_DEINTERLACE_TNR_FLAG_OUT_IOVA:A733_DEINTERLACE_FLAG_OUT_IOVA,(uintptr_t)deinterlace.flag_output,md_rounded(),A733_IOMMU_READ|A733_IOMMU_WRITE};
    }
    if(a733_iommu_build(&deinterlace.tables,deinterlace.table,(uintptr_t)deinterlace.table,spans,motion()?5:2)!=1)return 0;
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned i=0;i<allocations();++i){struct device_memory_allocation *a=device_memory_lookup(deinterlace.pool,deinterlace.leases[i]);
        if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;a->state=DEVICE_MEMORY_DEVICE;}
    return deinterlace_guards();
}
/* Both inputs of the current job still equal their authenticated frames;
 * neither DMA source may be CPU-written while DI owns it. */
int deinterlace_inputs_intact(void)
{
    for(unsigned f=deinterlace.job;f<deinterlace.job+(motion()?2u:1u);++f)
        if(memcmp(image(slot(f)),deinterlace.supplied.bytes+(size_t)f*deinterlace.bytes,deinterlace.bytes))return 0;
    return 1;
}
/* Retire the finished job's oldest input and its copied output, and prepare
 * them for the next job. The caller holds the matched finish with busy clear,
 * a clean fault scan and the private copy; the pinned driver returns a job's
 * buffers at that point. Both stay mapped for master9 only and owned by this
 * session. Any refusal leaves them retained with no further job. */
int deinterlace_refill(void)
{
    unsigned frame=deinterlace.job+(motion()?2u:1u),leases[2]={slot(frame),2};
    struct device_memory_allocation *a[2];
    if(frame>=frames()||!deinterlace_guards()||!deinterlace_inputs_intact())return 0;
    for(unsigned i=0;i<2;++i){a[i]=device_memory_lookup(deinterlace.pool,deinterlace.leases[leases[i]]);
        if(!a[i]||a[i]->state!=DEVICE_MEMORY_DEVICE)return 0;}
    for(unsigned i=0;i<2;++i)a[i]->state=DEVICE_MEMORY_CPU;
    memcpy(image(slot(frame)),deinterlace.supplied.bytes+(size_t)frame*deinterlace.bytes,deinterlace.bytes);
    memset(deinterlace.output,0,deinterlace.bytes);
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned i=0;i<2;++i)a[i]->state=DEVICE_MEMORY_DEVICE;
    return deinterlace_guards();
}
