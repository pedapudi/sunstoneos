/* Keep the board's actual USB0 owner, setup and polling, replacing its exchange. */
#define board_usb0_task mimus_unused_usb0_task
#define exchange mimus_unused_exchange
#define tud_vendor_tx_cb mimus_unused_tx
#define BOARD_USB0_STOP() do{board_fail(NULL);if(a733_board_host_workload(board.session.workload))a733_typec_revoke(&board.typec.provider);}while(0)
#include "mimus-usb-base.c"
#undef board_usb0_task
#undef exchange
#undef tud_vendor_tx_cb
#include "application.h"
#ifdef SUNSTONE_A733_FAULT_PAGE
#include "a733_fault.h"
/* Operation 5 returns the previous boot's 1,160-byte fault report alone,
 * without the 64-byte frame header (docs/fault-capture.md). */
#define FAULT_OP 5u
static uint8_t frame[sizeof(struct a733_fault_report)],request[16];
_Static_assert(sizeof(struct a733_fault_report)>=1088,"frame holds every response");
#else
static uint8_t frame[1088],request[16];
#endif
#ifdef SUNSTONE_A733_OS_NPU
#include "../os/npu.h"
/* Operation 6 returns the OS NPU client's 104-byte report alone, without the
 * 64-byte frame header (apps/os/npu.h). It reads no recognizer state. */
_Static_assert(sizeof(frame)>=OS_NPU_REPORT_BYTES,"frame holds the NPU report");
#endif
static unsigned frame_bytes,sent,transferred,request_used,frame_success;
/* USB0 owner only. Receipt publication follows the final TX completion. */
static unsigned delivery_generation,delivered_text,delivered_sector,delivered_trigger;
static unsigned response_generation,response_op,response_offset,response_count,response_total;
static int delivered(void)
{return delivered_text==response_total&&delivered_sector&&delivered_trigger;}

void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
    if(interface||!board_live(NULL)||(frame_success&&mimus_board_export_live()<0)||bytes>sent-transferred)stop();
    transferred+=bytes;if(transferred==frame_bytes&&sent==frame_bytes){
        if(frame_success&&mimus_board_keyboard()){
            if(response_generation!=mimus_board_generation())stop();
            if(response_op==1&&response_offset<=delivered_text&&response_offset+response_count>delivered_text)
                delivered_text=response_offset+response_count;
            if(response_op==2)delivered_sector=1;
            if(response_op==3)delivered_trigger=1;
            if(response_op==4&&(!delivered()||!mimus_board_ack(response_generation)))stop();
        }
        frame_bytes=0;
    }
}
static void exchange(void)
{
    BOARD_PROGRESS(BOARD_SERVICE_USB0);
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
    int keyboard=mimus_board_keyboard();unsigned current=mimus_board_generation();
    uint64_t expected=0;for(unsigned i=0;i<8;++i)expected|=(uint64_t)request[8+i]<<(8*i);
#ifdef SUNSTONE_A733_FAULT_PAGE
    if(op==FAULT_OP){
        if(request[1]||offset||bytes||expected)stop();
        memcpy(frame,&a733_fault_previous,sizeof(a733_fault_previous));
        frame_success=0;sent=transferred=0;frame_bytes=sizeof(a733_fault_previous);return;
    }
#endif
#ifdef SUNSTONE_A733_OS_NPU
    if(op==OS_NPU_REPORT_OP){
        if(request[1]||offset||bytes||expected)stop();
        os_npu_report_copy(frame);
        frame_success=0;sent=transferred=0;frame_bytes=OS_NPU_REPORT_BYTES;return;
    }
#endif
    if(op>(keyboard?4u:mimus_board_staged()?1u:2u)||request[1]||bytes>1024)stop();
    if((!keyboard||op==0)&&expected)stop();
    if(keyboard&&delivery_generation!=current){
        delivery_generation=current;delivered_text=delivered_sector=delivered_trigger=0;
    }
    if((op==0&&(offset||bytes))||(op==1&&!bytes)||(op==2&&(offset||bytes!=512))||(op==3&&(offset||bytes!=128))||(op==4&&(offset||bytes)))stop();
    memset(frame,0,1088);size_t copied=0;uint32_t total=0;
    int r=mimus_board_copy(op==1?offset:0,op==1?bytes:0,frame+64,&copied,&total);
    unsigned storage=atomic_load_explicit(&board.result,memory_order_acquire);
    if(!board_live(NULL)||storage==2||(keyboard&&op&&expected!=current))r=-1;
    if(storage!=(mimus_board_staged()?3u:1u)&&r==1)r=0;
    if(r==1){int live=mimus_board_export_live();if(live!=1)r=live;}
    if(op==2&&r==1){memcpy(frame+64,board.sector,512);copied=512;}
    if(op==3&&r==1){r=mimus_board_trigger_copy(frame+64);copied=r==1?128:0;}
    response_total=total;
    if(op==4&&r==1&&!delivered())r=-1;
    if(keyboard&&mimus_board_generation()!=current)r=0;
    if(r!=1)copied=0;
    put32(frame,0x424d494d);put32(frame+4,keyboard?3:1);put32(frame+8,r<0?2:r?1:0);put32(frame+12,(uint32_t)copied);
    put32(frame+16,total);put32(frame+20,offset);put32(frame+24,storage);put32(frame+28,op);
    for(unsigned i=0;i<8;++i){frame[32+i]=(uint8_t)(board.session.epoch>>(8*i));frame[40+i]=(uint8_t)(board.session.generation>>(8*i));}
    if(keyboard){put32(frame+48,current);put32(frame+56,UINT32_MAX);}
    response_generation=current;response_op=op;response_offset=offset;response_count=(unsigned)copied;
    frame_success=r==1;sent=transferred=0;frame_bytes=64+(unsigned)copied;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    mimus_board_usb0_started();
    usb0_service_run(exchange);
}
