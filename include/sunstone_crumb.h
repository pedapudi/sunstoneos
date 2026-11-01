#ifndef SUNSTONE_CRUMB_H
#define SUNSTONE_CRUMB_H
/* Progress breadcrumbs for returning images under the host-armed watchdog.
 *
 * A WDT0 reset preserves the seven RTC general-purpose words used here
 * (measured 2026-10-31, evidence/wdt-fel-return-20261031/); DRAM is lost, and
 * NPU jobs may own SRAM 0x44000-0xf3fff, so no SRAM is used. Word 0x07090108
 * is the FEL flag and is never written. After a watchdog reset the host reads
 * these words from the new FEL instance (tools/a733-crumbs.py).
 *
 * sunstone_crumb(stage, value) records a stage from EL0 or EL1, in a task or
 * an interrupt handler: it issues SVC #0x5c0d, which the returning image's EL1
 * vector stores without using RAM (apps/os/cpu-return/vectors.patch). EL3 code
 * uses sunstone_crumb_el3(). Every store is one 32-bit word, so a crumb from
 * another core can replace LAST but cannot tear it. Images without
 * SUNSTONE_CRUMBS compile all calls to nothing.
 *
 * While the session is live, the CPU0 supervisor restarts WDT0 every 20 ms
 * (sunstone_watchdog_restart). A failure stops the restarts, so WDT0 resets
 * the board one interval after the failure or after the session deadline.
 * The only normal return disarms WDT0, records RETURN_ROM with the mode read
 * back, and parks instead of returning if the enable bit is still set.
 *
 * Words (RTC base 0x07090000):
 *   0x100 TAG   0x5c0d << 16 | low 16 bits of the session epoch
 *   0x104 LAST  stage << 16 | low 16 bits of value
 *   0x10c TIME  core << 29 | (CNTPCT >> 10) & 0x1fffffff (42.7 us units)
 *   0x110 FAULT 0xfa << 24 | core << 20 | level << 16 | vector slot << 8 | x0 low byte
 *   0x114 ESR   level 3: ESR_EL3; level 1: x1 at the SMC, which is ESR_EL1
 *               for an EL1 fault (x0 = 4) and the detail for a deliberate exit
 *   0x118 ELR   low 32 bits of ELR at that level
 *   0x11c FAR   low 32 bits of FAR at that level
 * The first fault after entry fills FAULT, ESR, ELR and FAR; later ones leave
 * them. The success SMC (x0 = 0) records nothing. */
#include <stdint.h>

#define SUNSTONE_CRUMB_DEFINED 1
#define SUNSTONE_CRUMB_RTC 0x07090000u
#define SUNSTONE_CRUMB_TAG 0x100u
#define SUNSTONE_CRUMB_LAST 0x104u
#define SUNSTONE_CRUMB_TIME 0x10cu
#define SUNSTONE_CRUMB_FAULT 0x110u
#define SUNSTONE_CRUMB_ESR 0x114u
#define SUNSTONE_CRUMB_ELR 0x118u
#define SUNSTONE_CRUMB_FAR 0x11cu
#define SUNSTONE_CRUMB_TAG_MAGIC 0x5c0du
#define SUNSTONE_CRUMB_FAULT_MAGIC 0xfau
#define SUNSTONE_CRUMB_SVC 0x5c0d
/* WDT0 mode the host arms before launch: 16 s interval (index 11), enabled. */
#define SUNSTONE_CRUMB_WATCHDOG_MODE 0xb1u
#define SUNSTONE_CRUMB_WDT_RESTART 0x0205000cu

/* Stage identifiers: the high byte names the phase group. Other branches add
 * stages inside their own group (0x04 GPU, 0x06 NPU) and keep these values. */
