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
 * vector stores without using RAM (apps/os/vectors.patch). EL3 code
 * uses sunstone_crumb_el3(). Every store is one 32-bit word, so a crumb from
 * another core can replace LAST but cannot tear it. Images without
 * SUNSTONE_CRUMBS compile all calls to nothing.
 *
 * While the session is live, the CPU0 supervisor restarts WDT0 every 20 ms
 * (sunstone_watchdog_restart). A failure stops the restarts, so WDT0 resets
 * the board one interval after the failure or after the session deadline.
 * The only normal return disarms WDT0, records RETURN_ROM with the mode read
 * back, and parks instead of returning if the enable bit is still set. The
 * EL3 stop (a733_smp_stop) shortens an armed WDT0 to 0.5 s and restarts it, so
 * a deliberate stop returns to FEL about half a second later.
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
 * them. The success SMC (x0 = 0) records nothing.
 *
 * The first board failure (board_fail_at, apps/board/main.c) fills the same
 * words, from EL0 or EL1, through SVC #0x5c0e, before any later exit:
 *   FAULT 0xfa << 24 | core << 20 | 0xb << 16 | SPSR_EL1.M << 8 (0 EL0, 4/5 EL1)
 *   ESR   site: group << 12 | source line or detail, aux << 16 (bits 30:16),
 *         bit 31 set when ELR is a return address
 *   ELR   caller: the failing statement (SUNSTONE_CRUMB_HERE) or, with ESR
 *         bit 31, the return address of the failing call (symbolize ELR - 4);
 *         a caller that tail-calls board_fail shows its own caller instead
 *   FAR   value given by the site */
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
/* With FAULT clear, ELR carries this marker, ESR the inherited dynamic-OFF
 * core mask and FAR the completed static-OFF core mask. A real first fault
 * replaces all three diagnostic words before publishing FAULT. */
#define SUNSTONE_CRUMB_PROBE_POLICY_MAGIC 0x50505531u
#define SUNSTONE_CRUMB_SVC 0x5c0d
#define SUNSTONE_CRUMB_FAIL_SVC 0x5c0e
#define SUNSTONE_CRUMB_FAIL_LEVEL 0xbu
/* FAULT's level after a device failure that follows an application failure (the failure return): the words then hold the device's site, value
 * and caller, and the application's failure is the one the claim recorded in the result. Only that one replacement; every other later failure leaves the record. */
#define SUNSTONE_CRUMB_FAIL_LEVEL_SECOND 0xcu
/* Board failure site groups (ESR bits 15:12). */
#define SUNSTONE_CRUMB_FAIL_BOARD 0x1u      /* board_fail(context) or a board site; value: context or site detail */
#define SUNSTONE_CRUMB_FAIL_THERMAL 0x2u    /* thermal owner cancel/fault; value: driver fault_site (0 if none) */
#define SUNSTONE_CRUMB_FAIL_CPU_RETURN 0x3u /* cpu-return fail(detail); low 12 bits: detail */
#define SUNSTONE_CRUMB_FAIL_APPLICATION 0x4u /* ss_fail(detail); low 12 bits: detail, value: detail */
#define SUNSTONE_CRUMB_FAIL_CPU_CLOCK 0x5u  /* CPU clock owner refusal; aux: phase << 8 | cluster; value: status */
/* CPU clock site flags. AUX (aux bit 13) marks this layout; an image
 * without it put phase << 24 | cluster << 16 | status & 0xffff in the value.
 * GUARD (aux bit 14): the value is the entry guard's full refusal word,
 * detail << 12 | line of apps/os/cpu-return-entry.c. */
#define SUNSTONE_CRUMB_FAIL_CPU_PLL 0x9u /* Mismatched PLL word; expected N/state/domain in aux. */
#define SUNSTONE_CRUMB_FAIL_CPU_RETAINED 0x8u /* First CPU driver fault; layout in apps/os/cpu.c. */
#define SUNSTONE_CRUMB_CPU_AUX 0x20000000u
#define SUNSTONE_CRUMB_CPU_GUARD 0x40000000u
#define SUNSTONE_CRUMB_FAIL_PMIC 0x6u       /* PMIC power service; value: authority word */
#define SUNSTONE_CRUMB_FAIL_GPU 0x7u        /* GPU failure; value: reason or register status */
#define SUNSTONE_CRUMB_FAIL_NPU 0xau        /* NPU failure; value: reason or register status */
/* Registered return faults retain the owner in bits 23:16, the original
 * group in bits 27:24 and its detail in bits 11:0. The value and caller keep
 * their original meanings; each descriptor also keeps the original site. */
