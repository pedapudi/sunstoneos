#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a733_usb2_native.h"
#include "sunstone_usb_hid.h"
#include "owner-memory.h"
#include "tusb.h"

static struct a733_usb2_native native;
static struct sunstone_usb_hid (*allocated)[CFG_TUH_HID];
static unsigned mode,allocations,initializations,clock_calls,mounted,receives,lost_calls;
static size_t taken;
static unsigned marker;
static const uint8_t descriptor[]={
    5,1,9,6,0xa1,1,5,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,
    0x75,1,0x95,8,0x81,2,0x95,1,0x75,8,0x81,1,0x95,6,0x75,8,
    0x15,0,0x25,0x65,5,7,0x19,0,0x29,0x65,0x81,0,0xc0};

static int current(void *ctx) { assert(ctx==&native);return mode!=4; }
static int now(void *ctx,uint64_t *value)
{ assert(ctx==&native);++clock_calls;*value=1000000;return mode!=5; }
static void emit(void *ctx,const struct sunstone_hid_event *event)
{
    assert(ctx==&marker&&event->type==SUNSTONE_HID_ATTACH&&event->address==CFG_TUH_DEVICE_MAX&&
           event->instance==CFG_TUH_HID-1&&event->generation==1);
    ++mounted;
}
void *os_owner_memory(uint64_t bytes,uint64_t align,unsigned attributes)
{
    assert(!initializations&&!clock_calls&&!allocated&&++allocations==1);
    /* The public owner-dependent API sees no published host yet. */
    assert(tusb_time_millis_api()==0&&!clock_calls);
    assert(bytes==sizeof(*allocated)*CFG_TUH_DEVICE_MAX&&align==64&&attributes==OS_MEMORY_CACHED);
    taken=(size_t)bytes;
    if(mode==1)return NULL;
    void *memory=NULL;assert(!posix_memalign(&memory,64,(size_t)bytes));
    memset(memory,0,(size_t)bytes);allocated=memory;return memory;
}
bool tusb_rhport_init(uint8_t port,const tusb_rhport_init_t *init)
{
    assert(port==0&&init&&init->role==TUSB_ROLE_HOST&&++initializations==1);
    if(native.generic_host){
        assert(allocations==1&&allocated);
        for(size_t i=0;i<taken;++i)assert(!((unsigned char *)allocated)[i]);
        if(mode!=2){
            /* Run a class callback immediately: the last address and instance
             * must already have zeroed parser state inside the returned extent. */
            tuh_hid_mount_cb(CFG_TUH_DEVICE_MAX,CFG_TUH_HID-1,descriptor,sizeof descriptor);
            const struct sunstone_usb_hid *h=&allocated[CFG_TUH_DEVICE_MAX-1][CFG_TUH_HID-1];
            assert(h->mounted&&h->address==CFG_TUH_DEVICE_MAX&&h->instance==CFG_TUH_HID-1&&h->generation==1);
            assert(mounted==1&&receives==1);
        }
    }else assert(!allocations&&!allocated);
    return mode!=2;
}
bool tuh_hid_receive_report(uint8_t address,uint8_t instance)
{ assert(address==CFG_TUH_DEVICE_MAX&&instance==CFG_TUH_HID-1);++receives;return true; }
unsigned a733_xhci_device_speed(const struct a733_xhci *core,unsigned address)
{ (void)core;(void)address;assert(0);return 0; }
void a733_xhci_lost(struct a733_xhci *core) { assert(core==&native.core);++lost_calls; }
void a733_usb2_platform_lost(struct a733_usb2_platform *platform)
{ assert(platform==&native.platform);++lost_calls; }

int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"allocation-failure"))mode=1;
    else if(!strcmp(argv[1],"stack-failure"))mode=2;
    else if(!strcmp(argv[1],"nongeneric"))mode=3;
    else if(!strcmp(argv[1],"wrong-owner"))mode=4;
    else if(!strcmp(argv[1],"clock-failure"))mode=5;
    else assert(!strcmp(argv[1],"normal"));
    native.self=&native;native.core_started=1;native.core.self=&native.core;
    native.core.context=&native;native.core.ops.current=current;native.core.ops.now=now;
    native.core.generation=7;native.generic_host=mode!=3;
    native.platform.config.deadline=2000000;native.platform.config.lifetime_deadline=3000000;
    native.core.devices[CFG_TUH_DEVICE_MAX-1].address=CFG_TUH_DEVICE_MAX;
    native.descriptors[CFG_TUH_DEVICE_MAX-1].generic.ready=1;
    a733_usb2_set_input(&native,emit,&marker);
    int result=a733_usb2_host_bind(&native);
    assert(result==(mode==0||mode==3));
    if(mode==1||mode==4)assert(!initializations&&!clock_calls&&!lost_calls&&!allocated);
    if(mode==1){
        assert(allocations==1);native.generic_host=0;
        assert(!a733_usb2_host_bind(&native)&&allocations==1&&!initializations);
    }else if(mode!=4)assert(!a733_usb2_host_bind(&native));
    if(mode==4)assert(!allocations);
    if(mode==2||mode==5)assert(allocated&&allocations==1&&native.failed&&lost_calls==2);
    if(mode==5)assert(!initializations);
    printf("{\"case\":\"%s\",\"devices\":%u,\"hid_bytes\":%zu,\"allocations\":%u,\"initializations\":%u,\"mounted\":%u}\n",
           argv[1],CFG_TUH_DEVICE_MAX,taken,allocations,initializations,mounted);
    return 0;
}
