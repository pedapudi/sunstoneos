/* Read-only microSD reads on the returning platform, with the result returned
 * through the boot ROM.
 *
 * The image runs the production SD0 provider (drivers/storage) with the
 * production clock, pin, rail and PMIC providers as two FreeRTOS tasks on the
 * returning platform of rank-9-usb-v4. It identifies the card in SD profile 2
 * (high speed, 3.3 V) or 3 (UHS-I SDR104, 1.8 V), runs the input's read plan,
 * prints one line per read with its time and CRC-32 into the result page, parks
 * the PMIC service and exits; the monitor then returns to the ROM, where the
 * host reads the page over FEL. It never touches USB0. A stop prints its reason
 * and exits with status 1, which also returns to the ROM; a hang ends at the
 * monitor's secure timer, 2 s after entry, with status 2.
 *
 * Every deadline below is measured from the start of kernel_main() and fits
 * inside that 2 s window, which the monitor fixes. The card, controller, pin,
 * clock and rail ownership stays as it is at the return: the forced-FEL path
 * does not touch them, and the lifetime ends with whole-board power removal. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "platform.h"
#include "boot.h"
#include "a733_range.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "a733_clock_leases.h"
#include "a733_pins_native.h"
#include "a733_gpu_resources_native.h"
#include "a733_rail_leases.h"
#include "a733_pmic_native.h"
#include "a733_mmc_platform_native.h"
#include "a733_mapping_check.h"
#include "aarch64_device_memory.h"
#include "sunxi_smhc.h"
#include "sdrt.h"
#include <stdatomic.h>
#include <string.h>

/* Deadlines in microseconds after the start of kernel_main(). */
#define SDRT_IDENTIFY_US 1150000u  /* card identification and mode selection */
#define SDRT_PLAN_END_US 1550000u  /* no read starts at or after this time */
#define SDRT_READ_US 100000u       /* one read, bounded by the lifetime below */
#define SDRT_LIFETIME_US 1650000u  /* every SD0 provider operation */
#define SDRT_END_US 1800000u       /* PMIC parking and the report */
#define SDRT_PMIC_OWNER 2u

struct sdrt sdrt;
/* The functions checks/storage-read-returning.py calls stay out of line and
 * linked, so the check runs the same code as the tasks. */
#define SDRT_ENTRY __attribute__((used,retain,noinline))
/* IDMAC writes each read here: 128 KiB, write-back, cleaned before and
 * invalidated after every transfer. The descriptors live in the result
 * block, which the EL1 map makes non-cacheable (platform/a733/platform.c),
 * 1 MiB above the result page. */
static uint8_t buffer[SUNXI_MMC_MAX_DMA_BLOCKS*512u] __attribute__((aligned(4096)));
#define SDRT_DESCRIPTORS ((volatile struct sunxi_mmc_dma_descriptor *)(uintptr_t)UINT64_C(0x42100000))
static StaticTask_t tasks[2];
static StackType_t stacks[2][4096];

__attribute__((noinline)) uint64_t sdrt_ticks(void){return counter();}
static uint64_t now_us(void)
{
    uint64_t v;
    return a733_counter_clock_now(&sdrt.counter,&v)==1?v:UINT64_MAX;
}
static int live(void *unused)
{(void)unused;return !atomic_load_explicit(&sdrt.failed,memory_order_acquire)&&now_us()<sdrt.end;}
static void fail(void *unused){(void)unused;atomic_store_explicit(&sdrt.failed,1,memory_order_release);}
static int enter(void *unused,uint64_t deadline)
{(void)unused;return live(NULL)&&a733_platform_lock_enter(&sdrt.locks,A733_PLATFORM_REGISTER_LOCK,deadline);}
static void leave(void *unused){(void)unused;a733_platform_lock_leave(&sdrt.locks,A733_PLATFORM_REGISTER_LOCK);}
static int clock_now(void *unused,uint64_t *v){(void)unused;return a733_counter_clock_now(&sdrt.counter,v);}

