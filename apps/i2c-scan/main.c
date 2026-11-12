/* Header I2C scan with nothing attached: for each of TWI0 to TWI3 and S_TWI2, open the bus at 100 kHz and at 400 kHz,
 * probe all 128 seven-bit addresses with a zero-length write, expect an address NACK from every one, close the bus and
 * report. The scan is the application's whole workload; the OS service does every register operation and the controller
 * interrupt completes every probe (apps/os/i2c.c). The result is the 32-byte header and one 152-byte record per
 * (bus, rate) in the order scanned; apps/i2c-scan/host.py decodes it. */
#include "i2c_scan-app.h"
#include "sunstone_i2c.h"
#include <string.h>

#define MAGIC UINT64_C(0x3143325f5343414e)   /* "NAC_2C1" little-endian marker, checked by host.py */
#define BUSES 5u
#define RATES 2u
struct record {
    uint32_t bus, rate_request, rate, input_hz, clock_word, open_status, close_status, scanned;
    uint32_t nack, ack, other, status_mask, first_other, first_other_status, hardware_status_mask, reserved0;
    uint32_t lines_open, lines_close, interrupts, spurious, strays, idle_starts, busy_starts, fault;
    uint32_t fault_detail, reserved1;
    uint64_t open_ticks, scan_ticks, close_ticks;
    uint64_t min_ticks, max_ticks, sum_ticks;
};
_Static_assert(sizeof(struct record)==152,"record size");
struct report {uint64_t magic;uint32_t version,records,records_expected,failed;uint64_t reserved;struct record record[BUSES*RATES];};
_Static_assert(sizeof(struct report)==32+BUSES*RATES*152,"report size");
static struct report report;

static void wait_for(int (*call)(unsigned,uint32_t),unsigned bus,uint32_t rate,int *status)
{
    int r;
    while((r=call(bus,rate))==0)ss_i2c_wait(2000);
    *status=r;
}
static int open_call(unsigned bus,uint32_t rate){return ss_i2c_open(bus,rate);}
static int close_call(unsigned bus,uint32_t unused){(void)unused;return ss_i2c_close(bus);}

static void scan(struct record *r,unsigned bus,uint32_t rate)
{
    int status;
    const uint64_t t0=ss_counter_ticks();
    wait_for(open_call,bus,rate,&status);
    r->bus=bus;r->rate_request=rate;r->open_status=(uint32_t)status;
    r->open_ticks=ss_counter_ticks()-t0;
    r->min_ticks=UINT64_MAX;
    if(status!=1){report.failed=1;r->lines_open=r->lines_close=0xffffffffu;
        struct ss_i2c_info info;if(ss_i2c_info(bus,&info)==1){r->fault=info.fault;r->fault_detail=info.fault_detail;}
        return;}
    const uint64_t t1=ss_counter_ticks();
    for(unsigned address=0;address<128;++address){
        struct ss_i2c_message probe={(uint16_t)address,0,NULL,0};
        int s;
        while((s=ss_i2c_submit(bus,&probe,1,5000))==0)ss_i2c_wait(1000);
        if(s<0){report.failed=1;r->first_other=address;r->first_other_status=0xfffffff0u;++r->other;break;}
        struct ss_i2c_result result;
        while((s=ss_i2c_result(bus,&result))==0)ss_i2c_wait(5000);
        if(s<0){report.failed=1;++r->other;break;}
        ++r->scanned;
        const uint64_t ticks=result.finished_ticks-result.started_ticks;
        if(ticks<r->min_ticks)r->min_ticks=ticks;
        if(ticks>r->max_ticks)r->max_ticks=ticks;
        r->sum_ticks+=ticks;
        r->status_mask|=1u<<result.status;
        if(result.status==SS_I2C_NACK_ADDRESS)++r->nack;
        else if(result.status==SS_I2C_OK)++r->ack;
        else{if(!r->other){r->first_other=address;r->first_other_status=(uint32_t)result.status;}++r->other;}
        r->hardware_status_mask|=result.hardware_status<64?1u<<(result.hardware_status>>1):0x80000000u;
    }
    r->scan_ticks=ss_counter_ticks()-t1;
    ss_progress();
    struct ss_i2c_info info;
    if(ss_i2c_info(bus,&info)==1){
        r->rate=info.rate_hz;r->input_hz=info.input_hz;r->clock_word=info.clock_word;
        r->interrupts=info.interrupts;r->spurious=info.spurious;r->strays=info.strays;
        r->idle_starts=info.idle_starts;r->busy_starts=info.busy_starts;r->lines_open=info.lines_open;
    }
    const uint64_t t2=ss_counter_ticks();
    wait_for(close_call,bus,0,&status);
    r->close_status=(uint32_t)status;
    r->close_ticks=ss_counter_ticks()-t2;
    if(ss_i2c_info(bus,&info)==1){r->lines_close=info.lines_close;r->fault=info.fault;r->fault_detail=info.fault_detail;}
    if(status!=1)report.failed=1;
}
void i2c_scan_main(const struct ss_grant *grant)
{
    (void)grant;
    static const uint32_t rates[RATES]={100000,400000};
    /* The CPU clock owner raises both clusters while the application starts. Like the other CPU-return applications,
     * scan only once it has finished and the thermal policy is not throttling, so the scan sees a settled system. */
    while(!ss_clock_ready()||ss_throttled()){
        if(ss_live()<0)ss_fail(0x1c20);
        ss_progress();
        ss_usb_wait(1000);
    }
    report.magic=MAGIC;report.version=1;report.records_expected=BUSES*RATES;
    for(unsigned bus=0;bus<BUSES;++bus)
        for(unsigned i=0;i<RATES;++i){
            scan(&report.record[report.records],bus,rates[i]);
            ++report.records;
            ss_progress();
        }
    ss_finish(&report,sizeof(report));
}
