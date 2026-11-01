#include "a733_usb_memory.h"
#include "a733_memory_uncached.h"
#include "aarch64_device_memory.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned calls,fault;
const uintptr_t a733_local_pool_stride=0x200000;
int aarch64_device_translate(uintptr_t p,int write,struct aarch64_translation *out)
{
 assert(write&&p>=A733_USB_MEMORY_BASE&&p<A733_USB_MEMORY_END&&!(p&0x1fffff));
 ++calls;if(fault&&calls==fault)return 0;
 *out=(struct aarch64_translation){.physical=p,.attributes=0x44,.shareability=3};return 1;
}
unsigned aarch64_device_cache_stride(void){return 64;}
int aarch64_device_cache(enum aarch64_cache_op op,uintptr_t p,size_t bytes)
{(void)op;(void)p;(void)bytes;assert(0);return 0;}
int main(int argc,char **argv)
{
 if(argc>1)fault=(unsigned)strtoul(argv[1],NULL,0);
 assert(!a733_memory_uncached_init());
 int ok=a733_memory_uncached_prepare();assert(ok==!fault);
 if(fault){assert(!a733_memory_uncached_init());assert(!a733_memory_uncached_prepare());puts("USB pool translation refusal retained");return 0;}
 assert(calls==4);assert(a733_memory_uncached_prepare()&&calls==4);
 struct device_memory_pool *pool=a733_memory_uncached_init();assert(pool);
 assert(pool->backing.cpu==A733_USB_MEMORY_BASE&&pool->backing.physical==A733_USB_MEMORY_BASE&&pool->backing.bytes==A733_USB_MEMORY_BYTES);
 struct device_memory_lease first,second;struct device_memory_extent parent,jpeg;
 assert(device_memory_acquire(pool,1,A733_USB_MEMORY_PARENT_BYTES,&first,&parent));
 assert(parent.cpu==A733_USB_MEMORY_BASE&&parent.physical==parent.cpu&&parent.bytes==A733_USB_MEMORY_PARENT_BYTES);
 /* JPEG retains input first, then its 16 KiB-aligned IOMMU table and both
  * guarded output planes. Use the production maximum geometry. */
 assert(device_memory_acquire(pool,2,524288+8192,&second,&jpeg));
 assert(jpeg.cpu==A733_USB_MEMORY_BASE+A733_USB_MEMORY_PARENT_BYTES);
 const size_t retained[]={32768,307200+8192,155648+8192};
 for(unsigned i=0;i<3;++i){
  struct device_memory_lease lease;struct device_memory_extent extent;
  assert(device_memory_acquire_aligned(pool,2,retained[i],i?4096:16384,&lease,&extent));
  assert(extent.cpu>=jpeg.cpu+jpeg.bytes&&extent.cpu+extent.bytes<=A733_USB_MEMORY_END);
  assert(device_memory_retain(pool,lease));
 }
 assert(device_memory_retain(pool,second));
 struct device_memory_lease spare;struct device_memory_extent unused;
 assert(!device_memory_acquire(pool,3,A733_USB_MEMORY_BYTES-A733_USB_MEMORY_PARENT_BYTES,&spare,&unused));
 assert(!a733_usb_memory_to_app((void *)A733_USB_MEMORY_BASE,4096));
 assert(!a733_usb_memory_to_app((void *)(A733_USB_MEMORY_DATA_START-1),1));
 assert(!a733_usb_memory_to_app((void *)A733_USB_MEMORY_DATA_START,0));
 assert(a733_usb_memory_to_app((void *)A733_USB_MEMORY_DATA_START,A733_USB_MEMORY_DATA_BYTES)==(void *)A733_USB_MEMORY_APP_START);
 assert(a733_usb_memory_from_app((void *)A733_USB_MEMORY_APP_START,A733_USB_MEMORY_DATA_BYTES)==(void *)A733_USB_MEMORY_DATA_START);
 assert(a733_usb_memory_to_app((void *)(A733_USB_MEMORY_DATA_END-1),1)==(void *)(A733_USB_MEMORY_APP_END-1));
 assert(!a733_usb_memory_to_app((void *)(A733_USB_MEMORY_DATA_END-1),2));
 assert(!a733_usb_memory_from_app((void *)A733_USB_MEMORY_APP_END,1));
 assert(!a733_usb_memory_from_app((void *)(A733_USB_MEMORY_APP_START-1),1));
 assert(!a733_usb_memory_from_app((void *)(A733_USB_MEMORY_APP_END-1),SIZE_MAX));
 assert(!a733_usb_memory_to_app((void *)jpeg.cpu,jpeg.bytes));
 puts("USB NC pool capacity, first parent placement and payload alias bounds passed");return 0;
}
