/* Actual USB0 frame publication; only endpoint I/O is modeled. */
#include "../apps/board/usb0.c"
#include "a733_storage_digest.h"
struct board_application board;
struct board_provider providers;
static struct board_radio_profile profile;
uint8_t board_check_frame[584];
unsigned board_check_frame_bytes;
volatile unsigned board_check_result;
static unsigned available=1;
void vTaskDelay(TickType_t ticks)
{board_check_result=100+(unsigned)ticks;__asm__ volatile("brk #0x733");for(;;);}
int board_live(void *unused){(void)unused;return 1;}
uint32_t tud_vendor_n_available(uint8_t interface){return interface?0:available;}
uint32_t tud_vendor_n_read(uint8_t interface,void *buffer,uint32_t bytes)
{if(interface||bytes!=1)return 0;*(uint8_t *)buffer=0;available=0;return 1;}
uint32_t tud_vendor_n_write(uint8_t interface,const void *buffer,uint32_t bytes)
{if(interface||bytes>sizeof(board_check_frame))return 0;memcpy(board_check_frame,buffer,bytes);board_check_frame_bytes=bytes;return bytes;}
uint32_t tud_vendor_n_write_flush(uint8_t interface){(void)interface;return 0;}
void board_check_run(unsigned mode)
{
    profile.magic=BOARD_RADIO_PROFILE_MAGIC;profile.version=3;profile.bytes=sizeof(profile);profile.response_bytes=5;
    board.session=(struct a733_board_session){.magic=A733_BOARD_SESSION_MAGIC,.version=1,.bytes=sizeof(board.session),.epoch=7,.generation=8,.workload=1};
    storage_digest_bytes(&profile,sizeof(profile),board.session.radio_profile_digest);
    atomic_init(&board.result,mode==2?0:1);atomic_init(&board.radio_result,2);atomic_init(&board.radio_bytes,5);
    atomic_init(&board.watchdog_state,mode==1?5:1);
    for(unsigned i=0;i<512;++i)board.sector[i]=(uint8_t)(i^0x5a);
    exchange();exchange();
    unsigned expected=mode==2?72:584;
    if(board_check_frame_bytes==expected&&response_sent==expected){
        tud_vendor_tx_cb(0,expected);
        if(!response_bytes)board_check_result=1;
    }
    __asm__ volatile("brk #0x733");for(;;);
}
