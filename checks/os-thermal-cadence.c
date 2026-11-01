/* Execute the real owner step and report writer with modeled device/task APIs. */
#define a733_thermal_read cadence_read
#define a733_thermal_poll cadence_poll
#ifndef THERMAL_SOURCE
#define THERMAL_SOURCE "../apps/os/thermal.c"
#endif
#include THERMAL_SOURCE
#undef a733_thermal_read
#undef a733_thermal_poll

struct board_application board;
struct board_provider providers;
uint64_t cadence_state[9],cadence_read_times[256];
uint8_t cadence_report[sizeof(struct os_thermal_report)+THERMAL_RECORDS*sizeof(struct os_thermal_reading)];
static uint64_t model_time,read_delay;
static unsigned reads,clocks,failures,poll_ready,valid_mask;
static int rearm_ok;
static int32_t temperature;

uint64_t a733_smp_counter(void){return model_time*24;}
int a733_counter_clock_now(void *v,uint64_t *out){(void)v;*out=model_time;return 1;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (TaskHandle_t)(uintptr_t)1;}
void vTaskEnterCritical(void){}
void vTaskExitCritical(void){}
int board_live(void *v){(void)v;return 1;}
int os_application_deadline(void){return 1;}
unsigned os_application_activity_generation(void){return 7;}
int os_cpu_clock_step(const struct a733_thermal_policy *p){(void)p;++clocks;return 1;}
void board_fail_at(uint32_t site,uint32_t value,const void *caller)
{(void)site;(void)value;(void)caller;++failures;}
int gicv3_spi_mask(const struct gicv3_spi *irq){(void)irq;return 1;}
int gicv3_spi_rearm(const struct gicv3_spi *irq){(void)irq;return rearm_ok;}
enum a733_clock_result a733_clock_retain(struct a733_clock_leases *c,const struct a733_clock_token *t)
{(void)c;(void)t;return A733_CLOCK_OK;}
enum a733_thermal_result cadence_poll(struct a733_thermal *d)
{(void)d;return poll_ready?A733_THERMAL_READY:A733_THERMAL_PENDING;}
enum a733_thermal_result cadence_read(struct a733_thermal *d,uint64_t deadline,struct a733_thermal_sample *s)
{
    (void)d;(void)deadline;
    if(reads<256)cadence_read_times[reads]=model_time;
    ++reads;model_time+=read_delay;
    if(!valid_mask)return A733_THERMAL_AGAIN;
    memset(s,0,sizeof(*s));s->valid=(uint8_t)valid_mask;s->sequence=thermal.policy.sequence+1;s->observed_us=model_time;
    for(unsigned i=0;i<5;++i)s->millicelsius[i]=temperature;
    return A733_THERMAL_SAMPLE;
}

/* Operations: reset, step, begin/end window, delay, temperature, retaining,
 * rearm refusal, inactive startup. All production branches execute normally. */
int cadence_control(unsigned op,uint64_t at,uint64_t value)
{
    int result=1;model_time=at;
    if(op==0){
        memset(&thermal,0,sizeof(thermal));memset(&readings,0,sizeof(readings));
        memset(&window,0,sizeof(window));memset(&providers,0,sizeof(providers));
        memset(&board,0,sizeof(board));memset(cadence_state,0,sizeof(cadence_state));
        memset(cadence_report,0,sizeof(cadence_report));
        reads=clocks=failures=0;read_delay=0;poll_ready=0;rearm_ok=1;temperature=40000;valid_mask=31;
        thermal.handle=(TaskHandle_t)(uintptr_t)1;thermal.active=1;thermal.masked=1;
        thermal.driver.initialized=1;board.session.deadline_us=at+60000000;
        board.session.epoch=11;board.session.generation=12;
        a733_thermal_policy_init(&thermal.policy,at,policy,&thermal);
        a733_thermal_policy_cpu_limit(&thermal.policy,70000);
        atomic_store_explicit(&load_state,A733_THERMAL_THROTTLE,memory_order_relaxed);
    }else if(op==1)result=step();
    else if(op==2)os_thermal_record_begin();
    else if(op==3){
        os_thermal_record_end((int)value);
        uint32_t total=window.header.bytes;
        for(uint32_t off=0;off<total;){
            size_t copied=0,n=total-off;if(n>1024)n=1024;uint32_t extent=0;
            if(os_thermal_report_copy(7,off,n,cadence_report+off,&copied,&extent)!=1||
               copied!=n||extent!=total)return -2;
            off+=(uint32_t)copied;
        }
        cadence_state[6]=total;
    }else if(op==4)read_delay=value;
    else if(op==5)temperature=(int32_t)value;
    else if(op==6)thermal.retaining=(unsigned)value;
    else if(op==7){rearm_ok=-1;thermal.masked=1;}
    else if(op==8){thermal.active=0;poll_ready=0;}
    else if(op==9)valid_mask=(unsigned)value;
    else if(op==10)thermal.driver.fault=(unsigned)value;
    cadence_state[0]=atomic_load_explicit(&readings.count,memory_order_acquire);
    cadence_state[1]=readings.next;cadence_state[2]=reads;cadence_state[3]=clocks;
    cadence_state[4]=failures;cadence_state[5]=thermal.active;
    cadence_state[7]=thermal.policy.sequence;
    cadence_state[8]=(uint64_t)os_thermal_throttled();
    return result;
}
