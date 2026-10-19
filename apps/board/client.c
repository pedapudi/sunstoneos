#include "application.h"
#include <string.h>
static void put32(uint8_t *p,uint32_t n)
{for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n)
{put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
void board_host_task(void *unused)
{
    (void)unused;unsigned issued=0,reports=0;
    for(;;){
        if(!board_live(NULL))break;
        enum a733_usb2_result r=a733_usb2_native_poll(&board.host);
        if(r==A733_USB2_RETAINED||r==A733_USB2_INVALID)break;
        enum a733_usb2_device_kind kind=a733_usb2_device_kind(&board.host);
        if(r==A733_USB2_READY&&!issued){
            if(kind==A733_USB2_DEVICE_MSC){
                if(!a733_usb2_read_sector(&board.host,0,board.host.platform.config.lifetime_deadline))break;
            }else if(kind==A733_USB2_DEVICE_HID){
                memset(board.sector,0,sizeof(board.sector));
                if(!a733_usb2_hid_receive(&board.host,board.host.platform.config.lifetime_deadline))break;
            }else break;
            issued=1;
        }
        if(issued==1&&kind==A733_USB2_DEVICE_MSC){
            int done=a733_usb2_read_result(&board.host,board.sector);
            if(done<0)break;
            if(done==1){atomic_store_explicit(&board.result,1,memory_order_release);issued=2;}
        }else if(issued==1&&kind==A733_USB2_DEVICE_HID){
            struct a733_usb2_hid_report report;
            int done=a733_usb2_hid_result(&board.host,&report);if(done<0)break;
            if(done==1){
                if(!reports){
                    put32(board.sector,UINT32_C(0x52444948));put32(board.sector+4,1);
                    put32(board.sector+12,report.address);put64(board.sector+16,report.generation);
                    board.sector[24]=report.speed;board.sector[25]=report.interface;
                    board.sector[26]=report.protocol;board.sector[27]=report.endpoint;
                }
                uint8_t *entry=board.sector+32+reports*80;
                put64(entry,report.sequence);put32(entry+8,report.bytes);
                memcpy(entry+16,report.data,report.bytes);++reports;put32(board.sector+8,reports);
                if(reports==4){atomic_store_explicit(&board.result,1,memory_order_release);issued=2;}
                else if(!a733_usb2_hid_receive(&board.host,board.host.platform.config.lifetime_deadline))break;
            }
        }
        /* The published sector or four-report HID envelope is immutable.
         * Continue observing attachment, controller faults and lifetime. */
        vTaskDelay(1);
    }
    a733_usb2_native_lost(&board.host);
    a733_typec_revoke(&board.typec.provider);
    atomic_store_explicit(&board.result,2,memory_order_release);
    for(;;)vTaskDelay(1);
}