#define SUNSTONE_CRUMB_FAIL_QUIESCE 0xcu
#define SUNSTONE_CRUMB_QUIESCE_NPU 0x0u
#define SUNSTONE_CRUMB_QUIESCE_GPU 0x1u
#define SUNSTONE_CRUMB_QUIESCE_USB 0x2u
#define SUNSTONE_CRUMB_QUIESCE_RADIO 0x3u
#define SUNSTONE_CRUMB_QUIESCE_AUDIO 0x4u
#define SUNSTONE_CRUMB_QUIESCE_HEADSET 0x5u
#define SUNSTONE_CRUMB_QUIESCE_STORAGE 0x6u
#define SUNSTONE_CRUMB_QUIESCE_DMA 0x7u
#define SUNSTONE_CRUMB_QUIESCE_CPU_CLOCK 0x8u
#define SUNSTONE_CRUMB_QUIESCE_THERMAL_OWNER 0x9u
#define SUNSTONE_CRUMB_QUIESCE_PMIC_OWNER 0xau
#define SUNSTONE_CRUMB_QUIESCE_TERMINAL_OWNER 0xbu
#define SUNSTONE_CRUMB_FAIL_USB0 0xbu       /* USB0 guard; layout in sunstone_usb0_stop.h */
/* Frozen finite NPU images use group 8. The host distinguishes their defined
 * finite return producers from CPU retention using the launched image ELF. */
/* NPU first-failure layouts, independent of progress stages. Register offsets
 * are the physical address's low 16 bits, divided by four in aux bits 13:0;
 * the symbolized source identifies the bank. Value keeps all 32 status bits.
 * A lifecycle record instead puts enum a733_npu_startup_stage in aux bits 7:0. */
#define SUNSTONE_CRUMB_NPU_REGISTER 0x40000000u
#define SUNSTONE_CRUMB_NPU_STARTUP 0x20000000u
/* Sustained NPU measurements: bits 26:24 hold stage minus 23 (23..28),
 * bits 23:16 hold job index (0..199, 255 for global checks). FAR holds the
 * observed operand's low word when it fits, otherwise its high word and
 * HIGH_WORD is set. The original site/caller identifies the exact predicate
 * and its expected value. REGISTER and STARTUP remain clear. */
#define SUNSTONE_CRUMB_NPU_MEASUREMENT 0x10000000u
#define SUNSTONE_CRUMB_NPU_MEASUREMENT_HIGH_WORD 0x08000000u
/* OS request failures use this flag only when REGISTER, STARTUP and
 * MEASUREMENT are clear. Aux bits 10:8 hold the service operation; bits 7:0
 * retain the expected ticket's low byte. FAR holds the failed predicate's
 * observed low 32 bits. The retained OS snapshot keeps full identities and
 * operands; the RTC record alone cannot reconstruct a full 64-bit ticket. */
#define SUNSTONE_CRUMB_NPU_REQUEST 0x08000000u
/* Cancellation predicate: sample bits 24:23 (3 unavailable), low seven ticket
 * bits 22:16. FAR keeps the operand's low word, or its high word when needed.
 * The complete operand and ticket also remain in the pinned diagnostic. */
#define SUNSTONE_CRUMB_NPU_CANCEL 0x04000000u
#define SUNSTONE_CRUMB_NPU_CANCEL_HIGH_WORD 0x02000000u
/* Without CANCEL, OUTPUT uses bit 25 and ticket low9 in bits24:16. FAR is
 * byte index <<8 | actual byte; bit31 instead denotes an actual byte length. */
#define SUNSTONE_CRUMB_NPU_OUTPUT 0x02000000u
#define SUNSTONE_CRUMB_NPU_OUTPUT_LENGTH 0x80000000u
#define SUNSTONE_CRUMB_FAIL_RETURN 0x80000000u /* site flag: caller is a return address */
#define SUNSTONE_CRUMB_FAIL_SITE(group) (SUNSTONE_CRUMB_FAIL_##group << 12 | ((uint32_t)__LINE__ & 0xfffu))
/* The address of this statement, for the caller word of a board failure.
 * Unlike a return address it survives inlining and tail calls. */
