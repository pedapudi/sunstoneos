/* USB2 owner and the application's single-sector/read-keyboard mailboxes. */
#include "os.h"
#include "application.h"
#include "../board/application.h"
#ifdef SUNSTONE_BOARD_USB2_IRQ
#include "../board/host_irq.h"
#endif
#include <string.h>
static struct {atomic_uint state;uint64_t sector;uint8_t data[512];} storage;
static struct {atomic_uint ready;struct ss_key_report report;} keyboard;
int ss_storage_read(uint64_t sector,uint8_t data[512])
{
    const struct ss_grant *g=ss_grant();
    if(!data||!g||!(g->devices&SS_DEVICE_USB_STORAGE)||ss_live()<0)return -1;
    unsigned state=atomic_load_explicit(&storage.state,memory_order_acquire);
    if(!state){storage.sector=sector;atomic_store_explicit(&storage.state,1,memory_order_release);return 0;}
    if(storage.sector!=sector)return -1;
    if(state==4)return -1;if(state!=3)return 0;
    memcpy(data,storage.data,512);atomic_store_explicit(&storage.state,0,memory_order_release);return 1;
}
int ss_key_report(struct ss_key_report *report)
{
    const struct ss_grant *g=ss_grant();
    if(!report||!g||!(g->devices&SS_DEVICE_USB_KEYBOARD)||ss_live()<0)return -1;
    if(!atomic_load_explicit(&keyboard.ready,memory_order_acquire))return 0;
    *report=keyboard.report;atomic_store_explicit(&keyboard.ready,0,memory_order_release);return 1;
}
static int serve(void)
{
    struct a733_usb2_native *n=&board.host;
    unsigned state=atomic_load_explicit(&storage.state,memory_order_acquire);
    if(state==1){
        if(!a733_usb2_read_sector(n,storage.sector,n->platform.config.lifetime_deadline))return 0;
        atomic_store_explicit(&storage.state,2,memory_order_release);state=2;
    }
    if(state==2){
        int r=a733_usb2_read_result(n,storage.data);if(r<0)return 0;
        if(r==1){memcpy(board.sector,storage.data,512);atomic_store_explicit(&board.result,1,memory_order_release);
            atomic_store_explicit(&storage.state,3,memory_order_release);}
    }
    if(os_session_devices()&SS_DEVICE_USB_KEYBOARD){
        if(!n->hid_mounted||n->hid.subclass!=1||n->hid.protocol!=1)return 0;
        if(n->hid.pending){
            struct a733_usb2_hid_report raw;int r=a733_usb2_hid_result(n,&raw);if(r<0)return 0;
            if(r==1){
                if(raw.generation!=n->core.generation||raw.generation!=n->platform.attachment.generation||raw.bytes!=8||
                   atomic_load_explicit(&keyboard.ready,memory_order_acquire))return 0;
                keyboard.report=(struct ss_key_report){.attachment=raw.generation,.sequence=raw.sequence,.address=raw.address,
                    .speed=raw.speed,.interface=raw.interface,.protocol=raw.protocol,.endpoint=raw.endpoint,
                    .subclass=1,.mode=0,.bytes=8};
                memcpy(keyboard.report.data,raw.data,8);atomic_store_explicit(&keyboard.ready,1,memory_order_release);
            }
        }
        if(!n->hid.pending&&!atomic_load_explicit(&keyboard.ready,memory_order_acquire)&&
           !a733_usb2_hid_receive(n,n->platform.config.lifetime_deadline))return 0;
    }
    return 1;
}
void os_storage_service(void){if(!serve())board_fail(NULL);}
void board_host_task(void *unused)
{
    (void)unused;board.host.dual_leaf=!!(os_session_devices()&SS_DEVICE_USB_KEYBOARD);
    for(;;){
        if(!board_live(NULL))break;
#ifdef SUNSTONE_BOARD_USB2_IRQ
        int ready=board_host_irq_before();if(ready<0)break;
        if(!ready){board_host_irq_wait();continue;}
#endif
        enum a733_usb2_result r=a733_usb2_native_poll(&board.host);
        if(r==A733_USB2_RETAINED||r==A733_USB2_INVALID)break;
#ifdef SUNSTONE_BOARD_USB2_IRQ
        if(board_host_irq_after()<0)break;
#endif
        os_application_host_observed(r==A733_USB2_READY);
        if(r==A733_USB2_READY&&!serve())break;
#ifdef SUNSTONE_BOARD_USB2_IRQ
        board_host_irq_wait();
#else
        vTaskDelay(1);
#endif
    }
#ifdef SUNSTONE_BOARD_USB2_IRQ
    board_host_irq_stop();
#endif
    atomic_store_explicit(&storage.state,4,memory_order_release);
    a733_usb2_native_lost(&board.host);a733_typec_revoke(&board.typec.provider);
    atomic_store_explicit(&board.result,2,memory_order_release);board_fail(NULL);
    for(;;)vTaskSuspend(NULL);
}
