/* CPU cluster record of the integrated OS image with the Cortex-A76 cores
 * admitted (docs/designs/cpu-a76-dvfs.md). Before the scheduler, the thermal
 * start reads the clock configuration once and refuses a CPU-B cluster that
 * is not on its locked PLL with a decodable rate. The recognizer prints the
 * record before and after each recognition (apps/mimus/recorded.cc). */
#include "os.h"
#include "a733_cpu_clock.h"
#include "a733_mmio.h"
#include "FreeRTOS.h"
#include <stdatomic.h>
#include <stdio.h>
static struct a733_cpu_clock_report report;
/* Applied CPU-L and CPU-B points in MHz and their voltages in mV, 11 bits
 * each, then a 20-bit count of completed transitions. One store-release by
 * the owner and one load-acquire by a reader; aligned 64-bit accesses are
 * single-copy atomic on AArch64. A zero voltage was not read. */
static _Atomic uint64_t record;
static uint64_t pack(const uint32_t mhz[2],const uint32_t mv[2],uint32_t count)
{
    return (uint64_t)(mhz[0]&0x7ff)|(uint64_t)(mhz[1]&0x7ff)<<11|(uint64_t)(mv[0]&0x7ff)<<22|
        (uint64_t)(mv[1]&0x7ff)<<33|(uint64_t)(count&0xfffff)<<44;
}
/* Device-nGnRnE reads of the words that a733_cpu_clock_address allows; the
 * thermal start has checked their mappings. */
static int read_word(void *cookie,uintptr_t a,uint32_t *v)
{
    (void)cookie;
    if(!a733_cpu_clock_address(a,0))return 0;
    *v=a733_mmio_read32(a);return 1;
}
static uint32_t mhz(enum a733_cpu_domain d)
{return report.domain[d].decoded?(uint32_t)(report.domain[d].hz/1000000u):0;}
int os_cpu_admit(void)
{
    if(!a733_cpu_clock_report(read_word,NULL,&report))return 0;
    const struct a733_cpu_clock_domain *big=&report.domain[A733_CPU_BIG];
    if(!big->decoded||big->source!=3)return 0;
    const uint32_t points[2]={mhz(A733_CPU_LITTLE),mhz(A733_CPU_BIG)},mv[2]={0,0};
    atomic_store_explicit(&record,pack(points,mv,0),memory_order_release);
    return 1;
}
/* EL0-safe; the recognizer task. */
int os_cpu_clock_line(char *line,size_t bytes)
{
    uint64_t w=atomic_load_explicit(&record,memory_order_acquire);
    int n=snprintf(line,bytes,"CLOCK cpu_l_mhz=%u cpu_b_mhz=%u cpu_l_mv=%u cpu_b_mv=%u transitions=%u "
        "dsu_mhz=%u sid=%08x bin=%u core=%u\n",(unsigned)(w&0x7ff),(unsigned)(w>>11&0x7ff),
        (unsigned)(w>>22&0x7ff),(unsigned)(w>>33&0x7ff),(unsigned)(w>>44&0xfffff),
        (unsigned)mhz(A733_CPU_DSU),(unsigned)report.sid_word,report.binned?report.bin:99u,
        (unsigned)portGET_CORE_ID());
    return n>0&&(size_t)n<bytes?n:0;
}