#if defined(SUNSTONE_CRUMBS) && defined(__aarch64__)
#define SUNSTONE_CRUMB_HERE() __extension__({ const void *here_; __asm__ volatile("adr %0, ." : "=r"(here_)); here_; })
#else
#define SUNSTONE_CRUMB_HERE() ((const void *)0)
#endif
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
    /* Return step: 1 drain, 2 request CPU-B lowering, 3 wait for lowering,
     * 4 verify clocks/leases, 5 done. Value: step << 12 | owner phase << 10 |
     * cluster << 8 | driver state. Owner phases: START, INSPECT, REQUEST,
     * FOLLOW, numbered 0 through 3. */
    SUNSTONE_CRUMB_CLOCK_RETURN = 0x0204,
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
    SUNSTONE_CRUMB_GPU_PEER_SUBMIT = 0x0412, /* value: peer job sequence */
    SUNSTONE_CRUMB_GPU_PEER_COMPLETE = 0x0413, /* value: peer job sequence */
    SUNSTONE_CRUMB_GPU_RETIRE = 0x0420,
    SUNSTONE_CRUMB_GPU_RETIRED = 0x0421,    /* value: result */
    SUNSTONE_CRUMB_GPU_THERMAL = 0x0430,    /* value: result */
    SUNSTONE_CRUMB_GPU_PARKED = 0x0440,     /* value: result */
    SUNSTONE_CRUMB_GPU_RECOVERY_STOP = 0x0450, /* value: failed firmware serial */
    SUNSTONE_CRUMB_GPU_RECOVERY_RESET = 0x0451, /* value: failed firmware serial */
    SUNSTONE_CRUMB_GPU_RECOVERY_RESTART = 0x0452, /* value: failed firmware serial */
    SUNSTONE_CRUMB_GPU_RECOVERY_READY = 0x0453, /* value: failed firmware serial */
    SUNSTONE_CRUMB_RETURN_POWER_DOWN = 0x0502, /* value: core */
    SUNSTONE_CRUMB_PROBE_BEGIN = 0x0801,
    SUNSTONE_CRUMB_PROBE_PROGRESS = 0x0802,
    SUNSTONE_CRUMB_PROBE_CLEAN = 0x0803,
    SUNSTONE_CRUMB_PROBE_POLICY = 0x0804, /* inherited policy; core << 12 | PWPR */
    SUNSTONE_CRUMB_RETURN_ROM = 0x0503,     /* value: WDT0 mode after disarm */
    SUNSTONE_CRUMB_NPU_WAIT_STARTUP = 0x0601,
    SUNSTONE_CRUMB_NPU_OPEN = 0x0602,
    SUNSTONE_CRUMB_NPU_BOOTSTRAP_READY = 0x0603,
    SUNSTONE_CRUMB_NPU_SUBMIT = 0x0604,
    SUNSTONE_CRUMB_NPU_JOB_DONE = 0x0605,
    SUNSTONE_CRUMB_NPU_OUTPUT_COPIED = 0x0606,
    SUNSTONE_CRUMB_NPU_REAPED = 0x0607,
    SUNSTONE_CRUMB_NPU_CLOSE = 0x0608,
    SUNSTONE_CRUMB_NPU_LOCAL_STOP = 0x0609,
    SUNSTONE_CRUMB_NPU_QUIESCENT = 0x060a,
    SUNSTONE_CRUMB_NPU_EXIT = 0x060b,
    SUNSTONE_CRUMB_NPU_HELPER_OPEN = 0x0620,   /* value: runtime slot */
    SUNSTONE_CRUMB_NPU_HELPER_LOAD = 0x0621,   /* value: model index */
    SUNSTONE_CRUMB_NPU_HELPER_SUBMIT = 0x0622, /* value: model index */
    SUNSTONE_CRUMB_NPU_HELPER_DONE = 0x0623, /* value: job ticket */
    SUNSTONE_CRUMB_NPU_HELPER_COPIED = 0x0624, /* value: copied bytes */
    SUNSTONE_CRUMB_NPU_HELPER_CLOSE = 0x0625,  /* value: runtime slot */
    SUNSTONE_CRUMB_NPU_HELPER_REFUSED = 0x062e,
    SUNSTONE_CRUMB_NPU_HELPER_FAILED = 0x062f,
    /* Startup register pass; each marker precedes its sub-step's reads.
     * PERI0_OUTPUT repeats for each fabric parent, with fabric index as value. */
    SUNSTONE_CRUMB_NPU_PASS_DOMAIN = 0x0630,
    SUNSTONE_CRUMB_NPU_PASS_REFERENCE = 0x0631,
    SUNSTONE_CRUMB_NPU_PASS_SRAM = 0x0632,
    SUNSTONE_CRUMB_NPU_PASS_AHB = 0x0633,
    SUNSTONE_CRUMB_NPU_PASS_NSI = 0x0634,
    SUNSTONE_CRUMB_NPU_PASS_MBUS = 0x0635,
    SUNSTONE_CRUMB_NPU_PASS_PERI0 = 0x0636,
    SUNSTONE_CRUMB_NPU_PASS_PERI0_OUTPUT = 0x0637,
    SUNSTONE_CRUMB_NPU_PASS_VIDEO_ENC = 0x0638,
    SUNSTONE_CRUMB_NPU_PASS_VIDEO_DEC = 0x0639,
    SUNSTONE_CRUMB_NPU_PASS_VIDEO_BUS = 0x063a,
    SUNSTONE_CRUMB_NPU_PASS_MODULE = 0x063b,
    SUNSTONE_CRUMB_NPU_PASS_RESETS = 0x063c,
    SUNSTONE_CRUMB_NPU_PASS_DRAM_CLOCK = 0x063d,
    SUNSTONE_CRUMB_NPU_PASS_DRAM_PHY = 0x063e,
    SUNSTONE_CRUMB_NPU_PASS_CLOCK_MODES = 0x063f,
    SUNSTONE_CRUMB_NPU_PASS_PLL_PROFILE = 0x0640,
    SUNSTONE_CRUMB_NPU_PASS_PLL_GATES = 0x0641,
    SUNSTONE_CRUMB_NPU_PASS_PLL_TUNING = 0x0642,
    SUNSTONE_CRUMB_NPU_PASS_DONE = 0x0643,
    SUNSTONE_CRUMB_NPU_FIRST_CLOSE = 0x0644,
    SUNSTONE_CRUMB_NPU_STOPPED = 0x0645,
    SUNSTONE_CRUMB_NPU_RESTART_READY = 0x0646,
    SUNSTONE_CRUMB_NPU_REOPEN = 0x0647,
    SUNSTONE_CRUMB_NPU_POWER_OFF_REQUEST = 0x0648,
    SUNSTONE_CRUMB_NPU_POWER_OFF_ACK = 0x0649,
    SUNSTONE_CRUMB_NPU_POWER_ON_REQUEST = 0x064a,
    SUNSTONE_CRUMB_NPU_POWER_ON_ACK = 0x064b,
    /* Local table value is its zero-based step; PPU delay value is the word
     * index. Every marker precedes the named operation. */
    SUNSTONE_CRUMB_NPU_LOCAL_STOP_WRITE = 0x064c,
    SUNSTONE_CRUMB_NPU_LOCAL_OPEN_WRITE = 0x064d,
    SUNSTONE_CRUMB_NPU_PLL_OPEN_WRITE = 0x064e,
    SUNSTONE_CRUMB_NPU_SRAM_TO_CPU = 0x064f,
    SUNSTONE_CRUMB_NPU_SRAM_TO_NPU = 0x0650,
    SUNSTONE_CRUMB_NPU_POWER_PRECHECK = 0x0651,
    SUNSTONE_CRUMB_NPU_POWER_REFUSED = 0x0652,
    SUNSTONE_CRUMB_NPU_PPU_DELAY_WRITE = 0x0653,
    SUNSTONE_CRUMB_NPU_PPU_POLICY = 0x0654,
    SUNSTONE_CRUMB_NPU_PPU_STATUS = 0x0655,
    SUNSTONE_CRUMB_NPU_CANCEL_TRIGGER = 0x0660,
    SUNSTONE_CRUMB_NPU_CANCEL_PROVEN = 0x0661,
    SUNSTONE_CRUMB_NPU_CANCEL_ASSERT = 0x0662,
    SUNSTONE_CRUMB_NPU_CANCEL_RELEASE = 0x0663,
    SUNSTONE_CRUMB_NPU_CANCEL_IDLE = 0x0664,
    SUNSTONE_CRUMB_NPU_CANCEL_REAPED = 0x0665,
    SUNSTONE_CRUMB_NPU_CANCEL_CLOSED = 0x0666,
    SUNSTONE_CRUMB_NPU_CANCEL_IRQ_RETIRED = 0x0667,
    SUNSTONE_CRUMB_NPU_CANCEL_IRQ_REARM = 0x0668,
    SUNSTONE_CRUMB_NPU_CANCEL_BOOTSTRAP = 0x0669,
    SUNSTONE_CRUMB_NPU_CANCEL_OUTPUT = 0x066a,
    SUNSTONE_CRUMB_NPU_CANCEL_QUIESCENT = 0x066b,
    SUNSTONE_CRUMB_NPU_RETAINED = 0x06ff,
};