/* Every page of [a, a + bytes) translates to itself for write with exactly
 * this attribute and shareability, as apps/storage-read/native.c requires. */
static int memory(uintptr_t a,size_t bytes,uint8_t attributes,uint8_t shareability)
{
    if(!bytes||bytes>UINTPTR_MAX-a||shareability>3)return 0;
    const struct a733_mapping_rule rule={A733_MAPPING_WRITE,1,{{attributes,(uint8_t)(1u<<shareability)}},0,1};
    return a733_mapping_check_pages(aarch64_device_translate,a,a,bytes,&rule);
}
static int mappings(void)
{
    static const uintptr_t words[]={0x02002d00,0x02002d0c,0x02002500,0x02002580,0x02002588,
        0x020000f0,0x02000344,0x03400118,0x03400218,0x03400318};
    if(!memory(0x04020000,0x304,0,2))return 0;
    for(unsigned i=0;i<sizeof(words)/sizeof(words[0]);++i)if(!memory(words[i],1,0,2))return 0;
    return memory((uintptr_t)buffer,sizeof(buffer),0xff,3)&&
        memory((uintptr_t)SDRT_DESCRIPTORS,SUNXI_MMC_DMA_DESCRIPTORS*sizeof(*SDRT_DESCRIPTORS),0x44,2);
}
static int admit(void *cookie,const struct a733_mmc_platform_config *c,const struct a733_clock_token *t)
{
    return cookie==&sdrt&&live(NULL)&&mappings()&&!memcmp(c,&sdrt.config,sizeof(*c))&&
        t->owner==sdrt.config.owner&&a733_clock_token_status(&sdrt.clocks,t)==A733_CLOCK_OK;
}

/* CRC-32 (IEEE, reflected), with the ARMv8 CRC32 instructions, which use
 * the same polynomial; n is a multiple of 512. */
static uint32_t crc32(const uint8_t *p,size_t n)
{
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;i+=8){
        uint64_t v;memcpy(&v,p+i,8);
        __asm__(".arch_extension crc\n crc32x %w0, %w0, %x1" : "+r"(c) : "r"(v));
    }
    return ~c;
}
/* The core's dma_sync: clean before the transfer, invalidate after it. */
static int dma_sync(void *cookie,uint8_t *b,size_t n,unsigned phase)
{
    (void)cookie;
    uintptr_t start=(uintptr_t)b,base=(uintptr_t)buffer;
    return (phase==SUNXI_MMC_DMA_BEFORE||phase==SUNXI_MMC_DMA_AFTER)&&
        start>=base&&start<=base+sizeof(buffer)&&n<=base+sizeof(buffer)-start&&
        aarch64_device_cache(phase==SUNXI_MMC_DMA_BEFORE?AARCH64_CACHE_CLEAN:AARCH64_CACHE_INVALIDATE,(uintptr_t)b,n)==1;
}
static char *hex(char *p,uint64_t v,unsigned digits)
{
    for(unsigned i=0;i<digits;++i)p[i]="0123456789abcdef"[(v>>(4*(digits-1-i)))&15];
    return p+digits;
}
static void line(const char *label,uint64_t value)
{platform_puts(label);platform_hex(value);platform_puts("\n");}

/* Copy and check the input, then initialize every provider without hardware
 * access. Returns 1 on success. */
