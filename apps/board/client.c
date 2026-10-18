#include "application.h"
void board_host_task(void *unused)
{
    (void)unused;unsigned issued=0;
    for(;;){
        if(!board_live(NULL))break;
        enum a733_usb2_result r=a733_usb2_native_poll(&board.host);
        if(r==A733_USB2_RETAINED||r==A733_USB2_INVALID)break;
        if(r==A733_USB2_READY&&!issued){
            if(!a733_usb2_read_sector(&board.host,0,board.host.platform.config.lifetime_deadline))break;
            issued=1;
        }
        if(issued==1){int done=a733_usb2_read_result(&board.host,board.sector);
            if(done<0)break;
            if(done==1){atomic_store_explicit(&board.result,1,memory_order_release);
                /* Retain copied bytes, but keep observing host attachment,
                 * controller faults and the absolute lifetime. */
                issued=2;
            }
        }
        vTaskDelay(1);
    }
    a733_usb2_native_lost(&board.host);
    a733_typec_revoke(&board.typec.provider);
    atomic_store_explicit(&board.result,2,memory_order_release);
    for(;;)vTaskDelay(1);
}
