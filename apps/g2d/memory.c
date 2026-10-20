#include "g2d.h"
#include "a733_memory_uncached.h"
#include "a733_memory_reservations.h"
#include "a733_storage_digest.h"
#include <string.h>
int g2d_upload_allocate(void)
{
    if(g2d.wb_pool||!a733_g2d_geometry_valid(&g2d.session))return 0;
    g2d.bytes=(size_t)g2d.session.stride*g2d.session.height;
    g2d.rounded=(g2d.bytes+4095)&~(size_t)4095;
    g2d.wb_pool=a733_memory_reservations_init();if(!g2d.wb_pool)return 0;
    for(unsigned i=0;i<2;++i){struct device_memory_extent e;
        if(!device_memory_acquire_aligned(g2d.wb_pool,g2d.session.owner,g2d.rounded,4096,
           &g2d.wb_leases[i],&e)||e.bytes!=g2d.rounded||e.cpu%4096)return 0;
        memset((void *)e.cpu,0,e.bytes);
        if(i)g2d.payload=(void *)e.cpu;
        else if(!g2d_input_bind(&g2d.supplied,(void *)e.cpu,e.bytes))return 0;
    }
    storage_digest_bytes(&g2d.session,sizeof(g2d.session),g2d.session_digest);
    return 1;
}
int g2d_guards(void)
{
    for(unsigned i=1;i<4;++i){struct device_memory_extent e;enum device_memory_state state;
        size_t size=i==3?4096:g2d.rounded;
        uint8_t *expected=i==1?g2d.input:i==2?g2d.output:g2d.commands;
        if(!device_memory_describe(g2d.pool,g2d.leases[i],&e,&state)||
           state!=DEVICE_MEMORY_DEVICE||e.bytes!=size+8192||e.cpu+4096!=(uintptr_t)expected)return 0;
        const volatile uint8_t *p=(const void *)e.cpu;
        for(unsigned n=0;n<4096;++n)if(p[n]!=0xa5||p[4096+size+n]!=0xa5)return 0;
        if(i<3)for(size_t n=g2d.bytes;n<g2d.rounded;++n)if(expected[n]!=0xa5)return 0;
    }
    return 1;
}
__attribute__((noinline)) int g2d_allocate(void)
{
    if(g2d.pool||!g2d_input_ready(&g2d.supplied))return 0;
    g2d.pool=a733_memory_uncached_init();if(!g2d.pool)return 0;
    for(unsigned i=0;i<4;++i){struct device_memory_extent e;
        size_t size=!i?32768:i==3?12288:g2d.rounded+8192;
        if(!device_memory_acquire_aligned(g2d.pool,g2d.session.owner,size,i?4096:16384,
           &g2d.leases[i],&e)||e.bytes!=size||e.cpu%(i?4096:16384))return 0;
        if(!i)g2d.table=(void *)e.cpu;
        else{memset((void *)e.cpu,0xa5,size);uint8_t *p=(void *)(e.cpu+4096);
            if(i==1)g2d.input=p;else if(i==2)g2d.output=p;else g2d.commands=p;}
    }
    memcpy(g2d.input,g2d.supplied.bytes,g2d.bytes);memset(g2d.output,0,g2d.bytes);
    if(!a733_g2d_commands(g2d.commands,4096,&g2d.session))return 0;
    struct a733_iommu_span spans[]={
        {A733_G2D_SOURCE_IOVA,(uintptr_t)g2d.input,g2d.rounded,A733_IOMMU_READ},
        {A733_G2D_OUTPUT_IOVA,(uintptr_t)g2d.output,g2d.rounded,A733_IOMMU_READ|A733_IOMMU_WRITE},
        {A733_G2D_RCQ_IOVA,(uintptr_t)g2d.commands,4096,A733_IOMMU_READ}};
    if(a733_iommu_build(&g2d.tables,g2d.table,(uintptr_t)g2d.table,spans,3)!=1)return 0;
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned i=0;i<4;++i){struct device_memory_allocation *a=device_memory_lookup(g2d.pool,g2d.leases[i]);
        if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;a->state=DEVICE_MEMORY_DEVICE;}
    return g2d_guards();
}
