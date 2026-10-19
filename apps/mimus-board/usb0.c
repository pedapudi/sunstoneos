/* Keep the board's actual USB0 owner, setup and polling, replacing its exchange. */
#define board_usb0_task mimus_unused_usb0_task
#define exchange mimus_unused_exchange
#define tud_vendor_tx_cb mimus_unused_tx
#include "mimus-usb-base.c"
#undef board_usb0_task
#undef exchange
#undef tud_vendor_tx_cb
#include "application.h"
static uint8_t frame[1088],request[16];
static unsigned frame_bytes,sent,transferred,request_used,frame_success;
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
    if(interface||!board_live(NULL)||(frame_success&&mimus_board_export_live()<0)||bytes>sent-transferred)stop();
    transferred+=bytes;if(transferred==frame_bytes&&sent==frame_bytes)frame_bytes=0;
}
static void exchange(void)
{
    if(frame_bytes){
        if(!board_live(NULL))stop();
        if(frame_success){int live=mimus_board_export_live();if(live<0)stop();if(!live)return;}
        sent+=tud_vendor_write(frame+sent,frame_bytes-sent);tud_vendor_flush();return;
    }
    unsigned available=tud_vendor_available();if(!available)return;
    unsigned n=16-request_used;if(n>available)n=available;
    unsigned got=tud_vendor_read(request+request_used,n);if(!got||got>n)stop();request_used+=got;
    if(request_used!=16)return;request_used=0;
    unsigned op=request[0];uint32_t offset=0;for(unsigned i=0;i<4;++i)offset|=(uint32_t)request[4+i]<<(8*i);
    size_t bytes=request[2]|(unsigned)request[3]<<8;
    if(op>2||request[1]||bytes>1024)stop();
    for(unsigned i=8;i<16;++i)if(request[i])stop();
    if((op==0&&(offset||bytes))||(op==1&&!bytes)||(op==2&&(offset||bytes!=512)))stop();
    memset(frame,0,sizeof(frame));size_t copied=0;uint32_t total=0;
    int r=mimus_board_copy(op==1?offset:0,op==1?bytes:0,frame+64,&copied,&total);
    unsigned storage=atomic_load_explicit(&board.result,memory_order_acquire);
    if(!board_live(NULL)||storage==2)r=-1;
    if(storage!=1&&r==1)r=0;
    if(r==1){int live=mimus_board_export_live();if(live!=1)r=live;}
    if(op==2&&r==1){memcpy(frame+64,board.sector,512);copied=512;}
    if(r!=1)copied=0;
    put32(frame,0x424d494d);put32(frame+4,1);put32(frame+8,r<0?2:r?1:0);put32(frame+12,(uint32_t)copied);
    put32(frame+16,total);put32(frame+20,offset);put32(frame+24,storage);put32(frame+28,op);
    for(unsigned i=0;i<8;++i){frame[32+i]=(uint8_t)(board.session.epoch>>(8*i));frame[40+i]=(uint8_t)(board.session.generation>>(8*i));}
    frame_success=r==1;sent=transferred=0;frame_bytes=64+(unsigned)copied;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    for(;;){if(!board_live(NULL))stop();int r=a733_native_usb_step(&providers.shared);if(r<0)stop();if(r==1)break;vTaskDelay(1);}
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};if(!tusb_init(0,&init))stop();
    for(;;){if(!board_live(NULL))stop();tud_task();a733_usb_prepare();if(polling)dcd_int_handler(0);exchange();vTaskDelay(1);}
}
