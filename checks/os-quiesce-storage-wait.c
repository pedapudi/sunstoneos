/* Production native wait functions, with modeled IRQ, card and lease effects. */
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "a733_mmc_platform.h"
#include "a733_storage_bounds.h"
#include "device_memory_pool.h"
#include "storage_wait.h"
#include "sd_note.h"
#define OS_SD_WRITE 1
#define OS_SD_WRITE_PIECE_SECTORS 256u
#define READ_US 500000u
#define WRITE_US 2000000u
#define RECOVER_US 10000000u
#define SUNSTONE_DEADLINE_NOTIFIED 1
#define DIAGP(stage,detail) ((void)(stage),(void)(detail),(const void *)0)
#define PLAT(r) (r)
#define DIAG(stage,detail) ((void)(stage),(void)(detail),0)
#define PLATX(r) (r)
static struct {struct {atomic_uint pmic_ready;} shared;} providers;
static struct a733_mmc_platform platform;
static struct a733_mmc_platform_config config={.deadline_us=5000000,.lifetime_deadline_us=60000000};
static struct sunxi_mmc_result result;
static struct sunxi_mmc_card card;
static struct a733_storage_bounds bounds;
static uint8_t write_buffer[131072];
static uint64_t os_sd_write_pieces,os_sd_write_sectors,os_sd_mode[5],busy_total;
static uint64_t first_fault_detail, saved_where;
static unsigned first_fault_piece, first_fault_idst, saved_aux, restarts;
static uint64_t clock_us, closing;
static int requested, scenario, starts, polls, takes, recoveries, close_calls, retained, released, sleeps;
static int opened=1;
static unsigned reopened;
static uint32_t os_sd_bank_first[3], os_sd_bank_reopen[3];
static atomic_uint close_proven;
static struct device_memory_lease descriptor_lease;
static struct device_memory_pool memory;
#define a733_memory_uncached_init() (&memory)
static uint64_t now_us(void){return clock_us;}
static uint64_t os_cpu_return_deadline_us(void){return closing;}
static int os_cpu_return_requested(void){return requested;}
static int board_live(void *p){(void)p;return 1;}
static int in_region(const uint8_t *p,size_t n){(void)p;(void)n;return 1;}
static int write_fail(unsigned s,enum a733_mmc_platform_result r,uint64_t b,uint32_t n)
{(void)s;(void)r;(void)b;(void)n;return 0;}
static int recover_fail(unsigned s,enum a733_mmc_platform_result r){(void)s;(void)r;return 0;}
static void record_mode(void){}
static void irq_suspend(void){}
static int irq_resume(void){return 1;}
void a733_storage_bounds_authority(const struct a733_storage_bounds *b,struct sunxi_mmc_write_authority *a)
{(void)b;memset(a,0,sizeof *a);}
static void before_hardware(uint64_t deadline)
{assert(!closing||clock_us<closing);assert(deadline>clock_us);assert(!closing||deadline<=closing);}
enum a733_mmc_platform_result a733_mmc_platform_write_dma(struct a733_mmc_platform *p,const struct sunxi_mmc_write_authority *a,uintptr_t t,uint64_t b,uint32_t n,const uint8_t *s,uint64_t d)
{
    (void)p;(void)a;(void)t;(void)b;(void)n;(void)s;before_hardware(d);++starts;
    if(scenario==9){requested=1;closing=0;}
    return A733_MMC_PENDING;
}
enum a733_mmc_platform_result a733_mmc_platform_sd_status(struct a733_mmc_platform *p,uint8_t *b,uint64_t d)
{(void)p;(void)b;before_hardware(d);++starts;return A733_MMC_PENDING;}
enum a733_mmc_platform_result a733_mmc_platform_poll(struct a733_mmc_platform *p)
{
    (void)p;assert(!closing||clock_us<closing);++polls;
    if(scenario==11){requested=1;closing=1000;clock_us+=200;return A733_MMC_PENDING;}
    if(scenario==3){requested=1;closing=1000;return A733_MMC_ERROR;}
    if(scenario==5)return A733_MMC_COMPLETE;
    return polls>1&&scenario==0?A733_MMC_COMPLETE:A733_MMC_PENDING;
}
enum a733_mmc_platform_result a733_mmc_platform_take(struct a733_mmc_platform *p,struct sunxi_mmc_result *r,uint64_t d)
{
    (void)p;before_hardware(d);++takes;
    if(scenario==5){requested=1;closing=1000;clock_us+=200;return A733_MMC_BUSY;}
    r->error=SUNXI_MMC_OK;r->confirmed_blocks=1;return A733_MMC_READY;
}
enum a733_mmc_platform_result a733_mmc_platform_recover(struct a733_mmc_platform *p,uint64_t d)
{(void)p;before_hardware(d);++recoveries;requested=1;closing=1000;return A733_MMC_PENDING;}
enum a733_mmc_platform_result a733_mmc_platform_card(struct a733_mmc_platform *p,struct sunxi_mmc_card *c,uint64_t d)
{(void)p;(void)c;before_hardware(d);return A733_MMC_READY;}
enum a733_mmc_platform_result a733_mmc_platform_close(struct a733_mmc_platform *p,uint64_t d)
{(void)p;before_hardware(d);++close_calls;if(scenario==12){requested=1;closing=1000;}return scenario==7?A733_MMC_CLOSED:A733_MMC_PENDING;}
enum a733_mmc_platform_result a733_mmc_platform_retain(struct a733_mmc_platform *p)
{(void)p;++retained;return A733_MMC_RETAINED;}
int device_memory_release(struct device_memory_pool *p,struct device_memory_lease l)
{(void)p;(void)l;++released;return 1;}
enum sunxi_mmc_wait sunxi_mmc_wait_hint(const struct sunxi_mmc *p,uint32_t *us)
{(void)p;*us=0;return SUNXI_MMC_WAIT_IRQ;}
static int sunstone_deadline_wait_until(uint64_t until)
{
    ++sleeps;
    if((scenario==1||scenario==4||scenario==13)&&!requested){requested=1;closing=1000;clock_us=500;return SUNSTONE_DEADLINE_NOTIFIED;}
    assert(!closing||until<=closing);clock_us=until;return 0;
}
#include "storage-native-wait-model.h"
int main(int argc,char **argv)
{
    assert(argc==2);scenario=atoi(argv[1]);
    static uint8_t payload[512] __attribute__((aligned(64)));
    if(scenario==2||scenario==6||scenario==7||scenario==8){requested=1;closing=scenario==8?0:1000;}
    if(scenario==11){assert(!identify_owner());assert(polls==1&&retained==1);}
    else if(scenario==12){assert(!reentry_close());assert(close_calls==1&&retained==1&&clock_us==1000);}
    else if(scenario==13){assert(!pmic_wait());assert(!polls&&sleeps==1);}
    else if(scenario==6||scenario==7){
        descriptor_lease.pool=(uintptr_t)&memory;close_owner();
        if(scenario==6){assert(retained==1&&!released&&opened&&!close_proven&&clock_us==1000);}
        else {assert(!retained&&released==1&&!opened&&close_proven);}
    } else if(scenario==10){assert(!recover_blocking());assert(recoveries==1&&polls==0);}
    else {
        uint32_t unit=0;
        int r=scenario==4?hw_allocation_unit(&unit):hw_write_piece(1,1,payload);
        if(scenario==0)assert(r==1&&starts==1&&takes==1);
        else assert(!r);
        if(scenario==2||scenario==8)assert(!starts&&!polls);
        else assert(starts==1);
        assert(recoveries==0);
        if(scenario==1||scenario==4||scenario==5)assert(clock_us==1000);
        if(scenario==3)assert(polls==1);
        if(scenario==9)assert(polls==0);
    }
    printf("PASS native storage wait case %d: polls=%d starts=%d close=%d time=%llu us\n",scenario,polls,starts,close_calls,(unsigned long long)clock_us);
    return 0;
}