SDRT_ENTRY int sdrt_setup(void)
{
    const volatile struct sdrt_input *in=(const volatile struct sdrt_input *)SDRT_INPUT_ADDRESS;
    for(size_t i=0;i<sizeof(sdrt.input);++i)((uint8_t *)&sdrt.input)[i]=((const volatile uint8_t *)in)[i];
    const struct sdrt_input *c=&sdrt.input;
    if(c->magic!=SDRT_MAGIC||c->version!=SDRT_VERSION||c->bytes!=sizeof(*c)||!c->generation||
       !c->owner||!c->handoff||!c->topology||c->owner==SDRT_PMIC_OWNER||
       (c->profile!=SUNXI_MMC_SD_HIGH_SPEED4&&c->profile!=SUNXI_MMC_SD_UHS_SDR104)||
       !c->reads||c->reads>SDRT_PLAN)return 0;
    for(unsigned i=0;i<c->reads;++i)
        if(!c->plan[i].blocks||c->plan[i].blocks>sizeof(buffer)/512)return 0;
    atomic_init(&sdrt.failed,0);atomic_init(&sdrt.pmic_stop,0);atomic_init(&sdrt.pmic_parked,0);
    if(!a733_counter_clock_init(&sdrt.counter,24000000)||
       !a733_platform_locks_init(&sdrt.locks,a733_counter_clock_now,&sdrt.counter)||
       !a733_clock_leases_init(&sdrt.clocks,a733_counter_clock_now,&sdrt.counter))return 0;
    sdrt.start=now_us();
    if(sdrt.start==UINT64_MAX||!a733_deadline_after(sdrt.start,SDRT_END_US,&sdrt.end))return 0;
    sdrt.handoff.authorized[A733_PIN_L]=3;     /* PL0/PL1: the PMIC's S_TWI0. */
    sdrt.handoff.authorized[A733_PIN_F]=0x7f;  /* PF0..PF6: SD0 and its card detect. */
    const struct a733_pins_locks pl={enter,leave,clock_now,NULL};
    if(!a733_pins_native_init(&sdrt.pin_native,&pl)||
       !a733_pins_init_banks(&sdrt.pins,&sdrt.pin_native.operations,&sdrt.pin_native,&sdrt.handoff)||
       !a733_gpu_resources_native_init(&sdrt.gpu_native,&sdrt.locks.callbacks))return 0;
    sdrt.gpu_native.operations.now=clock_now;
    if(!a733_gpu_resources_init(&sdrt.gpu,&sdrt.gpu_native.operations,&sdrt.gpu_native))return 0;
    const struct a733_rail_binding rb={&sdrt.clocks,&sdrt.gpu,c->generation,a733_counter_clock_now,&sdrt.counter};
    if(!a733_rail_leases_init(&sdrt.rails,&rb))return 0;
    sdrt.pmic_config=(struct a733_pmic_config){.owner=SDRT_PMIC_OWNER,.identity=c->generation,
        .startup_deadline=sdrt.end,.pins={c->pmic_pins[0],c->pmic_pins[1]}};
    sdrt.config=(struct a733_mmc_platform_config){.owner=c->owner,.handoff=c->handoff,.topology=c->topology,
        .deadline_us=sdrt.start+SDRT_IDENTIFY_US,.lifetime_deadline_us=sdrt.start+SDRT_LIFETIME_US,
        .kind=SUNXI_MMC_SD,.voltage_mask=c->voltage_mask,.sd_profile=(enum sunxi_mmc_sd_profile)c->profile,
        .dma=SDRT_DESCRIPTORS};
    memcpy(sdrt.config.fabric,c->fabric,sizeof(c->fabric));
    memcpy(sdrt.config.fabric_hz,c->fabric_hz,sizeof(c->fabric_hz));
    memcpy(sdrt.config.expected,c->expected,sizeof(c->expected));
    memcpy(sdrt.config.desired,c->desired,sizeof(c->desired));
    a733_clock_mask_t scopes;
    return a733_mmc_platform_scopes(&sdrt.config,&scopes);
}
/* The PMIC adapter and provider, bound to the PMIC task. */
SDRT_ENTRY int sdrt_bind(TaskHandle_t pmic)
{
    return pmic&&a733_pmic_native_init(&sdrt.pmic_native,&sdrt.locks,pmic)&&
        a733_pmic_native_bind(&sdrt.pmic,&sdrt.pmic_config,&sdrt.pmic_native,&sdrt.clocks,&sdrt.rails,&sdrt.pins)&&
        a733_gpu_resources_native_use_pmic(&sdrt.gpu_native,&sdrt.gpu,&sdrt.pmic);
}

