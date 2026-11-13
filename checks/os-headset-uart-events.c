/* Execute the actual OS binding on the real host FreeRTOS kernel/deadline
 * backend. UART, GIC and application boundaries are modeled. Controller/H4
 * and exact protocol metadata queries have their own composition checks. */
#include "host_rtos.h"
uint64_t a733_smp_counter(void);
int a733_cpu_coherence_publish(uintptr_t,size_t,uint64_t);
#define a733_uart_platform_controller model_controller
#define a733_uart_platform_irq_sync model_sync
#define a733_uart_rx_empty model_rx_empty
#ifndef HEADSET_SOURCE
#define HEADSET_SOURCE "../apps/os/headset.c"
#endif
#include HEADSET_SOURCE
#undef a733_uart_platform_controller
#undef a733_uart_platform_irq_sync
#undef a733_uart_rx_empty
int a733_uart_rx_empty(struct a733_uart *,uint64_t);
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct board_application board;
struct board_provider providers;
static struct headset_memory fixture_memory;
static const char *scenario;
static unsigned grants=OS_GRANT_HEADSET,board_up=1,closing;
static unsigned enabled,active,mask_calls,rearm_calls,sync_calls,device_reads;
static unsigned application_calls,retain_calls,stop_calls,failed_board;
static unsigned initialized,tx_enabled,rx_bytes,rx_timeout,unclassified;
static unsigned layers,blocked,modeled_fault,rearm_race,stuck_rearm;
static uint64_t work_generation,sync_until,protocol_until=2000000;
static uint64_t first_delivered,second_delivered,return_until=1900000;
static unsigned saw_masked_without_tx,saw_short_rearm;
static unsigned uart_busy,uart_iir_reads,uart_usr_reads,uart_rfl_reads;
static int uart_read(void *unused,uintptr_t address,uint32_t *value)
{
    (void)unused;assert(!host_in_isr()&&!enabled&&xTaskGetCurrentTaskHandle()==headset.owner);
    unsigned offset=(unsigned)(address-app.uart.uart.plan.base);
    if(offset==8){++uart_iir_reads;*value=uart_busy?7:1;}
    else if(offset==0x7c){++uart_usr_reads;*value=6;uart_busy=0;}
    else if(offset==0x84){++uart_rfl_reads;*value=rx_bytes;}
    else abort();
    return 1;
}
static int uart_time(void *unused,uint64_t *value)
{(void)unused;*value=host_time_us();return 1;}
static void tx_irq(unsigned on)
{tx_enabled=on;if(on)app.uart.uart.ier|=2u;else app.uart.uart.ier&=~2u;}

