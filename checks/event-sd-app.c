/* Production region application control flow and result layout; memory and
 * device status are modeled. This does not measure native throughput. */
#include "large_region-app.h"
#include "sunstone_storage.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef void *TaskHandle_t;
typedef uint64_t StackType_t,UBaseType_t;
typedef uint64_t StaticTask_t;
#define pdTRUE 1
#define portMAX_DELAY UINT64_MAX
#define CONTROL_PASSES 10u
#define CONTROL_BYTES (UINT64_C(40)<<20)
#define FULL_PASSES 3u
#define SWITCH_ROUNDS 20000u
#define TAG_DONE UINT64_C(0x4c52544f4e45)
#define TAG_NOT_READY UINT64_C(0x4c52544e4f54)
static unsigned scenario,stats_calls,waits,rounds,reads,terminal;
static uint64_t clock_us;
static int stats_ready;
static jmp_buf ending;
static const char *destination;
static StaticTask_t peer_state;
static StackType_t peer_stack[512];
static TaskHandle_t main_handle,peer_handle;
static void peer(void *p){(void)p;assert(0);}
static TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void *)1;}
static TaskHandle_t xTaskCreateStaticAffinitySet(void (*f)(void *),const char *name,unsigned words,void *arg,unsigned priority,StackType_t *stack,StaticTask_t *state,UBaseType_t affinity)
{assert(f==peer&&name&&words==512&&!arg&&priority==3&&stack==peer_stack&&state==&peer_state&&affinity==64);return (void *)2;}
static void vTaskSetApplicationTaskTag(TaskHandle_t task,void *tag){assert(task==peer_handle&&!tag);}
static void vTaskDelay(uint64_t ticks){(void)ticks;assert(0);}
static uint64_t round_trips(void){++rounds;clock_us+=10;return 240;}
static uint64_t bw_read(const uint64_t *base,size_t bytes)
{assert(base==(const void *)0x80000000&&(bytes==CONTROL_BYTES||bytes==(UINT64_C(1)<<30)));++reads;clock_us+=10;return 0;}
int ss_clock_ready(void){return 1;}
void ss_progress(void){}
uint64_t ss_counter_ticks(void){return clock_us*24;}
void ss_large_attach(void *task){assert(task==peer_handle);}
int ss_live(void){return scenario==8&&waits?-1:1;}
int ss_large_wait(unsigned index,uint64_t timeout)
{assert(index==LARGE_REGION_LARGE_WEIGHTS&&timeout==1000000);return scenario==4?-1:1;}
int ss_large_status(unsigned index,struct ss_large_status *out)
{assert(index==LARGE_REGION_LARGE_WEIGHTS);*out=(struct ss_large_status){.state=SS_LARGE_READY,.loaded=536870912,.total=536870912,.elapsed_us=24386000,.copy_us=154000,.digest_us=979000};return 0;}
void *ss_large_base(unsigned index,size_t *bytes)
{assert(index==LARGE_REGION_LARGE_WEIGHTS);*bytes=UINT64_C(1)<<30;return (void *)0x80000000;}
void ss_usb_wait(uint32_t us)
{
    ++waits;
    if(!stats_ready&&scenario==1){clock_us+=250;stats_ready=1;return;}
    if(!stats_ready&&scenario==7){clock_us=39500000;stats_ready=1;return;}
    if(scenario==8){clock_us+=us/2;return;}
    clock_us+=us;
}
int ss_storage_owner_stats(struct ss_storage_owner_stats *out)
{
    ++stats_calls;
    if(!stats_ready)return 0;
    *out=(struct ss_storage_owner_stats){.elapsed_us=clock_us+24386000,.asleep_us=clock_us+23000000,
        .wakes=4200,.irq_cores=1,.owner_cores=1,.caller_cores=1,.irq_entries={4098},.irq_recheck=scenario==3};
    if(scenario==2&&stats_calls>1)++out->wakes;
    return 1;
}
_Noreturn void ss_fail(uint32_t code){terminal=code;longjmp(ending,1);}
_Noreturn void ss_finish(const void *payload,size_t size)
{
    assert(size==512);
    FILE *file=fopen(destination,"wb");assert(file);assert(fwrite(payload,1,size,file)==size);assert(!fclose(file));
    terminal=1;longjmp(ending,1);
}
#include "event-sd-app-production.h"
int main(int argc,char **argv)
{
    assert(argc==3);scenario=(unsigned)atoi(argv[1]);destination=argv[2];stats_ready=scenario!=1&&scenario!=5&&scenario!=7;
    if(scenario==6)clock_us=39500000;
    struct ss_grant grant={.main_core=6,.finish_by_us=40000000};
    if(!setjmp(ending))large_region_main(&grant);
    if(scenario==5)assert(terminal==0x1e02&&clock_us==grant.finish_by_us);
    else if(scenario>=6)assert(terminal==0x1e03);
    else assert(terminal==1);
    if(scenario!=4)assert(rounds==3&&reads==13);
    if(scenario<4)assert(clock_us>=1000000&&stats_calls>=2&&waits>=1);
    puts("PASS region result and precise post-retirement observation");
}