static int stop(unsigned reason){sdrt.reason=reason;fail(NULL);return -1;}
/* One step of the SD task: 2 progressed, continue at once; 0 waiting, continue
 * after one scheduler tick; 1 plan done or out of window; -1 stopped. A read's
 * submission, completion and take run back to back, so the plan's 256 reads
 * do not each wait for scheduler ticks. */
SDRT_ENTRY int sdrt_step(void)
{
    enum a733_mmc_platform_result r;
    if(!live(NULL))return stop(1);
    switch(sdrt.stage){
    case SDRT_CONSTRUCT:
        if(!a733_mmc_platform_native_init_selected(&sdrt.native,&sdrt.locks,xTaskGetCurrentTaskHandle(),
               SUNXI_MMC_SD,A733_MMC_EMMC_SDC2,admit,&sdrt))return stop(2);
        sdrt.native.operations.dma_sync=dma_sync;
        if(!a733_mmc_platform_init(&sdrt.platform,&sdrt.config,&sdrt.native.operations,&sdrt.native,
               &sdrt.clocks,&sdrt.pins,&sdrt.rails,&sdrt.pmic))return stop(2);
        sdrt.stage=SDRT_IDENTIFY;return 2;
    case SDRT_IDENTIFY: {
        /* Identification, mode selection and, for profile 3, the 1280 CMD19
         * tuning commands advance one controller step per poll. One poll per
         * scheduler tick would not fit them in the window, so poll for up to
         * 1 ms per wake; the tick between wakes runs the PMIC task, whose
         * supply snapshots identification waits for. */
        uint64_t wake=sdrt_ticks();
        for(;;){
            r=a733_mmc_platform_poll(&sdrt.platform);
            if(r==A733_MMC_READY){sdrt.stage=SDRT_CARD;return 2;}
            if(r!=A733_MMC_PENDING&&r!=A733_MMC_BUSY)return stop(3);
            if((sdrt_ticks()-wake)/24>=1000||!live(NULL))return 0;
        }
    }
    case SDRT_CARD:
        r=a733_mmc_platform_card(&sdrt.platform,&sdrt.card,sdrt.config.deadline_us);
        if(r==A733_MMC_BUSY)return 0;
        if(r!=A733_MMC_READY)return stop(4);
        sdrt.card_valid=1;sdrt.identified_us=now_us()-sdrt.start;sdrt.stage=SDRT_READ;return 2;
    case SDRT_READ: {
        if(sdrt.done==sdrt.input.reads)return 1;
        uint64_t now=now_us();
        if(now>=sdrt.start+SDRT_PLAN_END_US)return 1;
        const struct sdrt_read *q=&sdrt.input.plan[sdrt.done];
        sdrt.started=sdrt_ticks();
        r=a733_mmc_platform_read(&sdrt.platform,q->first,q->blocks,buffer,q->blocks*512u,
            now+SDRT_READ_US<sdrt.config.lifetime_deadline_us?now+SDRT_READ_US:sdrt.config.lifetime_deadline_us);
        if(r==A733_MMC_BUSY)return 0;
        if(r!=A733_MMC_PENDING)return stop(5);
        sdrt.stage=SDRT_POLL;return 2;
    }
    case SDRT_POLL:
        /* Poll the read to completion with no scheduler wait: the time from
         * submission measures the driver and the card. The provider's own
         * deadline ends a read that never completes. */
        for(;;){
            r=a733_mmc_platform_poll(&sdrt.platform);
            if(r==A733_MMC_COMPLETE){
                uint64_t us=(sdrt_ticks()-sdrt.started)/24;
                sdrt.elapsed[sdrt.done]=us>UINT32_MAX?UINT32_MAX:(uint32_t)us;sdrt.stage=SDRT_TAKE;return 2;
            }
            if(r!=A733_MMC_PENDING&&r!=A733_MMC_BUSY)return stop(6);
        }
    case SDRT_TAKE: {
        const struct sdrt_read *q=&sdrt.input.plan[sdrt.done];
        r=a733_mmc_platform_take(&sdrt.platform,&sdrt.result,sdrt.config.lifetime_deadline_us);
        if(r==A733_MMC_BUSY)return 0;
        if(r!=A733_MMC_READY||sdrt.result.error!=SUNXI_MMC_OK||sdrt.result.received!=q->blocks*512u)return stop(7);
        sdrt.crc[sdrt.done]=crc32(buffer,q->blocks*512u);
        const struct sunxi_mmc_result *t=&sdrt.result;
        sdrt.timing[sdrt.done]=(struct sdrt_timing){t->command_us,t->first_byte_us,t->done_us,t->clean_us,
            t->invalidate_us,t->polls};
        sdrt.done++;sdrt.stage=SDRT_READ;return 2;
    }
    default:return stop(8);
    }
}

