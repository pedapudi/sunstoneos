/* Execute the real owner step and report writer (apps/os/thermal.c) on the real FreeRTOS
 * kernel under the host shim (checks/host/). The owner is the test task, its critical sections
 * are the kernel's, and the counter is the shim's A733 counter set by each command. The
 * sensor device, the GIC, the CPU clock step and the board lifetime are modeled at their
 * boundary. checks/os-thermal-cadence.py drives it over a pipe: one command per line. */
#include <stdio.h>
#include <stdlib.h>
#include "host_rtos.h"
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
uint64_t cadence_state[12],cadence_read_times[256];
uint8_t cadence_report[sizeof(struct os_thermal_report)+THERMAL_RECORDS*sizeof(struct os_thermal_reading)];
static uint64_t read_delay;
#define model_time host_time_us()
static unsigned reads,clocks,failures,poll_ready,valid_mask;
static int rearm_ok;
static int32_t temperature;

int a733_counter_clock_now(void *v,uint64_t *out){(void)v;*out=model_time;return 1;}
int board_live(void *v){(void)v;return 1;}
uint32_t board_live_refusal(void){return 0;}
int os_application_deadline(void){return 1;}
unsigned os_application_activity_generation(void){return 7;}
static unsigned clock_busy;
int os_cpu_clock_step(const struct a733_thermal_policy *p){(void)p;++clocks;return 1;}
int os_cpu_clock_busy(void){return (int)clock_busy;}
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
    ++reads;host_advance_us(read_delay);
    if(!valid_mask)return A733_THERMAL_AGAIN;
    memset(s,0,sizeof(*s));s->valid=(uint8_t)valid_mask;s->sequence=thermal.policy.sequence+1;s->observed_us=model_time;
    for(unsigned i=0;i<5;++i)s->millicelsius[i]=temperature;
    return A733_THERMAL_SAMPLE;
}

/* Device paths that the owner step never takes. The linker keeps these references because the
 * sanitizer's global registration references the owner's operation tables; a call
 * fails the check. */
#define NEVER(name) static void never_##name(void){fprintf(stderr,"FAIL %s called\n",#name);abort();}
NEVER(translate)NEVER(token_status)NEVER(lock)NEVER(native)NEVER(cpu_admit)NEVER(session)
int aarch64_device_translate(uintptr_t a,int w,struct aarch64_translation *t){(void)a;(void)w;(void)t;never_translate();return 0;}
enum a733_clock_result a733_clock_token_status(struct a733_clock_leases *c,const struct a733_clock_token *t)
{(void)c;(void)t;never_token_status();return A733_CLOCK_OK;}
int a733_platform_lock_enter(struct a733_platform_locks *l,enum a733_platform_lock k,uint64_t d){(void)l;(void)k;(void)d;never_lock();return 0;}
void a733_platform_lock_leave(struct a733_platform_locks *l,enum a733_platform_lock k){(void)l;(void)k;never_lock();}
int a733_platform_lock_retain(struct a733_platform_locks *l,enum a733_platform_lock k){(void)l;(void)k;never_lock();return 0;}
int a733_thermal_native_read(void *c,uintptr_t a,uint32_t *v){(void)c;(void)a;(void)v;never_native();return 0;}
int a733_thermal_native_write(void *c,uintptr_t a,uint32_t v){(void)c;(void)a;(void)v;never_native();return 0;}
int os_cpu_admit(void){never_cpu_admit();return 0;}
const struct os_session *os_session_admitted(void){never_session();return NULL;}

/* Operations: reset, step, begin/end window, delay, temperature, retaining,
 * rearm refusal, inactive startup. All production branches execute normally. */
int cadence_control(unsigned op,uint64_t at,uint64_t value)
{
    int result=1;host_reset_time_us(at);
    if(op==0){
        memset(&thermal,0,sizeof(thermal));memset(&readings,0,sizeof(readings));
        memset(&window,0,sizeof(window));memset(&providers,0,sizeof(providers));
        memset(&board,0,sizeof(board));memset(cadence_state,0,sizeof(cadence_state));
        memset(cadence_report,0,sizeof(cadence_report));
        thermal_record_origin_us=0;
        clock_busy=0;reads=clocks=failures=0;read_delay=0;poll_ready=0;rearm_ok=1;temperature=40000;valid_mask=31;
        thermal.handle=xTaskGetCurrentTaskHandle();thermal.active=1;thermal.masked=1;
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
    else if(op==11)atomic_store_explicit(&thermal.pending,1,memory_order_release);
    else if(op==12)result=(int)idle_ticks();
    else if(op==13)clock_busy=(unsigned)value;
    cadence_state[0]=atomic_load_explicit(&readings.count,memory_order_acquire);
    cadence_state[1]=readings.next;cadence_state[2]=reads;cadence_state[3]=clocks;
    cadence_state[4]=failures;cadence_state[5]=thermal.active;
    cadence_state[7]=thermal.policy.sequence;
    cadence_state[8]=(uint64_t)os_thermal_throttled();
    cadence_state[9]=thermal.next_read;cadence_state[10]=thermal_record_origin_us;
    cadence_state[11]=model_time;
    return result;
}

/* Commands: "call OP AT VALUE", "report", "times". Each reply is one line. */
static void commands(void *unused)
{
    (void)unused;
    char line[256];
    while(fgets(line,sizeof line,stdin)){
        unsigned op;unsigned long long at,value;
        if(sscanf(line,"call %u %llu %llu",&op,&at,&value)==3){
            int r=cadence_control(op,at,value);
            printf("%u",(unsigned)r);
            for(unsigned i=0;i<12;++i)printf(" %llu",(unsigned long long)cadence_state[i]);
            puts("");
        }else if(!strncmp(line,"report",6)){
            for(uint64_t i=0;i<cadence_state[6];++i)printf("%02x",cadence_report[i]);
            puts("");
        }else if(!strncmp(line,"times",5)){
            for(uint64_t i=0;i<cadence_state[2]&&i<256;++i)printf("%llu ",(unsigned long long)cadence_read_times[i]);
            puts("");
        }else abort();
        fflush(stdout);
    }
}
int main(void){host_rtos_run(commands,NULL,2,1000000);return 0;}
