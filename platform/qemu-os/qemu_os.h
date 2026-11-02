#ifndef SUNSTONE_QEMU_OS_H
#define SUNSTONE_QEMU_OS_H
/* Interfaces between the virtual OS board's description (board.c), its
 * providers and the platform. Everything here is host-side: none of it
 * exists in the A733 image. */
#include <stdint.h>
void qemu_os_dec(uint64_t value);
void qemu_os_hex(uint64_t value);
void qemu_os_puts(const char *text);
_Noreturn void platform_panic(const char *name, unsigned code);
_Noreturn void platform_exit_privileged(unsigned status);
void qemu_os_log(const char *line);

/* Fixed addresses above the image's RAM. The check loads the scenario, the
 * launch contract and the sealed session there, as the stage-2 loader places
 * the session on the board (A733_BOARD_SESSION_ADDRESS, 0x41f00000). */
#define QEMU_OS_SCENARIO_ADDRESS UINT64_C(0x41e00000)
#define QEMU_OS_CONTRACT_ADDRESS UINT64_C(0x41e01000)
#define QEMU_OS_SCENARIO_MAGIC UINT64_C(0x514f535343454e31)
#define QEMU_OS_SCENARIO_VERSION 1u
/* Pull and drive of the PMIC bus pins (PL0, PL1) as boot0 leaves them; the
 * session declares the same values. */
#define QEMU_OS_PL_PULL 1u
#define QEMU_OS_PL_DRIVE 2u

/* Sensor channel order: CPUB, DDR, NPU, CPUL, GPU. */
#define QEMU_OS_SENSORS 5u
enum qemu_os_fault {
    QEMU_OS_FAULT_NONE,
    QEMU_OS_FAULT_THERMAL_CALIBRATION, /* the efuse calibration words are erased */
    QEMU_OS_FAULT_THERMAL_CLOCK,       /* the sensor clock gate is closed */
    QEMU_OS_FAULT_PMIC_BUS,            /* the PMIC bus's data line is held low */
    QEMU_OS_FAULT_USB_SILENT,          /* the USB transport task starts and reports nothing */
    QEMU_OS_FAULT_USB_STALL,           /* the USB transport reports ready, then stops reporting */
    QEMU_OS_FAULT_DISK_MOUNT,          /* the block device refuses every read of the first 64 sectors */
    QEMU_OS_FAULT_DISK_SUBMIT,         /* the block device refuses to start a streamed request */
    QEMU_OS_FAULT_DISK_MIDWAY,         /* the block device fails the fifth streamed request */
    QEMU_OS_FAULT_DISK_ABSENT,         /* the board offers no block device */
    QEMU_OS_FAULT_STRAY_NOTIFY,        /* the SD open is slow, and a USB-change hint reaches the application's task during it */
    QEMU_OS_FAULT_LOCK_RETAINED,       /* the SD open retains the platform register lock, as a failed controller transaction does, and the enable then waits */
    QEMU_OS_FAULT_CLOCKS_FAILED,       /* the clock provider is marked failed during the enable, which waits */
    QEMU_OS_FAULT_OPEN_FAILS           /* the SD open stops at its identification stage (7) with a platform result */
};
/* The scenario is data, written by the check. Times are microseconds of
 * guest time after the board's start. */
struct qemu_os_scenario {
    uint64_t magic, version, bytes;
    uint32_t fault, period_us;
    /* First time at which each sensor has a reading; UINT64_MAX never. */
    uint64_t first_us[QEMU_OS_SENSORS];
    /* The reading at that time and its change per reading, in milli-degrees C. */
    int32_t temperature_mc[QEMU_OS_SENSORS];
    int32_t slope_mc[QEMU_OS_SENSORS];
    /* The USB device transport reports ready this long after its task starts. */
    uint64_t usb_ready_us;
    /* After the application's main entry: 0 waits for its normal finish (and
     * for the throttle to clear); 1 ends the run at the first instruction of the
     * application's main; otherwise the run ends this long after entry. */
    uint64_t entry_linger_us;
    /* The run ends with status 4 at this guest time. */
    uint64_t limit_us;
    /* Bit 0: log every S_TWI0 register access to the console; bit 1: every register access. */
    uint64_t trace;
    /* A normal finish does not end the run before this guest time. */
    uint64_t hold_us;
    uint64_t reserved[4];
};
const struct qemu_os_scenario *qemu_os_scenario(void);
void qemu_os_registers_init(void);
/* The watchdog that the stage-2 loader arms for the resident lifetime: a four-second
 * interval, restarted by a write of 0x14af to its control register. */
void qemu_os_watchdog_arm(void);
int qemu_os_watchdog_expired(void);
uint64_t qemu_os_mmio_read(uintptr_t address, unsigned bytes);
void qemu_os_mmio_write(uintptr_t address, unsigned bytes, uint64_t value);
/* Called by the wrapper that the image puts around the application's main. */
struct ss_grant;
void qemu_os_app_entry(const char *name, const struct ss_grant *grant);
#endif