/* The PMIC service is parked and idle, no rail is borrowed and no lock is
 * held: no provider transaction is in flight when the CPU resets. */
SDRT_ENTRY int sdrt_quiet(void)
{
    const struct a733_pmic *p=&sdrt.pmic;
    if(!atomic_load_explicit(&sdrt.pmic_parked,memory_order_acquire)||p->held||p->retained||p->failed||
       p->active!=A733_PMIC_REQUESTS||atomic_load_explicit(&sdrt.locks.retained,memory_order_acquire))return 0;
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)if(p->slots[i].state||p->slots[i].borrow.id)return 0;
    return !xSemaphoreGetMutexHolder(sdrt.locks.mutex[0])&&!xSemaphoreGetMutexHolder(sdrt.locks.mutex[1]);
}

/* The last step before the image's own exit after the SD task: stop the
 * card clock, so that a card left in the middle of a read stops clocking data
 * onto DAT1 and DAT2, which the ROM's return makes the JTAG TMS and TCK
 * inputs. The order is the vendor driver's: the clock update of
 * sunxi-mmc-v5p3x.c lines 364-400, and the FIFO and DMA interface resets of
 * sunxi-mmc.c lines 669-680 (sunxi_mmc_reset_fifo) and 936-951:
 *   1. if a transfer is outstanding (a read submitted and not taken, a
 *      command still started, or a busy data or command state machine),
 *      reset the FIFO and the DMA interface (GCTRL bits 1 and 2) and wait
 *      for both bits to clear;
 *   2. clear the card clock enable and low-power bits and set MASK_DATA0
 *      (CLKCR bits 16, 17 and 31), so the update does not wait on a DAT0
 *      that the card holds low;
 *   3. issue the clock-update command (START, UPCLK_ONLY, WAIT_PRE_OVER) and
 *      wait for START to clear; write back RINT's set bits but SDIO;
 *   4. clear MASK_DATA0.
 * If the reset or the update does not complete within 10 ms, gate the
 * controller's module clock instead (CCU SMHC0 word bit 31), which stops the
 * card clock at its source. The writes go to the controller directly: after a
 * fault the provider keeps its register lock retained, and the PMIC task,
 * whether or not it has parked, accesses only S_TWI0.
 *
 * Returns 1 when the update completed and CLKCR reads back with the card
 * clock off, 3 when the update timed out and the module clock reads back
 * gated, 2 when the controller's bus gate, reset or module clock was never
 * enabled (no card clock, no write), and 0 when neither stop read back. The
 * monitor's own 2 s return runs no image code, so this step does not cover
 * that exit. */
