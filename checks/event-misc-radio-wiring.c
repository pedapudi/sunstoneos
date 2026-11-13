#include "host_rtos.h"
#include "sunstone_deadline.h"
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#define SUNSTONE_A733_OS 1
#define SUNSTONE_A733_RADIO_IRQ 1
#define OS_GRANT_HEADSET 64u
#define OS_SERVICE_RADIO 5u
#define OS_SERVICE_NETWORK 6u
#define SUNSTONE_CRUMB_HERE() ((const void *)1)
enum board_radio_result {BOARD_RADIO_PENDING,BOARD_RADIO_RETAINED,BOARD_RADIO_INVALID};
static struct {unsigned platform;} radio;
static struct {TaskHandle_t handles[8];_Atomic unsigned radio_result,radio_bytes,watchdog_state;struct {unsigned provider;} typec;} board;
static struct {struct {_Atomic unsigned pmic_ready;} shared;} providers;
static jmp_buf pass_end;
static unsigned mode,requested,terminal,headset=1,live=1,faults,revokes,waits,polls,progress,parks,module_polls,notices,registered[7],certificates[7];
static uint64_t module_until=4000000;
static int os_radio_quiesce_parked(void){return terminal;}
static int os_radio_quiesce_requested(void){return requested;}
static int os_radio_quiesce_poll(void *r,int result){assert(r==&radio);(void)result;return 1;}
static void module_step(void){++module_polls;}
static uint64_t module_deadline(uint64_t until){assert(until==5000000);return module_until;}
static int module_pending(void){return 1;}
static void (*os_module_poll)(void)=module_step;
static uint64_t (*os_module_next_deadline)(uint64_t)=module_deadline;
static int (*os_headset_quiesce_pending)(void)=module_pending;
static int os_granted(unsigned grant){assert(grant==OS_GRANT_HEADSET);return headset;}
static void os_wifi_credential_poll(void){}
static int board_live(void *unused){assert(!unused);return live;}
static uint64_t board_runtime_deadline_us(void){return 5000000;}
static int board_radio_external_failure(void *r){assert(r==&radio);return 0;}
static enum board_radio_result board_radio_poll(void *r){assert(r==&radio);++polls;return BOARD_RADIO_PENDING;}
static int board_radio_client_poll(void *r){assert(r==&radio);++polls;return 1;}
static int board_radio_client_result(void *r,unsigned *verified){assert(r==&radio);*verified=0;return 0;}
static int a733_radio_platform_clean(void *p){assert(p==&radio.platform);return 1;}
static int os_radio_quiesce_fault(uint32_t value,const void *caller){assert(value==0x909||value==0x90a);(void)caller;++faults;return requested;}
static void a733_typec_revoke(void *p){assert(p==&board.typec.provider);++revokes;}
static void board_progress(unsigned service){assert(service==(mode==1||mode==3||mode==4?OS_SERVICE_NETWORK:OS_SERVICE_RADIO));++progress;}
#define BOARD_PROGRESS(service) board_progress(service)
static _Noreturn void park(void){++parks;longjmp(pass_end,1);}
static void observe(void *unused)
{(void)unused;assert(waits==1&&polls==1&&eTaskGetState(xTaskGetCurrentTaskHandle())==eRunning);assert(eTaskGetState(board.handles[mode==1?6:5])==eBlocked);}
static void publication(void *unused)
{(void)unused;xTaskNotifyGive(board.handles[mode==1?6:5]);}
static void notice_peer(void *unused)
{
    (void)unused;assert(sunstone_deadline_register());
    assert(sunstone_deadline_wait_until(5000000)==SUNSTONE_DEADLINE_NOTIFIED);
    assert(atomic_load(&board.radio_result)==3&&atomic_load(&board.watchdog_state)==4);++notices;
    vTaskSuspend(NULL);
}
static int board_radio_owner_wait(void *r,uint64_t until)
{
    assert(r==&radio&&until==(headset?module_until:5000000));assert(host_time_us()==0);++waits;
    if(mode==2)return -1;
    assert(sunstone_deadline_wait_until(until)==SUNSTONE_DEADLINE_NOTIFIED);
    assert(host_time_us()==2000000);longjmp(pass_end,1);
}
static int board_radio_client_wait(void *r)
{
    assert(r==&radio&&host_time_us()==0);++waits;
    if(mode==3)return -1;
    assert(sunstone_deadline_wait_until(5000000)==SUNSTONE_DEADLINE_NOTIFIED);
    assert(host_time_us()==2000000);longjmp(pass_end,1);
}
#include "radio-loops.inc"
static int board_service_deadline_register(unsigned service){assert(service==5||service==6);++registered[service];return 1;}
static int board_service_blocked_until(unsigned service,uint64_t until){assert(until>host_time_us());++certificates[service];return 1;}
static void *os_board_radio(void){return &radio;}
static unsigned irq_calls;
static void board_radio_irq(void *r){assert(r==&radio);++irq_calls;}
#include "radio-shims.inc"
static void owner(void *unused)
{
    (void)unused;TaskHandle_t self=xTaskGetCurrentTaskHandle();assert(sunstone_deadline_register());
    unsigned index=mode==1||mode==3||mode==4?6:5;board.handles[index]=self;
    assert(!radio_blocked_until(index,11-index,4000000));
    assert(radio_blocked_until(index,index,4000000)&&radio_blocked_until(index,index,4000000));
    assert(registered[index]==1&&certificates[index]==2);os_radio_irq();assert(irq_calls==1);
    atomic_store(&providers.shared.pmic_ready,1);
    if(mode<2){host_run_at(1000000,observe,NULL);host_run_at(2000000,publication,NULL);}
    if(mode==2||mode==3){
        board.handles[11-index]=host_task_create("peer",notice_peer,NULL,5);
        taskYIELD();
    }
    if(mode==4)requested=1;
    if(mode==5)module_until=0;
    if(!setjmp(pass_end)){if(index==6)radio_client(NULL);else radio_owner(NULL);}
    if(mode<2)assert(waits==1&&polls==1&&!parks&&!faults&&!revokes&&progress==1);
    else if(mode==4)assert(parks==1&&!waits&&!polls&&!faults);
    else if(mode==5)assert(parks==1&&!waits&&faults==1&&revokes==1);
    else{assert(parks==1&&waits==1&&faults==1&&revokes==1);taskYIELD();assert(notices==1);}
}
int main(int argc,char **argv)
{
    assert(argc==2);mode=(unsigned)strtoul(argv[1],NULL,0);assert(mode<6);
    host_timer_handler(sunstone_deadline_irq);host_rtos_run(owner,NULL,4,6000000);
    puts("PASS generated radio owner/client waits and failure publications");
}