#ifdef SUNSTONE_GPU_TIMING
#include "gpu_queue_timing.h"
#define SUNSTONE_GPU_TIMING_CRUMB(stage,value) do { if ((stage)==SUNSTONE_CRUMB_GPU_SUBMIT || (stage)==SUNSTONE_CRUMB_GPU_COMPLETE || (stage)==SUNSTONE_CRUMB_GPU_PEER_SUBMIT || (stage)==SUNSTONE_CRUMB_GPU_PEER_COMPLETE) gpu_queue_timing_crumb(stage,value); } while (0)
#else
#define SUNSTONE_GPU_TIMING_CRUMB(stage,value) ((void)0)
#endif
#ifdef SUNSTONE_CRUMBS
#ifdef SUNSTONE_A733_OS
/* The OS image's sealed lifetime copy (apps/os/sealed.c), readable at EL1 and EL0. Breadcrumbs are stored only under the
 * returning lifetime (3, A733_LIFETIME_RETURN), so another lifetime takes no SVC. Weak: a host harness that links no
 * sealed session records no breadcrumb. */
extern const volatile uint32_t os_crumb_lifetime __attribute__((weak));
#define SUNSTONE_CRUMB_LIVE() (&os_crumb_lifetime && os_crumb_lifetime == 3u)
#else
#define SUNSTONE_CRUMB_LIVE() 1
#endif
static inline void sunstone_crumb(uint32_t stage, uint32_t value)
{
    SUNSTONE_GPU_TIMING_CRUMB(stage,value);
    if (!SUNSTONE_CRUMB_LIVE()) return;
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
/* EL0 or EL1 only: the first board failure's site, value and caller. */
static inline void sunstone_crumb_fail(uint32_t site, uint32_t value, const void *caller)
{
    __asm__ volatile("mov x0, %0\n mov x1, %1\n mov x2, %2\n svc #0x5c0e"
        :: "r"((uint64_t)site), "r"((uint64_t)value), "r"((uint64_t)(uintptr_t)caller) : "x0", "x1", "x2", "memory");
}
/* EL3 only: a failure note from a path that cannot use the SVC. It fills FAULT, ESR, ELR and FAR as the first board failure does, with level 3 in FAULT, and only while FAULT is
 * still clear: the first fault or failure of a session keeps its record. */
static inline void sunstone_crumb_el3_fail(uint32_t site, uint32_t value, const void *caller)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    volatile uint32_t *rtc = (volatile uint32_t *)(uintptr_t)SUNSTONE_CRUMB_RTC;
    if (rtc[SUNSTONE_CRUMB_FAULT / 4]) return;
    rtc[SUNSTONE_CRUMB_ESR / 4] = site; rtc[SUNSTONE_CRUMB_ELR / 4] = (uint32_t)(uintptr_t)caller; rtc[SUNSTONE_CRUMB_FAR / 4] = value;
    rtc[SUNSTONE_CRUMB_FAULT / 4] = SUNSTONE_CRUMB_FAULT_MAGIC << 24 | (uint32_t)((mpidr >> 8) & 7) << 20 | 3u << 16;
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
static inline void sunstone_crumb(uint32_t stage, uint32_t value) { SUNSTONE_GPU_TIMING_CRUMB(stage,value); (void)stage; (void)value; }
static inline void sunstone_crumb_el3(uint32_t stage, uint32_t value) { (void)stage; (void)value; }
static inline void sunstone_crumb_el3_start(uint64_t epoch) { (void)epoch; }
static inline void sunstone_watchdog_restart(void) {}
static inline void sunstone_crumb_fail(uint32_t site, uint32_t value, const void *caller) { (void)site; (void)value; (void)caller; }
static inline void sunstone_crumb_el3_fail(uint32_t site, uint32_t value, const void *caller) { (void)site; (void)value; (void)caller; }
#endif
#endif
