#include "deinterlace.h"
#include "a733_memory_uncached.h"
#include "a733_memory_reservations.h"
#include "a733_storage_digest.h"
#include <string.h>
int deinterlace_upload_allocate(void)
{
    if(deinterlace.wb_pool||!a733_deinterlace_geometry_valid(&deinterlace.session))return 0;
    deinterlace.bytes=A733_DEINTERLACE_INPUT_CAPACITY;
    deinterlace.rounded=(deinterlace.bytes+4095)&~(size_t)4095;
    deinterlace.wb_pool=a733_memory_reservations_init();if(!deinterlace.wb_pool)return 0;
    for(unsigned i=0;i<2;++i){struct device_memory_extent e;
        if(!device_memory_acquire_aligned(deinterlace.wb_pool,deinterlace.session.owner,deinterlace.rounded,4096,
           &deinterlace.wb_leases[i],&e)||e.bytes!=deinterlace.rounded||e.cpu%4096)return 0;
        memset((void *)e.cpu,0,e.bytes);
        if(i)deinterlace.payload=(void *)e.cpu;
        else if(!deinterlace_input_bind(&deinterlace.supplied,(void *)e.cpu,e.bytes))return 0;
    }
    storage_digest_bytes(&deinterlace.session,sizeof(deinterlace.session),deinterlace.session_digest);
    return 1;
}
int deinterlace_guards(void)
{
    for(unsigned i=1;i<3;++i){struct device_memory_extent e;enum device_memory_state state;
        size_t size=deinterlace.rounded;
        uint8_t *expected=i==1?deinterlace.input:deinterlace.output;
        if(!device_memory_describe(deinterlace.pool,deinterlace.leases[i],&e,&state)||
           state!=DEVICE_MEMORY_DEVICE||e.bytes!=size+8192||e.cpu+4096!=(uintptr_t)expected)return 0;
        const volatile uint8_t *p=(const void *)e.cpu;
        for(unsigned n=0;n<4096;++n)if(p[n]!=0xa5||p[4096+size+n]!=0xa5)return 0;
        if(i<3)for(size_t n=deinterlace.bytes;n<deinterlace.rounded;++n)if(expected[n]!=0xa5)return 0;
    }
    return 1;
}
__attribute__((noinline)) int deinterlace_allocate(void)
{
    if(deinterlace.pool||!deinterlace_input_ready(&deinterlace.supplied))return 0;
    deinterlace.pool=a733_memory_uncached_init();if(!deinterlace.pool)return 0;
    for(unsigned i=0;i<3;++i){struct device_memory_extent e;
        size_t size=!i?32768:deinterlace.rounded+8192;
        if(!device_memory_acquire_aligned(deinterlace.pool,deinterlace.session.owner,size,i?4096:16384,
           &deinterlace.leases[i],&e)||e.bytes!=size||e.physical!=e.cpu||e.cpu%(i?4096:16384))return 0;
        if(!i)deinterlace.table=(void *)e.cpu;
        else{memset((void *)e.cpu,0xa5,size);uint8_t *p=(void *)(e.cpu+4096);
            if(i==1)deinterlace.input=p;else deinterlace.output=p;}
    }
    memcpy(deinterlace.input,deinterlace.supplied.bytes,deinterlace.bytes);memset(deinterlace.output,0,deinterlace.bytes);
    if(!a733_deinterlace_commands(&deinterlace.plan,&deinterlace.session))return 0;
    struct a733_iommu_span spans[]={
        {A733_DEINTERLACE_SOURCE_IOVA,(uintptr_t)deinterlace.input,deinterlace.rounded,A733_IOMMU_READ},
        {A733_DEINTERLACE_OUTPUT_IOVA,(uintptr_t)deinterlace.output,deinterlace.rounded,A733_IOMMU_READ|A733_IOMMU_WRITE}};
    if(a733_iommu_build(&deinterlace.tables,deinterlace.table,(uintptr_t)deinterlace.table,spans,2)!=1)return 0;
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned i=0;i<3;++i){struct device_memory_allocation *a=device_memory_lookup(deinterlace.pool,deinterlace.leases[i]);
        if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;a->state=DEVICE_MEMORY_DEVICE;}
    return deinterlace_guards();
}