static int wait_clear(uintptr_t address,uint32_t bits)
{
    uint64_t end=sdrt_ticks()+10*24000;
    while(mmio_read(address)&bits)if(sdrt_ticks()>=end)return 0;
    return 1;
}
static int wait_idle(uintptr_t address)
{
    uint64_t end=sdrt_ticks()+10*24000;
    while(SUNXI_SMHC_IDST_STATE(mmio_read(address)))if(sdrt_ticks()>=end)return 0;
    return 1;
}
SDRT_ENTRY int sdrt_clock_off(void)
{
    const uintptr_t base=A733_MMC_BASE(0),module=A733_MMC_MODULE(0),bus=A733_MMC_BUS(0);
    const uint32_t card_on=UINT32_C(1)<<16,low_power=UINT32_C(1)<<17,mask_data0=UINT32_C(1)<<31;
    const uint32_t resets=SUNXI_SMHC_GCTRL_FIFO_RESET|SUNXI_SMHC_GCTRL_DMA_RESET;
    if((mmio_read(bus)&0x10001u)!=0x10001u||!(mmio_read(module)&(UINT32_C(1)<<31)))return 2;
    int dma=(mmio_read(base+SUNXI_SMHC_DMAC)&0x80u)||(mmio_read(base+SUNXI_SMHC_GCTRL)&SUNXI_SMHC_GCTRL_DMA_ENABLE);
    int stalled=dma||sdrt.stage==SDRT_POLL||sdrt.stage==SDRT_TAKE||
        (mmio_read(base+SUNXI_SMHC_CMD)&SUNXI_SMHC_CMD_START)||(mmio_read(base+SUNXI_SMHC_STATUS)&SUNXI_SMHC_STATUS_BUSY);
    int ok=1;
    if(dma){
        /* The board kernel's IDMAC teardown (sunxi-mmc.c:1171-1180), then
         * the IDMAC reset and its state machine idle. */
        mmio_write(base+SUNXI_SMHC_IDST,SUNXI_SMHC_IDST_CLEAR);
        mmio_write(base+SUNXI_SMHC_IDIE,0);
        mmio_write(base+SUNXI_SMHC_DMAC,SUNXI_SMHC_DMAC_RESET);
        ok=wait_clear(base+SUNXI_SMHC_DMAC,SUNXI_SMHC_DMAC_RESET)&&wait_idle(base+SUNXI_SMHC_IDST);
        mmio_write(base+SUNXI_SMHC_GCTRL,(mmio_read(base+SUNXI_SMHC_GCTRL)|resets)&~SUNXI_SMHC_GCTRL_DMA_ENABLE);
    } else if(stalled)mmio_write(base+SUNXI_SMHC_GCTRL,mmio_read(base+SUNXI_SMHC_GCTRL)|resets);
    if(stalled)ok=ok&&wait_clear(base+SUNXI_SMHC_GCTRL,resets);
    if(ok){
        /* During the voltage switch the board kernel's clock updates carry
         * the voltage-switch bit (sunxi-mmc-v5p3x.c:378-379). */
        uint32_t update=SUNXI_SMHC_UPDATE|(sdrt.platform.core.signal_window?SUNXI_SMHC_CMD_VOLTAGE_SWITCH:0u);
        mmio_write(base+SUNXI_SMHC_CLKCR,(mmio_read(base+SUNXI_SMHC_CLKCR)&~(card_on|low_power))|mask_data0);
        __asm__ volatile("dsb sy":::"memory");
        mmio_write(base+SUNXI_SMHC_CMD,update);
        ok=wait_clear(base+SUNXI_SMHC_CMD,SUNXI_SMHC_CMD_START);
        mmio_write(base+SUNXI_SMHC_RINT,mmio_read(base+SUNXI_SMHC_RINT)&~SUNXI_SMHC_INT_SDIO);
    }
    if(ok){
        mmio_write(base+SUNXI_SMHC_CLKCR,mmio_read(base+SUNXI_SMHC_CLKCR)&~mask_data0);
        return !(mmio_read(base+SUNXI_SMHC_CLKCR)&card_on);
    }
    /* Gate the module clock and assert the bus reset: the module gate stops
     * the card clock, and the reset stops IDMAC, which runs on the bus clock. */
    mmio_write(module,mmio_read(module)&~(UINT32_C(1)<<31));
    mmio_write(bus,mmio_read(bus)&~(UINT32_C(1)<<16));
    __asm__ volatile("dsb sy":::"memory");
    return (mmio_read(module)&(UINT32_C(1)<<31))||(mmio_read(bus)&(UINT32_C(1)<<16))?0:3;
}

