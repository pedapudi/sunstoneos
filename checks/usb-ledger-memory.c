#include "a733_usb_memory.h"
#include "a733_memory_uncached.h"
#include "aarch64_device_memory.h"
#include "owner-memory.h"
#include "usb-memory.h"

const unsigned ss_usb_devices=SUNSTONE_USB_HOST_DEVICES;
unsigned fixture_mode,fixture_scheduler,fixture_clean;
uint64_t fixture_base=UINT64_C(0x50000000),fixture_trace[10];
int xTaskGetSchedulerState(void) { return (int)fixture_scheduler; }
void *os_owner_memory_below(uint64_t bytes,uint64_t align,unsigned attrs,uint64_t limit)
{
    fixture_trace[0]=bytes;fixture_trace[1]=align;fixture_trace[2]=attrs;fixture_trace[3]=limit;
    ++fixture_trace[4];
    return fixture_mode==1?0:(void *)(uintptr_t)fixture_base;
}
int os_usb_memory_alias(uint64_t base,uint64_t bytes)
{
    ++fixture_trace[5];fixture_trace[6]=base;fixture_trace[7]=bytes;
    fixture_trace[8]=a733_usb_memory_parent_base();
    return fixture_mode!=2;
}
int aarch64_device_cache(enum aarch64_cache_op op,uintptr_t base,size_t bytes)
{ (void)op;(void)base;(void)bytes;++fixture_clean;return 1; }
void *memset(void *to,int value,size_t bytes)
{ unsigned char *p=to;while(bytes--)*p++=(unsigned char)value;return to; }
void *memcpy(void *to,const void *from,size_t bytes)
{ unsigned char *p=to;const unsigned char *q=from;while(bytes--)*p++=*q++;return to; }
static struct device_memory_lease parent,jpeg;
static struct device_memory_extent extent;
uint64_t fixture_take_parent(void)
{
    struct device_memory_pool *p=a733_memory_uncached_init();
    return p&&device_memory_acquire(p,1,A733_USB_MEMORY_PARENT_BYTES,&parent,&extent)?extent.cpu:0;
}
uint64_t fixture_take_jpeg(void)
{
    struct device_memory_pool *p=a733_memory_uncached_init();
    if(!p||!device_memory_acquire(p,2,A733_USB_MEMORY_RETAINED_BYTES,&jpeg,&extent))return 0;
    if(!device_memory_retain(p,jpeg))return 0;
    return extent.cpu;
}
uint64_t fixture_to_app(uint64_t base,uint64_t bytes)
{ return (uintptr_t)a733_usb_memory_to_app((void *)(uintptr_t)base,(size_t)bytes); }
uint64_t fixture_from_app(uint64_t base,uint64_t bytes)
{ return (uintptr_t)a733_usb_memory_from_app((void *)(uintptr_t)base,(size_t)bytes); }
int fixture_pool_valid(unsigned variant)
{
    struct device_memory_pool *p=a733_memory_uncached_init();
    if(!p)return a733_usb_memory_pool_valid(p);
    struct device_memory_pool copy=*p;
    if(variant==1)return a733_usb_memory_pool_valid(&copy);
    if(variant==2)p->backing.cpu+=4096;
    if(variant==3)p->backing.physical+=4096;
    if(variant==4)p->backing.bytes-=4096;
    if(variant==5)p->memory_type=DEVICE_MEMORY_WRITE_BACK;
    if(variant==6)p->memory_class=DEVICE_MEMORY_CLASS_DMA32;
    if(variant==7)p->initialized=0;
    int valid=a733_usb_memory_pool_valid(p);
    *p=copy;
    return valid;
}
