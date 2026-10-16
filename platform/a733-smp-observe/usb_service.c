#include "FreeRTOS.h"
#include "task.h"
#include "boot.h"
#include "result.h"
#include "a733_usb.h"
#include "tusb.h"
#include <string.h>
static StaticTask_t task_state;
static StackType_t task_stack[2048] __attribute__((aligned(16)));
static struct a733_observe_snapshot snapshot;
static unsigned ready,initialized,received,sent,response_bytes,complete;
static uint32_t sequence;
static uint8_t challenge[32],request[64],response[528] __attribute__((aligned(64)));
extern void *volatile pxCurrentTCBs[];
extern unsigned char a733_observe_svc_return[];
static const tusb_desc_device_t device={
    .bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,.bcdUSB=0x200,
    .bMaxPacketSize0=64,.idVendor=0x1f3a,.idProduct=0xefe8,.bcdDevice=0x0101,
    .iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1};
static const uint8_t hs[]={TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN,0,100),
    TUD_VENDOR_DESCRIPTOR(0,4,0x02,0x81,512)};
static const uint8_t fs[]={TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN,0,100),
    TUD_VENDOR_DESCRIPTOR(0,4,0x02,0x81,64)};
static const tusb_desc_device_qualifier_t qualifier={.bLength=sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType=TUSB_DESC_DEVICE_QUALIFIER,.bcdUSB=0x200,.bMaxPacketSize0=64,.bNumConfigurations=1};
uint8_t const *tud_descriptor_device_cb(void){return (const uint8_t *)&device;}
uint8_t const *tud_descriptor_device_qualifier_cb(void){return (const uint8_t *)&qualifier;}
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{return index?NULL:(tud_speed_get()==TUSB_SPEED_HIGH?hs:fs);}
uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{static uint8_t other[sizeof(hs)];if(index)return NULL;memcpy(other,tud_speed_get()==TUSB_SPEED_HIGH?fs:hs,sizeof(hs));other[1]=TUSB_DESC_OTHER_SPEED_CONFIG;return other;}
uint16_t const *tud_descriptor_string_cb(uint8_t index,uint16_t language)
{
    (void)language;static uint16_t out[64];
    static const char *const text[]={"","sunstoneos","sunstoneos cached SMP observer","sunstoneos-a733-smp-observe","Result"};
    if(!index){out[0]=0x0304;out[1]=0x0409;return out;}
    if(index>=sizeof(text)/sizeof(*text))return NULL;
    unsigned n=(unsigned)strlen(text[index]);if(n>63)return NULL;out[0]=(uint16_t)(0x0300|2*n+2);
    for(unsigned i=0;i<n;++i)out[i+1]=(uint8_t)text[index][i];return out;
}
void tud_umount_cb(void)
{
    /* An initial bus reset is normal; abandoning an admitted exchange is not. */
    if(sequence || received || response_bytes)a733_observe_failure(5);
}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{(void)interface;(void)bytes;if(response_bytes && sent==response_bytes &&
    tud_vendor_write_available()==CFG_TUD_VENDOR_TX_BUFSIZE)complete=1;}
static void dispatch(void)
{
    uint32_t next;uint8_t next_challenge[32];
    if(!a733_observe_request(request,&a733_observe_identity,&next,next_challenge) ||
        sequence==UINT32_MAX || next!=sequence+1)a733_observe_failure(6);
    for(unsigned i=0;i<32;++i){if(sequence && challenge[i]!=next_challenge[i])a733_observe_failure(7);challenge[i]=next_challenge[i];}
    sequence=next;
    uint32_t header[4]={A733_OBSERVE_MESSAGE_MAGIC,sequence,A733_OBSERVE_COMMAND|UINT32_C(0x80000000),512};
    memcpy(response,header,16);
    const struct a733_observe_snapshot *s=__atomic_load_n(&ready,__ATOMIC_ACQUIRE)?&snapshot:NULL;
    if(!a733_observe_frame(response+16,&a733_observe_identity,s,challenge,a733_smp_build_id))a733_observe_failure(8);
    received=sent=complete=0;response_bytes=sizeof(response);
}
void a733_observe_privileged(void)
{
    uint64_t el,core,pc,psr,sp0,daif;
    __asm__ volatile("mrs %0,CurrentEL\n mrs %1,tpidr_el1\n mrs %2,elr_el1\n mrs %3,spsr_el1\n mrs %4,sp_el0\n mrs %5,daif"
        :"=r"(el),"=r"(core),"=r"(pc),"=r"(psr),"=r"(sp0),"=r"(daif));
    if(el!=4 || core || pc!=(uintptr_t)a733_observe_svc_return || (psr&15) || !(daif&128) ||
        pxCurrentTCBs[0]!=(void *)&task_state || (sp0&15) || sp0<(uintptr_t)task_stack ||
        sp0>(uintptr_t)(task_stack+2048))a733_observe_failure(9);
    a733_observe_deadline();
    if(!initialized){tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
        if(!tusb_init(0,&init))a733_observe_failure(10);initialized=1;}
    a733_usb_poll();tud_task_ext(0,false);a733_observe_deadline();
    if(response_bytes){
        if(sent<response_bytes){sent+=tud_vendor_write(response+sent,response_bytes-sent);tud_vendor_write_flush();}
        if(complete){response_bytes=0;complete=0;}
    }else if(tud_mounted() && tud_vendor_available()){
        if(tud_vendor_available()>sizeof(request)-received)a733_observe_failure(11);
        received+=tud_vendor_read(request+received,sizeof(request)-received);
        if(received==sizeof(request))dispatch();
    }
    a733_observe_deadline();
}
static void usb_task(void *unused)
{(void)unused;for(;;){a733_observe_svc();vTaskDelay(1);}}
void a733_observe_service_start(void)
{
    snapshot.start=a733_smp_counter();
    configASSERT(xTaskCreateStaticAffinitySet(usb_task,"observe",2048,NULL,4,task_stack,&task_state,1));
}
void a733_observe_freeze(void)
{
    configASSERT(portGET_CORE_ID()==0 && !__atomic_load_n(&ready,__ATOMIC_ACQUIRE));
    for(unsigned c=0;c<8;++c){
        snapshot.core[c][0]=__atomic_load_n(&a733_smp_core_results[c].task_rounds,__ATOMIC_ACQUIRE);
        snapshot.core[c][1]=__atomic_load_n(&a733_smp_core_results[c].background_progress,__ATOMIC_RELAXED);
        snapshot.core[c][2]=__atomic_load_n(&a733_smp_core_results[c].sgis,__ATOMIC_RELAXED);
        snapshot.core[c][3]=__atomic_load_n(&a733_smp_core_results[c].failure,__ATOMIC_ACQUIRE);
        snapshot.core[c][4]=__atomic_load_n(&a733_smp_core_results[c].ticks,__ATOMIC_RELAXED);
    }
    snapshot.end=a733_smp_counter();__atomic_store_n(&ready,1,__ATOMIC_RELEASE);
}