enum {
    SUNSTONE_CRUMB_EL3_ENTRY = 0x0101,      /* boot core reached EL3 C entry */
    SUNSTONE_CRUMB_EL3_ADMITTED = 0x0102,   /* contract and armed watchdog admitted */
    SUNSTONE_CRUMB_EL3_MONITOR = 0x0103,    /* cached tables and monitor ready */
    SUNSTONE_CRUMB_EL3_GIC = 0x0104,        /* MBUS restored, distributor routed */
    SUNSTONE_CRUMB_EL3_SECONDARY = 0x0105,  /* value: core dispatched */
    SUNSTONE_CRUMB_EL3_SECURITY = 0x0106,   /* security setup done, entering EL1 */
    SUNSTONE_CRUMB_EL3_STOP = 0x0111,       /* a733_smp_stop; value: status << 12 | detail */
    SUNSTONE_CRUMB_CLOCK_GUARD = 0x0201,    /* CPU clock entry guard passed */
    SUNSTONE_CRUMB_CLOCK_CHANGE = 0x0202,   /* request; value: domain << 12 | MHz */
    SUNSTONE_CRUMB_CLOCK_READY = 0x0203,    /* every granted request issued */
    SUNSTONE_CRUMB_PMIC_STOP = 0x0210,      /* PMIC owner stopped */
    SUNSTONE_CRUMB_THERMAL_DONE = 0x0221,   /* thermal service stopped */
    SUNSTONE_CRUMB_ASSET_START = 0x0301,    /* value: asset index */
    SUNSTONE_CRUMB_ASSET_DONE = 0x0302,     /* value: asset index, hashed */
    SUNSTONE_CRUMB_RECOGNITION_START = 0x0303,
    SUNSTONE_CRUMB_RECOGNITION_END = 0x0304, /* workers stopped */
    SUNSTONE_CRUMB_OUTPUT = 0x0305,         /* value: transcript bytes */
    SUNSTONE_CRUMB_CLOSE = 0x0306,          /* terminals notified */
    SUNSTONE_CRUMB_TERMINAL = 0x0307,       /* value: core */
    SUNSTONE_CRUMB_GPU_CONSTRUCT = 0x0401,
    SUNSTONE_CRUMB_GPU_CONSTRUCTED = 0x0402, /* value: result */
    SUNSTONE_CRUMB_GPU_FIRMWARE = 0x0403,
    SUNSTONE_CRUMB_GPU_READY = 0x0404,      /* value: result */
    SUNSTONE_CRUMB_GPU_SUBMIT = 0x0410,     /* value: job sequence */
    SUNSTONE_CRUMB_GPU_COMPLETE = 0x0411,   /* value: job sequence */
    SUNSTONE_CRUMB_GPU_RETIRE = 0x0420,
    SUNSTONE_CRUMB_GPU_RETIRED = 0x0421,    /* value: result */
    SUNSTONE_CRUMB_GPU_THERMAL = 0x0430,    /* value: result */
    SUNSTONE_CRUMB_GPU_PARKED = 0x0440,     /* value: result */
    SUNSTONE_CRUMB_RETURN_POWER_DOWN = 0x0502, /* value: core */
    SUNSTONE_CRUMB_RETURN_ROM = 0x0503,     /* value: WDT0 mode after disarm */
};

#ifdef SUNSTONE_CRUMBS
static inline void sunstone_crumb(uint32_t stage, uint32_t value)
{
    __asm__ volatile("mov x0, %0\n mov x1, %1\n svc #0x5c0d"
        :: "r"((uint64_t)stage), "r"((uint64_t)value) : "x0", "x1", "memory");
}
/* EL3 only: MMU off, or the cached monitor's Device identity map of 0-1 GiB. */
static inline void sunstone_crumb_el3(uint32_t stage, uint32_t value)
{
    uint64_t mpidr, now;
    __asm__ volatile("mrs %0, mpidr_el1\n mrs %1, cntpct_el0" : "=r"(mpidr), "=r"(now));
    volatile uint32_t *rtc = (volatile uint32_t *)(uintptr_t)SUNSTONE_CRUMB_RTC;
    rtc[SUNSTONE_CRUMB_LAST / 4] = stage << 16 | (value & 0xffff);
    rtc[SUNSTONE_CRUMB_TIME / 4] = (uint32_t)((mpidr >> 8) & 7) << 29 | (uint32_t)((now >> 10) & 0x1fffffff);
    __asm__ volatile("dsb sy" ::: "memory");
}
/* EL1 or EL3 only (the Device map of 0-1 GiB is privileged): restart WDT0. */
static inline void sunstone_watchdog_restart(void)
{
    *(volatile uint32_t *)(uintptr_t)SUNSTONE_CRUMB_WDT_RESTART = 0x14afu;
    __asm__ volatile("dsb sy" ::: "memory");
}
/* Boot core at EL3 entry: tag this session and clear the other words. */
static inline void sunstone_crumb_el3_start(uint64_t epoch)
{
    volatile uint32_t *rtc = (volatile uint32_t *)(uintptr_t)SUNSTONE_CRUMB_RTC;
    rtc[SUNSTONE_CRUMB_LAST / 4] = 0; rtc[SUNSTONE_CRUMB_TIME / 4] = 0;
    rtc[SUNSTONE_CRUMB_FAULT / 4] = 0; rtc[SUNSTONE_CRUMB_ESR / 4] = 0;
    rtc[SUNSTONE_CRUMB_ELR / 4] = 0; rtc[SUNSTONE_CRUMB_FAR / 4] = 0;
    rtc[SUNSTONE_CRUMB_TAG / 4] = SUNSTONE_CRUMB_TAG_MAGIC << 16 | (uint32_t)(epoch & 0xffff);
    __asm__ volatile("dsb sy" ::: "memory");
}
#else
static inline void sunstone_crumb(uint32_t stage, uint32_t value) { (void)stage; (void)value; }
static inline void sunstone_crumb_el3(uint32_t stage, uint32_t value) { (void)stage; (void)value; }
static inline void sunstone_crumb_el3_start(uint64_t epoch) { (void)epoch; }
static inline void sunstone_watchdog_restart(void) {}
#endif
#endif