/* The result lines; host tool tools/storage-read-returning.py parses them. */
SDRT_ENTRY void sdrt_report(int outcome)
{
    char text[160],*p;
    const struct sunxi_mmc_card *c=&sdrt.card;
    if(sdrt.card_valid){
        p=text;memcpy(p,"CARD ",5);p+=5;
        for(unsigned i=0;i<4;++i)p=hex(p,c->cid[i],8);
        *p++=' ';for(unsigned i=0;i<4;++i)p=hex(p,c->csd[i],8);
        *p++=' ';p=hex(p,c->ocr,8);*p++=' ';p=hex(p,c->rca,4);*p++=' ';p=hex(p,c->sectors,16);
        *p++=' ';for(unsigned i=0;i<8;++i)p=hex(p,c->scr[i],2);
        uint32_t divisor=(c->module_divisor&0x7fu)|((c->module_divisor&SUNXI_MMC_MODULE_300M)?0x80u:0u);
        *p++=' ';p=hex(p,(uint32_t)sdrt.input.profile|(c->bus_width<<8)|(c->timing<<16)|(divisor<<24),8);
        *p++=' ';p=hex(p,c->card_hz,8);
        *p++=' ';p=hex(p,(uint32_t)c->uhs_outcome|(uint32_t)c->signal_1v8<<4|(uint32_t)c->uhs_caps<<5|
            (uint32_t)c->sample_delay<<8|(uint32_t)c->tuning_start<<16|(uint32_t)c->tuning_width<<24,8);
        *p++=' ';p=hex(p,c->tuning_map,16);*p++=' ';p=hex(p,sdrt.identified_us,8);
        *p++='\n';*p=0;platform_puts(text);
    }
    /* One line per read: index, total time from submission (24 MHz counter),
     * the core's first-observed command, first-byte and completion times, the
     * clean and invalidate times (microseconds), the poll count and the CRC-32.
     * The host knows each read's sectors from the plan. */
    for(unsigned i=0;i<sdrt.done;++i){
        const struct sdrt_timing *t=&sdrt.timing[i];
        p=text;*p++='R';*p++=' ';p=hex(p,i,3);*p++=' ';p=hex(p,sdrt.elapsed[i]>0xfffffu?0xfffffu:sdrt.elapsed[i],5);
        *p++=' ';p=hex(p,t->command_us>0xffffu?0xffffu:t->command_us,4);
        *p++=' ';p=hex(p,t->first_byte_us>0xfffffu?0xfffffu:t->first_byte_us,5);
        *p++=' ';p=hex(p,t->done_us>0xfffffu?0xfffffu:t->done_us,5);
        *p++=' ';p=hex(p,t->clean_us>0xfffu?0xfffu:t->clean_us,3);
        *p++=' ';p=hex(p,t->invalidate_us>0xfffu?0xfffu:t->invalidate_us,3);
        *p++=' ';p=hex(p,t->polls>0xffffu?0xffffu:t->polls,4);
        *p++=' ';p=hex(p,sdrt.crc[i],8);*p++='\n';*p=0;platform_puts(text);
    }
    line("SD_READS=",sdrt.done);line("SD_PLAN=",sdrt.input.reads);
    line("SD_PMIC_PARKED=",atomic_load_explicit(&sdrt.pmic_parked,memory_order_acquire));
    line("SD_CLOCK_OFF=",sdrt.clock_off);
    if(outcome==0){platform_puts(sdrt.done==sdrt.input.reads?"COMPLETE SD0 returning read\n":
        "INCOMPLETE SD0 returning read: window\n");return;}
    platform_puts("INCOMPLETE SD0 returning read: stopped\n");
    const struct a733_mmc_platform *m=&sdrt.platform;
    line("SD_REASON=",sdrt.reason);line("SD_STAGE=",sdrt.stage);
    line("SD_PLATFORM_STAGE=",m->stage);line("SD_PLATFORM_FAULT=",m->fault);
    line("SD_SIGNAL=",m->signal);line("SD_CORE_STATE=",m->core.state);
    line("SD_CORE_COMMAND=",m->core.command);line("SD_CORE_ARGUMENT=",m->core.argument);
    line("SD_CORE_PROTOCOL=",m->core.protocol);line("SD_CORE_STEP=",m->core.command_step);
    line("SD_RESULT_ERROR=",m->core.result.error);line("SD_RESULT_INTERRUPTS=",m->core.result.interrupts);
    line("SD_RESULT_STATUS=",m->core.result.status);line("SD_RESULT_RESPONSE=",m->core.result.response);
    line("SD_ELAPSED_US=",now_us()-sdrt.start);
}

