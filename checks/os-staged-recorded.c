/* Actual patched application, recognizer copy and USB0 exchange. Device and
 * scheduler boundaries are modeled; this does not execute the engine. */
#include "application-native.c"
struct board_application board;
struct board_provider providers;
#ifdef SUNSTONE_A733_FAULT_PAGE
#include "a733_fault.h"
struct a733_fault_report a733_fault_previous;
#endif
volatile unsigned fixture_status,fixture_typec_reads,fixture_typec_revokes,fixture_usb2_reads;
volatile unsigned fixture_response_bytes,fixture_start_failed;
uint8_t fixture_packet[16],fixture_response[1088];
static unsigned available;
static uint64_t time_us=100;
static int provider_live=1;
extern void fixture_result(unsigned overlap);
static _Noreturn void finish(unsigned status)
{fixture_status=status;__asm__ volatile("brk #0");for(;;){}}
static void check(int ok,unsigned code){if(!ok)finish(code);}
int board_live(void *v){(void)v;return !atomic_load(&board.fatal)&&time_us<board.session.deadline_us;}
void board_fail(void *v){(void)v;atomic_store(&board.fatal,1);}
uint64_t a733_smp_counter(void){return time_us*24;}
int a733_typec_snapshot(struct a733_typec *p,struct a733_typec_token *out,uint64_t deadline)
{(void)p;(void)deadline;++fixture_typec_reads;*out=board.host.platform.attachment;return 1;}
void a733_typec_revoke(struct a733_typec *p){(void)p;++fixture_typec_revokes;}
enum a733_usb2_device_kind a733_usb2_device_kind(struct a733_usb2_native *p)
{++fixture_usb2_reads;return p->kind;}
static int host_live(void *v,uint64_t generation,uint64_t deadline)
{(void)v;(void)generation;(void)deadline;return provider_live;}
int mimus_keyboard_copy(uint8_t out[128])
{
    memset(out,0,128);uint32_t head[]={0x4752544b,1,128,2};memcpy(out,head,16);
    memcpy(out+16,&board.session.generation,8);
    const uint8_t fields[]={1,2,0,1,0x81,1,0,0};memcpy(out+24,fields,8);
    out[32]=1;out[80]=2;out[90]=0x28;return 1;
}
int os_thermal_throttled(void){return 0;}
void vTaskSuspend(TaskHandle_t t){(void)t;finish(90);}
void vTaskDelay(TickType_t t){(void)t;finish(91);}
static void put32(uint8_t *p,uint32_t n){memcpy(p,&n,4);}
static uint32_t tud_vendor_available(void){return available;}
static uint32_t tud_vendor_read(void *out,uint32_t n)
{check(n==16&&available==16,92);memcpy(out,fixture_packet,16);available=0;return n;}
static uint32_t tud_vendor_write(const void *p,uint32_t n)
{check(n<=sizeof(fixture_response),93);memcpy(fixture_response,p,n);fixture_response_bytes=n;return n;}
static uint32_t tud_vendor_write_flush(void){return 0;}
#define tud_vendor_flush tud_vendor_write_flush
/* The empty base includes only stop and startup wrappers below. It replaces
 * board USB hardware, not the application exchange. */
#include "../apps/mimus-board/usb0.c"
void fixture_init(unsigned workload)
{
    board.session.workload=workload;board.session.epoch=0x1234;board.session.generation=9;
    board.session.deadline_us=60000000;atomic_store(&board.result,workload==2?3:1);
    memset(board.sector,0x5a,sizeof(board.sector));
    if(workload!=2){
        board.host.kind=workload==4?A733_USB2_DEVICE_DUAL:A733_USB2_DEVICE_MSC;
        board.host.core.ops.live=host_live;board.host.platform.attachment.generation=9;
        board.host.platform.attachment.observed_us=time_us;
        mimus_board_host_observed(1);
    }
    fixture_result(workload!=2);fixture_status=1;__asm__ volatile("brk #0");
}
/* The actual USB task publishes readiness after the modeled service starts. */
void fixture_start(void){board_usb0_task(NULL);}
void fixture_query(void)
{
    available=16;fixture_response_bytes=0;
    /* Real service_pass checks health before calling exchange; model the
     * provider result at that boundary and use the actual stop revocation. */
    if(!provider_live)stop();
    exchange();exchange();tud_vendor_tx_cb(0,fixture_response_bytes);
    fixture_status=1;__asm__ volatile("brk #0");
}
void fixture_case(unsigned selected)
{
    uint8_t out[32];size_t copied=99;uint32_t total=99;
    switch(selected){
    case 0: /* No transcript before USB0 startup. */
        check(!mimus_board_live()&&!mimus_board_export_live(),10);break;
    case 1:
        check(mimus_board_live()&&mimus_board_export_live()==1,11);
        check(!fixture_typec_reads&&!fixture_typec_revokes&&!fixture_usb2_reads,12);break;
    case 2:
        board_fail(NULL);check(!mimus_board_live()&&mimus_board_export_live()==-1,13);
        check(mimus_board_copy(0,1,out,&copied,&total)==-1&&!copied&&!total,14);break;
    case 3:
        time_us=board.session.deadline_us;check(!mimus_board_live()&&mimus_board_export_live()==-1,15);break;
    case 4:
        atomic_store(&board.result,1);check(!mimus_board_live()&&!mimus_board_export_live(),16);break;
    case 5:
        check(mimus_board_copy(0,5,out,&copied,&total)==1&&copied==5&&total==5&&!memcmp(out,"test\n",5),17);break;
    case 6:
        fixture_result(0);check(mimus_board_copy(0,5,out,&copied,&total)==-1&&!copied&&!total,18);
        fixture_result(1);check(mimus_board_copy(0,5,out,&copied,&total)==1,19);break;
    case 7:
        check(mimus_board_export_live()==1,20);++board.host.platform.attachment.generation;
        check(mimus_board_export_live()==-1,21);break;
    case 8:
        time_us+=A733_TYPEC_FRESH_US;check(!mimus_board_live()&&mimus_board_export_live()==-1,22);break;
    case 9:
        provider_live=0;fixture_query();check(0,23);break;
    case 10:
        check(atomic_load(&board.fatal)&&!mimus_board_live()&&mimus_board_export_live()==-1,24);
        check(!fixture_typec_reads&&!fixture_typec_revokes&&!fixture_usb2_reads,25);break;
    default:check(0,26);
    }
    fixture_status=1;__asm__ volatile("brk #0");
}
