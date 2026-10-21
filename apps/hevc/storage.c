#include "storage.h"
#include "a733_storage_digest.h"
#include <string.h>
static const size_t capacity[HEVC_BUFFERS]={A733_HEVC_INPUT_SLOT,A733_HEVC_INPUT_SLOT,
 A733_HEVC_OUTPUT_SLOT,A733_HEVC_OUTPUT_SLOT,A733_HEVC_MV_SLOT,A733_HEVC_MV_SLOT,
 A733_HEVC_NEIGHBOR_SLOT,A733_HEVC_ENTRY_SLOT};
/* One 4 MiB table window; unmapped IOVA gaps separate every span. */
static const uint64_t iova[HEVC_BUFFERS]={0x10001000,0x10043000,0x10085000,0x100f8000,0x1016b000,0x10171000,0x10177000,0x10240000};
int hevc_storage_init(struct hevc_storage *s,struct device_memory_pool *wb,struct device_memory_pool *nc,uint64_t owner)
{
 if(!s||s->initialized||!wb||!nc||wb==nc||!owner)return 0;s->initialized=1;s->wb=wb;s->nc=nc;
 for(unsigned i=0;i<3;++i){struct device_memory_extent e;size_t n=i?A733_HEVC_OUTPUT_SLOT:524288;
  if(!device_memory_acquire_aligned(wb,owner,n,4096,&s->wb_leases[i],&e)||e.bytes!=n||e.cpu%4096||
   !device_memory_retain(wb,s->wb_leases[i]))return 0;
  if(!i)s->upload=(void *)e.cpu;else s->copies[i-1]=(void *)e.cpu;
 }
 uint64_t physical[HEVC_BUFFERS],table=0;
 for(unsigned i=0;i<9;++i){struct device_memory_extent e;size_t n=i?capacity[i-1]+8192:32768;
  if(!device_memory_acquire_aligned(nc,owner,n,i?4096:16384,&s->nc_leases[i],&e)||e.bytes!=n||e.cpu%(i?4096:16384))return 0;
  if(!i){s->table=(void *)e.cpu;table=e.physical;}
  else{memset((void *)e.cpu,0xa5,n);s->buffers[i-1]=(void *)(e.cpu+4096);physical[i-1]=e.physical+4096;}
 }
 struct a733_iommu_span spans[HEVC_BUFFERS];
 for(unsigned i=0;i<HEVC_BUFFERS;++i){s->job.buffers[i]=(struct a733_hevc_span){iova[i],capacity[i]};
  spans[i]=(struct a733_iommu_span){iova[i],physical[i],capacity[i],i<2?A733_IOMMU_READ:A733_IOMMU_READ|A733_IOMMU_WRITE};}
 return a733_iommu_build(&s->tables,s->table,table,spans,HEVC_BUFFERS)==1;
}
int hevc_storage_guards(const struct hevc_storage *s)
{
 if(!s||!s->initialized||!s->published)return 0;
 for(unsigned i=0;i<HEVC_BUFFERS;++i){struct device_memory_extent e;enum device_memory_state state;
  if(!device_memory_describe(s->nc,s->nc_leases[i+1],&e,&state)||state!=DEVICE_MEMORY_DEVICE||
   e.bytes!=capacity[i]+8192||e.cpu+4096!=(uintptr_t)s->buffers[i])return 0;
  const volatile uint8_t *p=(const void *)e.cpu;
  for(unsigned k=0;k<4096;++k)if(p[k]!=0xa5||p[4096+capacity[i]+k]!=0xa5)return 0;
  if(i==HEVC_OUTPUT0||i==HEVC_OUTPUT1)for(size_t k=A733_HEVC_PICTURE_BYTES;k<capacity[i];++k)if(p[4096+k]!=0xa5)return 0;
 }
 return 1;
}
int hevc_storage_publish(struct hevc_storage *s,size_t n,const struct a733_hevc_plan *plan,const uint8_t digest[32])
{
 if(!s||!plan||!digest||!s->initialized||s->published||!n||n>524288)return 0;
 uint8_t hash[32];storage_digest_bytes(s->upload,n,hash);if(memcmp(hash,digest,32))return 0;
 for(unsigned i=0;i<2;++i){const struct a733_hevc_picture *p=&plan->pictures[i];
  if(p->offset>n||!p->bytes||p->bytes>A733_HEVC_INPUT_SLOT||p->bytes>n-p->offset)return 0;
 }
 for(unsigned i=0;i<2;++i){const struct a733_hevc_picture *p=&plan->pictures[i];
  memset(s->buffers[i],0,capacity[i]);memcpy(s->buffers[i],s->upload+p->offset,p->bytes);
  memset(s->buffers[HEVC_OUTPUT0+i],0,A733_HEVC_PICTURE_BYTES);
 }
 for(unsigned i=HEVC_MV0;i<HEVC_BUFFERS;++i)memset(s->buffers[i],0,capacity[i]);
 s->job.input=s->upload;s->job.input_bytes=n;s->job.plan=*plan;
 /* Uncached writes complete before the device may observe the buffers. */
#if defined(__aarch64__)
 __asm__ volatile("dsb sy":::"memory");
#endif
 for(unsigned i=0;i<9;++i){struct device_memory_allocation *a=device_memory_lookup(s->nc,s->nc_leases[i]);
  if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;a->state=DEVICE_MEMORY_DEVICE;}
 s->published=1;return hevc_storage_guards(s);
}