static void pmic_task(void *unused)
{
    (void)unused;
    for(;;){
        if(atomic_load_explicit(&sdrt.pmic_stop,memory_order_acquire)){
            atomic_store_explicit(&sdrt.pmic_parked,1,memory_order_release);for(;;)vTaskDelay(1);
        }
        enum a733_pmic_status r=a733_pmic_poll(&sdrt.pmic);
        if(r!=A733_PMIC_READY&&r!=A733_PMIC_COMPLETE&&r!=A733_PMIC_PENDING&&r!=A733_PMIC_BUSY)fail(NULL);
        vTaskDelay(1);
    }
}
static void sd_task(void *unused)
{
    (void)unused;
    int r;
    for(;;){
        r=sdrt_step();
        if(r==2)continue;
        if(r)break;
        vTaskDelay(1);
    }
    /* A stopped provider keeps its controller, clock, pins and rails; the
     * report states where it stopped. The SD task asks the PMIC task to park
     * and waits for it until the end deadline; sdrt_quiet() requires the
     * park, and a missing park ends the run as a stop (reason 9). The card
     * clock stop that follows does not depend on it. */
    if(r<0&&sdrt.platform.initialized)(void)a733_mmc_platform_retain(&sdrt.platform);
    atomic_store_explicit(&sdrt.pmic_stop,1,memory_order_release);
    while(!atomic_load_explicit(&sdrt.pmic_parked,memory_order_acquire)&&now_us()<sdrt.end)vTaskDelay(1);
    int quiet=sdrt_quiet();
    sdrt.clock_off=(unsigned)sdrt_clock_off();
    if(r>0&&!quiet){sdrt.reason=9;r=-1;}
    if(r>0&&!sdrt.clock_off){sdrt.reason=10;r=-1;}
    sdrt_report(r>0?0:1);
    platform_exit(r>0&&sdrt.done==sdrt.input.reads?0:1);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
void kernel_main(void)
{
    platform_init();
    platform_puts("SD0 returning start\n");
    if(!sdrt_setup()){platform_puts("INCOMPLETE SD0 returning read: input\n");platform_exit(1);}
    line("SD_PROFILE=",sdrt.input.profile);line("SD_GENERATION=",sdrt.input.generation);
    TaskHandle_t pmic=xTaskCreateStatic(pmic_task,"sd-pmic",4096,NULL,2,stacks[0],&tasks[0]);
    /* Above the PMIC task: it does not run while a read is polled to
     * completion, and identification yields to it every millisecond. */
    TaskHandle_t sd=xTaskCreateStatic(sd_task,"sd-read",4096,NULL,3,stacks[1],&tasks[1]);
    if(!pmic||!sd||!sdrt_bind(pmic)){platform_puts("INCOMPLETE SD0 returning read: providers\n");platform_exit(1);}
    vTaskStartScheduler();
    platform_panic("scheduler returned",0);
}
