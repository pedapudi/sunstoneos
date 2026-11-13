/* Execute the real thermal owner and precise timer on the host FreeRTOS
 * kernel. The reused cadence fixture models only THS, GIC and clock effects.
 * THS interrupt semantics and the 25 ms service cadence are documented in
 * docs/thermal.md and the source manifest it names. */
#include <assert.h>
#ifdef THERMAL_EVENT_BASELINE
#define idle_deadline() ((uint64_t)idle_ticks())
#endif
#define a733_thermal_stop event_stop
#define os_cpu_return_requested cadence_no_return_requested
#define main cadence_fixture_main
#include "os-thermal-cadence.c"
#undef main
#undef a733_thermal_stop
#undef os_cpu_return_requested
#include "sunstone_deadline.h"

static unsigned stops,stop_done;
static int returning;
static uint32_t gic_control;
static uint64_t phase;
static const char *scenario;
enum a733_thermal_result event_stop(struct a733_thermal *driver,uint64_t until)
{
    assert(driver==&thermal.driver&&until>host_time_us());
    ++stops;return A733_THERMAL_STOPPED;
}
void os_cpu_return_thermal_done(void){++stop_done;}
int os_cpu_return_requested(void){return returning;}
unsigned board_session_lifetime(void){return A733_LIFETIME_RETURN;}
void board_service_progress(unsigned service){(void)service;abort();}

static void interrupt(void *unused)
{
    (void)unused;valid_mask=31;os_thermal_irq();
}
static void runner(void *unused)
{
    thermal.handle=xTaskGetCurrentTaskHandle();os_thermal_task(unused);
}
static void wait_until(uint64_t until)
{
    while(host_time_us()<until)
        assert(sunstone_deadline_wait_until(until)!=SUNSTONE_DEADLINE_ERROR);
}
static void test(void *unused)
{
    (void)unused;host_advance_us(phase);
    assert(cadence_control(0,host_time_us(),0)==1);
    thermal.irq.distributor=(uintptr_t)&gic_control;
    if(!strcmp(scenario,"irq"))valid_mask=0;
    assert(sunstone_deadline_register());
    host_timer_handler(sunstone_deadline_irq);
    assert(host_task_create("thermal",runner,NULL,3));
    if(!strcmp(scenario,"irq"))host_irq_at(phase+12345,interrupt,NULL);
    wait_until(phase+1000000);
    const unsigned wakes=clocks-1;
    printf("idle_wakes=%u reads=%u phase_us=%llu\n",wakes,reads,(unsigned long long)phase);
    fflush(stdout);
#ifndef THERMAL_EVENT_BASELINE
    assert(os_thermal_wake_record.errors==0);
    assert(os_thermal_wake_record.waits==wakes);
    if(!strcmp(scenario,"idle")){
        /* Forty cadence expiries, including the right endpoint. Scheduler
         * ticks and unrelated timer notifications cannot wake this owner. */
        assert(wakes==40&&reads==41);
        assert(os_thermal_wake_record.notifications==0);
        assert(os_thermal_wake_record.deadlines==40);
        for(unsigned i=0;i<reads;++i)assert(cadence_read_times[i]==phase+(uint64_t)i*25000);
    }else{
        assert(wakes==48&&reads==42);
        assert(os_thermal_wake_record.notifications==1);
        assert(os_thermal_wake_record.deadlines==47);
        assert(cadence_read_times[1]==phase+12345);
        assert(cadence_read_times[2]==phase+25000);
        assert(atomic_load_explicit(&readings.count,memory_order_acquire)==8);
    }
    /* The registered return request wakes an owner blocked on its next
     * sample. Its separate mask and stop steps retain their own deadline. */
    returning=1;
    assert(os_quiesce_step(NULL)==OS_QUIESCE_PENDING);
    wait_until(phase+1001000);
    assert(stops==1&&stop_done==1&&os_quiesce_step(NULL)==OS_QUIESCE_DONE);
    unsigned before=clocks;wait_until(phase+1025000);assert(clocks==before);
    puts("PASS thermal owner: exact cadence, interrupt wake and quiesce stop");
#else
    /* The original owner has at least one wake unrelated to these 40
     * cadence events. This is a measured control, not a timing assertion. */
    assert(wakes>40);
#endif
}
int main(int argc,char **argv)
{
    assert(argc==3);scenario=argv[1];phase=strtoull(argv[2],NULL,10);
    assert(!strcmp(scenario,"idle")||!strcmp(scenario,"irq"));
    assert(os_quiesce_register(&thermal_quiesce));
    host_rtos_run(test,NULL,2,2000000);return 0;
}