int board_live(void *unused){(void)unused;return board_up;}
void board_fail(void *unused){(void)unused;++failed_board;board_up=0;}
unsigned board_session_lifetime(void){return A733_LIFETIME_RETURN;}
void board_service_progress(unsigned service)
{assert(service==OS_SERVICE_BLUETOOTH||service==OS_SERVICE_HEADSET);}
int os_granted(unsigned mask){return mask&&(grants&mask)==mask;}
int os_cpu_return_requested(void){return (int)closing;}
int os_cpu_return_failing(void){return (int)closing;}
uint64_t os_cpu_return_deadline_us(void){return return_until;}
int board_service_deadline_register(unsigned service)
{assert(service==OS_SERVICE_BLUETOOTH||service==OS_SERVICE_HEADSET);return sunstone_deadline_register();}
int board_service_blocked_until(unsigned service,uint64_t until)
{
    assert(service==OS_SERVICE_BLUETOOTH||service==OS_SERVICE_HEADSET);
    assert(until>host_time_us()&&until<=board.session.deadline_us);return 1;
}
int bluetooth_headset_segment_stop(struct bluetooth_application *application)
{assert(application==&app);return 1;}
int bluetooth_application_retain(struct bluetooth_application *application)
{
    assert(application==&app&&xTaskGetCurrentTaskHandle()==headset.owner&&!host_in_isr());
    ++retain_calls;atomic_store(&app.failed,1);return -1;
}
int bluetooth_application_work_state(const struct bluetooth_application *application,struct bluetooth_work_state *state)
{
    assert(application==&app&&xTaskGetCurrentTaskHandle()==headset.owner&&!host_in_isr());
    memset(state,0,sizeof(*state));memcpy(state,&work_generation,sizeof(work_generation));return 1;
}
int bluetooth_application_rx_unclassified(const struct bluetooth_application *application)
{assert(application==&app);return (int)unclassified;}
uint64_t bluetooth_application_next_deadline(const struct bluetooth_application *application,uint64_t until)
{assert(application==&app);return protocol_until<until?protocol_until:until;}
int gicv3_spi_mask(const struct gicv3_spi *source)
{
    assert(source->distributor==0x03400000&&source->intid==35);
    assert(host_in_isr()||host_critical_depth());++mask_calls;enabled=0;return 1;
}
static void deliver(void *);
int gicv3_spi_rearm(const struct gicv3_spi *source)
{
    assert(source->intid==35&&host_critical_depth()&&!host_in_isr());++rearm_calls;
    if(stuck_rearm)return 0;
    if(!strcmp(scenario,"late-rearm")){
        if(rearm_calls==1)return 0;
        host_advance_us(headset_irq.budget_us+1);
    }
    enabled=1;
    if(rx_bytes&&!tx_enabled)saw_short_rearm=1;
    if(rearm_race){rearm_race=0;host_irq_at(host_time_us(),deliver,NULL);}
    else if(tx_enabled||uart_busy)host_irq_at(host_time_us()+1,deliver,NULL);
    return 1;
}
struct a733_uart *model_controller(struct a733_uart_platform *uart)
{
    assert(uart==&app.uart&&!host_in_isr()&&xTaskGetCurrentTaskHandle()==headset.owner);
    return app.uart.uart.state==A733_UART_READY?&app.uart.uart:NULL;
}
enum a733_uart_platform_result model_sync(struct a733_uart_platform *uart,uint64_t until)
{
    assert(uart==&app.uart&&!enabled&&!host_in_isr()&&!host_critical_depth());
    assert(until>host_time_us());++sync_calls;
    if(!strcmp(scenario,"slow-sync")&&sync_calls==1){host_advance_us(250);return A733_UART_PLATFORM_PENDING;}
    return active||host_time_us()<sync_until?A733_UART_PLATFORM_PENDING:A733_UART_PLATFORM_READY;
}
int model_rx_empty(struct a733_uart *uart,uint64_t until)
{
    assert(uart==&app.uart.uart&&xTaskGetCurrentTaskHandle()==headset.owner&&!host_in_isr());
    assert(until>host_time_us());++device_reads;
    if(!strcmp(scenario,"software-busy"))return a733_uart_rx_empty(uart,until);
    return rx_bytes?0:1;
}
int bluetooth_application_poll(struct bluetooth_application *application)
{
    assert(application==&app&&xTaskGetCurrentTaskHandle()==headset.owner&&!enabled&&!active);
    ++application_calls;
    if(!strcmp(scenario,"software-busy")){
        /* An upper CONFIGURE/software-only pass need not classify IIR. */
        if(application_calls==1)return 1;
        struct a733_uart_result result;
        assert(a733_uart_interrupt_rx_first(&app.uart.uart,NULL,0,NULL,0,1,board.session.deadline_us,&result)==A733_UART_FAULT);
        atomic_store(&app.failed,1);return -1;
    }
    if(modeled_fault){atomic_store(&app.failed,1);return -1;}
    if(!strcmp(scenario,"protocol-expiry")&&host_time_us()>=protocol_until){
        atomic_store(&app.failed,1);return -1;
    }
    if(!strcmp(scenario,"late")){
        if(atomic_load(&headset_irq.since))host_advance_us(headset_irq.budget_us+1);
        return 1;
    }
    if(blocked)return 1;
    if(!strcmp(scenario,"backlog-tx")&&unclassified){
        --unclassified;++work_generation;tx_irq(0);return 1;
    }
    if(!strcmp(scenario,"backlog-tx")&&!tx_enabled&&app.hci.transport.tx_offset<app.hci.transport.tx_length){
        tx_irq(1);++work_generation;return 1;
    }
    if(layers){
        --layers;++work_generation;
        if(!layers){app.hci.transport.tx_length=4;tx_irq(1);}
        return 1;
    }
    if(rx_bytes){
        if(!rx_timeout){tx_irq(0);saw_masked_without_tx=1;return 1;}
        rx_bytes=0;unclassified=1;++work_generation;return 1;
    }
    if(unclassified){unclassified=0;++work_generation;return 1;}
    if(tx_enabled){tx_irq(0);app.hci.transport.tx_offset=app.hci.transport.tx_length;++work_generation;}
    return 1;
}
int bluetooth_application_stop(struct bluetooth_application *application,uint64_t until)
{
    assert(application==&app&&xTaskGetCurrentTaskHandle()==headset.owner&&!enabled&&until==return_until);
    ++stop_calls;
    if(!strcmp(scenario,"stop-fault"))return -1;
    ++app.stop_phase;++work_generation;
    if(stop_calls<3)return 0;
    app.hci.state=BT_HCI_STOPPED;app.uart.uart.state=A733_UART_DRAINED;return 1;
}
static void deliver(void *unused)
{
    (void)unused;assert(host_in_isr()&&enabled);
    active=1;
    if(!first_delivered)first_delivered=host_time_us();else second_delivered=host_time_us();
    os_headset_uart_irq();active=0;
}
static void short_timeout(void *unused)
{(void)unused;rx_timeout=1;assert(enabled);deliver(NULL);}
static void input(void *unused)
{
    (void)unused;rx_bytes=3;
    if(strcmp(scenario,"short-rx")&&strcmp(scenario,"repeat-irq"))rx_timeout=1;
    if(!strcmp(scenario,"stuck-sync"))sync_until=UINT64_MAX;
    if(!strcmp(scenario,"sync-delay"))sync_until=host_time_us()+100;
    deliver(NULL);
}
static void initialize_owner(void)
{
    memory=&fixture_memory;headset.owner=xTaskGetCurrentTaskHandle();
    app.self=&app;app.owner=headset.owner;app.profile=&profile;
    app.deadline=board.session.deadline_us;app.firmware_generation=1;atomic_store(&app.result,1);
    app.hci.self=&app.hci;app.hci.state=BT_HCI_IDENTIFIED;
    app.hci.transport.state=BT_H4_ACTIVE;app.hci.transport.deferred_irq=1;
    app.hci.transport.count=2;app.hci.transport.assembling=2;
    app.uart.config.deferred_irq=1;app.uart.uart.state=A733_UART_READY;
    if(!strcmp(scenario,"software-busy")){
        assert(a733_uart_plan(&profile.uart.format,&app.uart.uart.plan)==1);
        app.uart.uart.ops=(struct a733_uart_ops){.read32=uart_read,.now_us=uart_time};
        uart_busy=1;
    }
    assert(irq_profile(&profile,&headset_irq.pre_us));
    headset_irq.budget_us=profile.headset.receive_latency_us-headset_irq.pre_us;
    assert(headset_irq.pre_us==3414&&headset_irq.budget_us==1586);
    headset_irq.bound=1;headset_irq.masked=1;
    if(!strcmp(scenario,"legacy-idle"))headset_irq.bound=0;
    if(!strcmp(scenario,"chain"))layers=3;
    if(!strcmp(scenario,"backlog-tx")){unclassified=2;app.hci.transport.tx_length=4;}
    if(!strcmp(scenario,"blocked")){layers=3;blocked=1;}
    if(!strcmp(scenario,"stuck-rearm"))stuck_rearm=1;
    if(!strcmp(scenario,"rearm-race")){rearm_race=1;rx_bytes=3;rx_timeout=1;}
    initialized=1;
}
static void run_owner(void *unused){(void)unused;initialize_owner();owner(NULL);}
static void run_shared(void *unused)
{
    (void)unused;initialize_owner();
    assert(board_service_deadline_register(OS_SERVICE_BLUETOOTH));
    for(;;){
        os_module_poll();
        if(atomic_load(&headset.failed)||os_quiesce_result(&headset_quiesce)==OS_QUIESCE_FAULT){vTaskSuspend(NULL);return;}
        uint64_t until=os_module_next_deadline(board.session.deadline_us);
        assert(until&&sunstone_deadline_wait_until(until)>=0);
    }
}
static void wait_for(uint64_t until)
{while(host_time_us()<until)assert(sunstone_deadline_wait_until(until)>=0);}
static void test(void *unused)
{
    (void)unused;
    profile.uart.deferred_irq=1;
    profile.uart.format=(struct a733_uart_format){1,8,2,0,1,24000000,1500000,0};
    profile.headset.receive_latency_us=5000;
    if(!strcmp(scenario,"budget")){
        uint64_t pre=0;assert(irq_profile(&profile,&pre)&&pre==3414);
        profile.headset.receive_latency_us=3414;assert(!irq_profile(&profile,NULL));
        profile.headset.receive_latency_us=3415;assert(irq_profile(&profile,NULL));
        profile.uart.deferred_irq=0;assert(!irq_profile(&profile,NULL));
        puts("PASS source-derived UART pre-IRQ budget and strict admission");return;
    }
    board.session.deadline_us=2000000;atomic_store(&providers.shared.pmic_ready,1);
    assert(os_quiesce_register(&headset_quiesce));assert(sunstone_deadline_register());
    host_timer_handler(sunstone_deadline_irq);
    if(!strcmp(scenario,"protocol-expiry"))protocol_until=100123;
    if(!strcmp(scenario,"shared"))grants|=OS_GRANT_RADIO;
    assert(host_task_create("headset owner",!strcmp(scenario,"shared")?run_shared:run_owner,NULL,3));
    assert(initialized);
    if(!strcmp(scenario,"software-busy")){
        assert(application_calls==1&&uart_busy&&uart_rfl_reads==1&&!uart_iir_reads&&!uart_usr_reads);
        wait_for(100);assert(application_calls==2&&uart_iir_reads==1&&uart_usr_reads==1&&uart_rfl_reads==1);
        assert(failed_board&&atomic_load(&headset.failed)&&!enabled);
        unsigned before=device_reads;wait_for(1000);assert(device_reads==before);
        puts("PASS software-only upper pass preserves BUSY7 until actual IIR fault service");return;
    }
    if(!strcmp(scenario,"legacy-idle")){
        wait_for(1000000);assert(application_calls==1001&&os_headset_wake_record[1].wakes==1000);
        puts("PASS precise legacy polling records 1000 idle wakes per second");return;
    }
    if(!strcmp(scenario,"slow-sync")){
        assert(headset_irq.retry_at==449&&sync_calls==1);
        wait_for(400);assert(sync_calls==1&&!application_calls);
        wait_for(500);assert(sync_calls==2&&application_calls==1&&enabled);
        puts("PASS slow synchronization schedules the next observation in the future");return;
    }
    if(!strcmp(scenario,"stuck-rearm")||!strcmp(scenario,"late-rearm")){
        wait_for(2000);assert(retain_calls==1&&failed_board&&atomic_load(&headset.failed)&&!enabled);
        unsigned before=application_calls;wait_for(5000);assert(application_calls==before);
        puts("PASS stuck rearm expires once and retains without further service");return;
    }
    if(!strcmp(scenario,"protocol-expiry")){
        wait_for(100200);assert(failed_board&&atomic_load(&headset.failed));
        puts("PASS precise upper-protocol deadline is observed off the tick grid");return;
    }
    if(!strcmp(scenario,"quiesce")||!strcmp(scenario,"stop-fault")){
        closing=1;(void)os_quiesce_step(NULL);
        wait_for(10);
        if(!strcmp(scenario,"quiesce"))assert(stop_calls==3&&os_quiesce_step(NULL)==OS_QUIESCE_DONE&&!enabled);
        else assert(stop_calls==1&&os_quiesce_step(NULL)==OS_QUIESCE_FAULT&&!enabled);
        unsigned before=device_reads;wait_for(1000);assert(device_reads==before);
        puts("PASS registered owner-only quiesce and retained stop failure");return;
    }
    if(!strcmp(scenario,"chain")){
        wait_for(100);assert(!layers&&app.hci.transport.tx_offset==4&&application_calls>=5&&os_headset_irq_record.software_passes>=2);
        unsigned before=application_calls;wait_for(1000000);assert(application_calls==before);
        puts("PASS copied upper work drains to THRE without a periodic wake");return;
    }
    if(!strcmp(scenario,"backlog-tx")){
        wait_for(100);assert(!unclassified&&app.hci.transport.tx_offset==4&&application_calls>=4);
        assert(os_headset_irq_record.software_passes>=2&&os_headset_irq_record.deliveries==1);
        unsigned before=application_calls;wait_for(1000000);assert(application_calls==before);
        puts("PASS software RX backlog drains before pending TX can rely on THRE");return;
    }
    if(!strcmp(scenario,"blocked")){
        wait_for(1000000);assert(application_calls==1&&layers==3&&!os_headset_irq_record.software_passes);
        puts("PASS unchanged blocked-credit metadata causes no repeated wake");return;
    }
    if(!strcmp(scenario,"rearm-race")){
        wait_for(100);assert(os_headset_irq_record.deliveries&&application_calls>=2&&!rx_bytes&&!unclassified);
        puts("PASS IRQ publication during rearm survives the arm-check race");return;
    }
    wait_for(1000000);assert(application_calls==1&&os_headset_wake_record[1].wakes==0&&enabled);
    host_irq_at(1000123,input,NULL);
    if(!strcmp(scenario,"short-rx"))host_irq_at(1000150,short_timeout,NULL);
    if(!strcmp(scenario,"repeat-irq")){
        host_irq_at(1000200,input,NULL);host_irq_at(1000250,short_timeout,NULL);
        wait_for(1000220);assert(atomic_load(&headset_irq.since)==1000124&&second_delivered==1000200);
    }
    wait_for(1002000);
    if(!strcmp(scenario,"late")||!strcmp(scenario,"stuck-sync")){
        assert(atomic_load(&headset.failed)&&failed_board&&!enabled);
        unsigned before=application_calls;wait_for(1005000);assert(application_calls==before);
        puts("PASS earliest pending IRQ budget expires with resources retained");return;
    }
    assert(!rx_bytes&&!unclassified&&!atomic_load(&headset_irq.since)&&enabled);
    if(!strcmp(scenario,"short-rx"))assert(saw_masked_without_tx&&saw_short_rearm&&second_delivered==1000150);
    if(!strcmp(scenario,"sync-delay"))assert(os_headset_irq_record.sync_waits&&os_headset_irq_record.max_classification_us>=199);
    unsigned before=application_calls;wait_for(1100000);assert(application_calls==before);
    puts("PASS zero idle wakes, notification service, preserved IRQ age and RX timeout rearm");
}
int main(int argc,char **argv)
{assert(argc==2);scenario=argv[1];host_rtos_run(test,NULL,2,3000000);return 0;}
