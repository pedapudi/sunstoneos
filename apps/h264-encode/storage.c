#include "storage.h"
#include "a733_storage_digest.h"
#include <string.h>
static const size_t bytes[A733_H264_ENCODE_BUFFERS]={462848,462848,4194304,
 491520,491520,12288,12288,8192,8192,4096};
static const uint64_t iovas[A733_H264_ENCODE_BUFFERS]={0x10001000,0x10074000,0x10400000,
 0x10801000,0x1087b000,0x108f5000,0x108fa000,0x108ff000,0x10902000,0x10905000};
int h264_encode_storage_init(struct h264_encode_storage *s,struct device_memory_pool *wb,
    struct device_memory_pool *nc,uint64_t owner)
{
    if(!s||s->initialized||!wb||!nc||wb==nc||!owner)return 0;
    s->initialized=1;s->wb=wb;s->nc=nc;
    for(unsigned i=0;i<3;++i){
      struct device_memory_extent e;size_t size=i?1048576:925696;
      if(!device_memory_acquire_aligned(wb,owner,size,4096,&s->wb_leases[i],&e)||
         e.bytes!=size||e.cpu%4096)return 0;
      if(!i)s->upload=(void *)e.cpu;else s->copies[i-1]=(void *)e.cpu;
    }
    for(unsigned i=0;i<9;++i){
      struct device_memory_extent e;size_t size=i==8?36864:i?bytes[i-1]+8192:32768;
      if(!device_memory_acquire_aligned(nc,owner,size,i?4096:16384,&s->nc_leases[i],&e)||
         e.bytes!=size||e.cpu%(i?4096:16384))return 0;
      if(!i)s->table=(void *)e.cpu;
      else{memset((void *)e.cpu,0xa5,size);s->buffers[i-1]=(void *)(e.cpu+4096);
        if(i==8){s->buffers[8]=s->buffers[7]+12288;s->buffers[9]=s->buffers[8]+12288;}}
    }
    struct a733_iommu_span spans[8];
    for(unsigned i=0;i<A733_H264_ENCODE_BUFFERS;++i){
      s->job.buffers[i]=(struct a733_h264_encode_span){iovas[i],bytes[i]};
      if(i<7)spans[i]=(struct a733_iommu_span){iovas[i],(uintptr_t)s->buffers[i],bytes[i],
        i<2?A733_IOMMU_READ:A733_IOMMU_READ|A733_IOMMU_WRITE};
    }
    /* Scratch shares one private extent, with checked internal guard pages.
     * Those internal pages are mapped RW; outer guards remain unmapped. */
    spans[7]=(struct a733_iommu_span){iovas[7],(uintptr_t)s->buffers[7],28672,A733_IOMMU_READ|A733_IOMMU_WRITE};
    s->job.input[0]=s->buffers[0];s->job.input[1]=s->buffers[1];
    return a733_iommu_build(&s->tables,s->table,(uintptr_t)s->table,spans,8)==1;
}
int h264_encode_storage_guards(const struct h264_encode_storage *s)
{
    if(!s||!s->initialized||!s->published)return 0;
    for(unsigned i=0;i<A733_H264_ENCODE_BUFFERS;++i){
      struct device_memory_extent e;enum device_memory_state state;
      unsigned lease=i<7?i+1:8;
      if(!device_memory_describe(s->nc,s->nc_leases[lease],&e,&state)||state!=DEVICE_MEMORY_DEVICE||
         e.bytes!=(i<7?bytes[i]+8192:36864))return 0;
      uintptr_t expected=e.cpu+4096+(i<7?0:(i-7)*12288);
      if(expected!=(uintptr_t)s->buffers[i])return 0;
      const volatile uint8_t *p=s->buffers[i]-4096;
      for(unsigned k=0;k<4096;++k)if(p[k]!=0xa5||p[4096+bytes[i]+k]!=0xa5)return 0;
      if(i<2)for(size_t k=460800;k<bytes[i];++k)if(p[4096+k]!=0xa5)return 0;
    }
    return 1;
}
int h264_encode_storage_publish(struct h264_encode_storage *s,const uint8_t digest[32])
{
    if(!s||!digest||!s->initialized||s->published||!s->upload)return 0;
    uint8_t hash[32];storage_digest_bytes(s->upload,921600,hash);if(memcmp(hash,digest,32))return 0;
    for(unsigned i=0;i<2;++i)memcpy(s->buffers[i],s->upload+i*460800,460800);
    /* All reconstructed/reference/scratch data starts at zero, as the source
     * allocation path. They retain opaque hardware formats and are never copied
     * to callers as pixels. Ring contents are initialized before device access. */
    for(unsigned i=2;i<A733_H264_ENCODE_BUFFERS;++i)memset(s->buffers[i],0,bytes[i]);
    for(unsigned i=0;i<9;++i){struct device_memory_allocation *a=device_memory_lookup(s->nc,s->nc_leases[i]);
      if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;a->state=DEVICE_MEMORY_DEVICE;}
    s->published=1;return h264_encode_storage_guards(s);
}
