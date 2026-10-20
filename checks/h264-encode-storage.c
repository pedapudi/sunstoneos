#define _GNU_SOURCE
#include "storage.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static struct device_memory_pool wb,nc;
static struct h264_encode_storage storage;
static size_t used(const struct device_memory_pool *p)
{size_t end=0;for(unsigned i=0;i<32;++i){const struct device_memory_allocation *a=&p->allocations[i];size_t n=((size_t)a->first+a->pages)*4096;if(n>end)end=n;}return end;}
int main(int argc,char **argv)
{
 assert(argc==2);void *w=mmap((void *)0x48000000,4194304,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 void *n=mmap((void *)0x78000000,8388608,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(w==(void *)0x48000000&&n==(void *)0x78000000);
 assert(device_memory_pool_init(&wb,(uintptr_t)w,(uintptr_t)w,4194304)&&device_memory_pool_init(&nc,(uintptr_t)n,(uintptr_t)n,8388608));
 assert(h264_encode_storage_init(&storage,&wb,&nc,12)&&!h264_encode_storage_init(&storage,&wb,&nc,12));
 assert(used(&wb)==H264_ENCODE_WB_BYTES&&used(&nc)==H264_ENCODE_NC_BYTES&&storage.tables.count==8);
 FILE *f=fopen(argv[1],"rb");assert(f&&fread(storage.upload,1,921600,f)==921600&&!fclose(f));
 uint8_t hash[32];storage_digest_bytes(storage.upload,921600,hash);
 assert(h264_encode_storage_publish(&storage,hash)&&h264_encode_storage_guards(&storage));
 assert(!h264_encode_storage_publish(&storage,hash));
 for(unsigned i=0;i<2;++i)assert(!memcmp(storage.job.input[i],storage.upload+i*460800,460800));
 for(unsigned i=0;i<10;++i){uint64_t physical;struct a733_h264_encode_span s=storage.job.buffers[i];assert(a733_iommu_translate(&storage.tables,s.iova,A733_IOMMU_READ,&physical)==1&&physical==(uintptr_t)storage.buffers[i]);
   if(i<2)assert(a733_iommu_translate(&storage.tables,s.iova,A733_IOMMU_WRITE,&physical)==-1);
   assert(s.bytes%4096==0&&!(s.iova%4096));
 }
 for(unsigned i=7;i<10;++i){storage.buffers[i][-1]^=1;assert(!h264_encode_storage_guards(&storage));storage.buffers[i][-1]^=1;}
 assert(h264_encode_storage_guards(&storage));
 printf("{\"passed\":true,\"nc_bytes\":%zu,\"wb_bytes\":%zu,\"cpu_storage_metadata\":%zu,\"iommu_spans\":8,\"scratch_internal_guards_mapped\":true,\"physical_access\":false}\n",used(&nc),used(&wb),sizeof(storage));
 return 0;
}
