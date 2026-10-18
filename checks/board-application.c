/* Native client contract checks. Driver behavior is modeled; component transfer
 * and electrical evidence are reused rather than replayed here. */
#include "application.h"
#include <string.h>
struct board_application board;
static unsigned mode,polls,reads,copies,revoked,retained;
volatile unsigned board_check_result;
int board_live(void *p){(void)p;return 1;}
static void finish(unsigned result)
{board_check_result=result;__asm__ volatile("brk #0x733");for(;;);}
void vTaskDelay(TickType_t ticks)
{
    if(ticks!=1)finish(10);
    unsigned result=atomic_load_explicit(&board.result,memory_order_acquire);
    if(result==1){
        if(mode==1||reads!=1||copies!=1||revoked||retained||polls<2||polls>3)finish(11);
        for(unsigned i=0;i<512;++i)if(board.sector[i]!=(uint8_t)(i^0xa5))finish(12);
        if(polls==3&&mode==0)finish(1);
        return;
    }
    if(result==2){
        if(!mode||!revoked||!retained)finish(13);
        if(mode==1){
            if(reads||copies||polls!=1)finish(14);
            for(unsigned i=0;i<512;++i)if(board.sector[i]!=0xcc)finish(15);
        }else{
            if(reads!=1||copies!=1||polls!=3)finish(16);
            for(unsigned i=0;i<512;++i)if(board.sector[i]!=(uint8_t)(i^0xa5))finish(17);
        }
        finish(1);
    }
    if(polls>3)finish(15);
}
enum a733_usb2_result a733_usb2_native_poll(struct a733_usb2_native *n)
{if(n!=&board.host)finish(20);++polls;return mode==1||(mode==2&&polls==3)?A733_USB2_RETAINED:polls==1?A733_USB2_PENDING:A733_USB2_READY;}
int a733_usb2_read_sector(struct a733_usb2_native *n,uint32_t lba,uint64_t deadline)
{if(n!=&board.host||lba||deadline!=board.host.platform.config.lifetime_deadline||++reads!=1)finish(21);return 1;}
int a733_usb2_read_result(struct a733_usb2_native *n,uint8_t out[512])
{if(n!=&board.host||out!=board.sector||!reads)finish(22);++copies;for(unsigned i=0;i<512;++i)out[i]=(uint8_t)(i^0xa5);return 1;}
void a733_usb2_native_lost(struct a733_usb2_native *n)
{if(n!=&board.host)finish(23);retained=1;}
void a733_typec_revoke(struct a733_typec *p)
{if(p!=&board.typec.provider)finish(24);revoked=1;}
void board_check_run(unsigned which)
{mode=which;memset(board.sector,0xcc,512);board.host.platform.config.lifetime_deadline=1000000;board_host_task(0);}
