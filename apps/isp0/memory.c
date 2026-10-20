#include "isp0.h"
#include "a733_memory_uncached.h"
#include "a733_memory_reservations.h"
#include "a733_storage_digest.h"
#include <string.h>
static size_t rounded(size_t n){return (n+4095)&~(size_t)4095;}
int isp0_upload_allocate(void)
{
 if(isp0.wb_pool||!a733_isp0_geometry_valid(&isp0.session))return 0;
 isp0.bytes=A733_ISP0_OUTPUT_CAPACITY;
 isp0.wb_pool=a733_memory_reservations_init();if(!isp0.wb_pool)return 0;
 for(unsigned i=0;i<2;i++){struct device_memory_extent e;
  if(!device_memory_acquire_aligned(isp0.wb_pool,isp0.session.owner,isp0.bytes,4096,
    &isp0.wb_leases[i],&e)||e.bytes!=isp0.bytes||e.cpu%4096)return 0;
  memset((void *)e.cpu,0,e.bytes);
  if(i)isp0.payload=(void *)e.cpu;
  else if(!isp0_input_bind(&isp0.supplied,(void *)e.cpu,e.bytes))return 0;
 }
 storage_digest_bytes(&isp0.session,sizeof(isp0.session),isp0.session_digest);return 1;
}
int isp0_guards(void)
{
 for(unsigned i=1;i<ISP0_EXTENTS;i++){struct device_memory_extent e;enum device_memory_state state;
  if(!device_memory_describe(isp0.pool,isp0.leases[i],&e,&state)||state!=DEVICE_MEMORY_DEVICE||
     e.physical!=e.cpu||e.bytes!=rounded(isp0.sizes[i])+8192||e.cpu+4096!=(uintptr_t)isp0.memory[i])return 0;
  const volatile uint8_t *p=(const void *)e.cpu;size_t n=rounded(isp0.sizes[i]);
  for(unsigned j=0;j<4096;j++)if(p[j]!=0xa5||p[4096+n+j]!=0xa5)return 0;
  for(size_t j=isp0.sizes[i];j<n;j++)if(isp0.memory[i][j]!=0xa5)return 0;
 }
 for(unsigned i=0;i<64;i++)if(isp0.memory[ISP0_RAW][A733_ISP0_INPUT_BYTES+i])return 0;
 return 1;
}
__attribute__((noinline)) int isp0_allocate(void)
{
 if(isp0.pool||!isp0_input_ready(&isp0.supplied))return 0;
 isp0.pool=a733_memory_uncached_init();if(!isp0.pool)return 0;
 const size_t sizes[]={32768,A733_ISP0_INPUT_BYTES+64,A733_ISP0_PLANE_BYTES,A733_ISP0_PLANE_BYTES,
   A733_ISP0_LOAD_BYTES,A733_ISP0_SAVE_BYTES,A733_ISP0_SAVE_LOAD_BYTES,1024};
 const uint64_t iovas[]={0,A733_ISP0_INPUT_IOVA,A733_ISP0_Y_IOVA,A733_ISP0_UV_IOVA,
   A733_ISP0_LOAD_IOVA,A733_ISP0_SAVE_IOVA,A733_ISP0_SAVE_LOAD_IOVA,A733_ISP0_VIPP_IOVA};
 struct a733_iommu_span spans[7];
 for(unsigned i=0;i<ISP0_EXTENTS;i++){struct device_memory_extent e;size_t n=rounded(sizes[i]);isp0.sizes[i]=sizes[i];
  if(!device_memory_acquire_aligned(isp0.pool,isp0.session.owner,n+(i?8192:0),i?4096:16384,
    &isp0.leases[i],&e)||e.bytes!=n+(i?8192:0)||e.physical!=e.cpu||e.cpu%(i?4096:16384))return 0;
  memset((void *)e.cpu,0xa5,e.bytes);isp0.memory[i]=(void *)(e.cpu+(i?4096:0));
  if(i){memset(isp0.memory[i],0,sizes[i]);unsigned permission=A733_IOMMU_READ;
   if(i==ISP0_Y||i==ISP0_UV||i==ISP0_SAVE||i==ISP0_SAVE_LOAD)permission|=A733_IOMMU_WRITE;
   spans[i-1]=(struct a733_iommu_span){iovas[i],(uintptr_t)isp0.memory[i],n,permission};}
 }
 memcpy(isp0.memory[ISP0_RAW],isp0.supplied.bytes,A733_ISP0_INPUT_BYTES);
 if(!a733_isp0_parameters(isp0.memory[ISP0_LOAD],sizes[ISP0_LOAD],isp0.memory[ISP0_SAVE_LOAD],
   sizes[ISP0_SAVE_LOAD],isp0.memory[ISP0_VIPP],1024)||
   a733_iommu_build(&isp0.tables,(void *)isp0.memory[ISP0_TABLE],(uintptr_t)isp0.memory[ISP0_TABLE],spans,7)!=1)return 0;
 __asm__ volatile("dsb sy":::"memory");
 for(unsigned i=0;i<ISP0_EXTENTS;i++){struct device_memory_allocation *a=device_memory_lookup(isp0.pool,isp0.leases[i]);
  if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;a->state=DEVICE_MEMORY_DEVICE;}
 return isp0_guards();
}
