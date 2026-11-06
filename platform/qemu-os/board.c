/* Board description of the virtual eight-core OS board.
 *
 * The board stands in for what runs before the OS on the A733 and for the
 * hardware the OS image cannot find in QEMU:
 *
 *   - the stage-2 loader: it stamps the launch contract's times, admits it
 *     with the OS's own admission (apps/os/admission.c), seals the session
 *     (apps/board/session.c) and hands the OS the runtime record that the EL3
 *     monitor publishes (a733_cached_runtime);
 *   - the register-level providers (mmio.c): thermal sensors with scripted
 *     temperatures and timing, the S_TWI0 controller and the PMIC;
 *   - a fixed-state USB device transport;
 *   - the breadcrumb records of the returning images, which name the first
 *     board failure, and the end of the run (semihosting).
 *
 * The OS source runs unmodified; this file only supplies what it calls. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include <string.h>
#include <stdatomic.h>
#include "qemu_os.h"
#include "cached.h"
#include "sunstone_crumb.h"
#include "../../apps/board/session.h"
#include "../../apps/board/application.h"
#include "../../apps/os/os.h"
#include "../../apps/os/application.h"
#include "../../apps/os/cpu-return-el0.h"
TaskHandle_t os_application_main_task(void);
uint64_t os_sd_owner_entry_ttbr(void) __attribute__((weak));
extern uint64_t os_space_kernel_ttbr;
TaskHandle_t os_sd_owner_task(void) __attribute__((weak));   /* only an image with the SD owner has one */

/* Identity of the image. The session names it and the OS refuses a session
 * that does not (os_session_admitted). */
const unsigned char a733_smp_build_id[32] = {
    'q','e','m','u','-','o','s','-','b','o','a','r','d','-','v','1',
    0x53,0x75,0x6e,0x73,0x74,0x6f,0x6e,0x65,0x4f,0x53,0x01,0x02,0x03,0x04,0x05,0x06};
struct a733_cached_runtime a733_cached_runtime[8];

const struct qemu_os_scenario *qemu_os_scenario(void)
{
    const struct qemu_os_scenario *s = (const void *)(uintptr_t)QEMU_OS_SCENARIO_ADDRESS;
    if (s->magic != QEMU_OS_SCENARIO_MAGIC || s->version != QEMU_OS_SCENARIO_VERSION || s->bytes != sizeof(*s))
        platform_panic("scenario page missing", 0);
    return s;
}

/* ---- Console ------------------------------------------------------------ */
static atomic_flag console = ATOMIC_FLAG_INIT;
void qemu_os_log(const char *line)
{
    while (atomic_flag_test_and_set_explicit(&console, memory_order_acquire)) __asm__ volatile("yield");
    qemu_os_puts(line);
    atomic_flag_clear_explicit(&console, memory_order_release);
}
/* A line is built in the caller's buffer and written whole under the lock. */
struct line { char text[240]; unsigned used; };
static void put(struct line *l, const char *s) { while (*s && l->used < sizeof(l->text) - 2) l->text[l->used++] = *s++; }
static void put_dec(struct line *l, uint64_t v)
{
    char t[21]; unsigned i = sizeof(t);
    t[--i] = 0;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    put(l, t + i);
}
static void put_hex(struct line *l, uint64_t v)
{
    char t[17]; unsigned i = 16;
    t[16] = 0;
    do { t[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    put(l, "0x"); put(l, t + i);
}
static void flush(struct line *l) { l->text[l->used++] = '\n'; l->text[l->used] = 0; qemu_os_log(l->text); l->used = 0; }
static uint64_t now_us(void) { return a733_smp_counter() / 24; }
/* EL0 tasks cannot read TPIDR_EL1; the port keeps the core in a register they can. */
static unsigned core_id(void) { return (unsigned)portGET_CORE_ID(); }

/* ---- Breadcrumbs and the end of the run --------------------------------- */
/* The first board failure is the record the board keeps in the RTC words
 * (include/sunstone_crumb.h). On the A733 the failure stops the watchdog's
 * renewal and the board resets; here the record is printed and the run ends,
 * which is that reset. Printed from the supervisor call, so one core at a time
 * and with the lock the other cores use. */
static atomic_uint failed;
static atomic_uint_fast64_t failed_at;
#define FAILURE_GRACE_US 60000u
static const char *const groups[] = {"NONE", "BOARD", "THERMAL", "CPU_RETURN", "APPLICATION", "CPU_CLOCK",
    "PMIC", "GPU", "CPU_RETAINED", "CPU_PLL", "NPU"};
void qemu_os_crumb(uint64_t stage, uint64_t value) { (void)stage; (void)value; }
void qemu_os_crumb_fail(uint64_t site, uint64_t value, uint64_t caller)
{
    if (atomic_exchange(&failed, 1)) return;
    struct line l = {.used = 0};
    const unsigned group = (unsigned)(site >> 12) & 0xf;
    put(&l, "FAILURE-RECORD group="); put(&l, group < sizeof(groups) / sizeof(*groups) ? groups[group] : "UNKNOWN");
    put(&l, " detail="); put_hex(&l, site & 0xfff);
    put(&l, " site="); put_hex(&l, site & 0xffffffffu);
    put(&l, " value="); put_hex(&l, value & 0xffffffffu);
    put(&l, " caller="); put_hex(&l, caller);
    put(&l, " core="); put_dec(&l, core_id());
    put(&l, " t_us="); put_dec(&l, now_us());
    flush(&l);
    /* The board goes on until its watchdog resets it; the monitor ends the run
     * after a short interval, which lets a supervising task add its stop detail. */
    atomic_store(&failed_at, now_us() + 1);
}
int qemu_os_failed(void) { return atomic_load(&failed); }
/* The returning lifetime is not linked here, so no claim follows. The vector under test is
 * apps/os/cpu-return-el0.S: this function records what the OS image's function records
 * (apps/os/cpu-return.c) and suspends the task, which runs on the stack the vector gave it. */
_Noreturn void os_cpu_return_el0_fault(uint64_t esr, uint64_t far, uint64_t elr)
{
    sunstone_crumb_fail(os_cpu_return_el0_site(esr), os_cpu_return_el0_value(esr, far), (const void *)(uintptr_t)elr);
    for (;;) vTaskSuspend(NULL);
}

/* ---- Platform functions the OS image takes from the cached platform ----- */
int a733_smp_device_preparation_admitted(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return el == 4 && core_id() == 0 && xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED;
}
int a733_cpu_coherence_publish(uint64_t base, uint64_t bytes, uint64_t deadline)
{ (void)base; (void)bytes; (void)deadline; return 1; }
/* The virtual board has no cache to flush: it seals at EL1 and keeps the sealed session in .bss (apps/os/sealed.c). */
int a733_cpu_coherence_flush(uint64_t base, uint64_t bytes, uint64_t deadline)
{ (void)base; (void)bytes; (void)deadline; return 1; }
void *pvPortMalloc(size_t size) { (void)size; platform_panic("heap allocation", 0); }
void vPortFree(void *p) { (void)p; }

/* Shared peripheral interrupts: the interrupt controller of the A733 is not
 * here. The thermal sensor's interrupt is enabled and masked as a flag and
 * never fires; the sensors are read every 25 ms. */
#include "gicv3_spi.h"
static atomic_uint spi_enabled[1020 / 32 + 1];
int gicv3_spi_mask(const struct gicv3_spi *s)
{
    if (!s || s->intid < 32 || s->intid >= 1020) return 0;
    atomic_fetch_and(&spi_enabled[s->intid / 32], ~(1u << (s->intid % 32)));
    return 1;
}
int gicv3_spi_rearm(const struct gicv3_spi *s)
{
    if (!s || s->intid < 32 || s->intid >= 1020) return -1;
    atomic_fetch_or(&spi_enabled[s->intid / 32], 1u << (s->intid % 32));
    return 1;
}
int gicv3_spi_masked(const struct gicv3_spi *s)
{
    if (!s || s->intid < 32 || s->intid >= 1020) return -1;
    return atomic_load(&spi_enabled[s->intid / 32]) & (1u << (s->intid % 32)) ? -1 : 1;
}

/* ---- USB device transport ----------------------------------------------- */
/* The transport reports a fixed state: enumerated on the host, ready to carry
 * requests, a scenario-set delay after its task starts. It carries none. The real task
 * (apps/os/usb0.c) starts the controller, serves host requests and tells the
 * application that USB is ready; this one only tells it so. */
void board_usb0_task(void *unused)
{
    (void)unused;
    const uint64_t ready = now_us() + qemu_os_scenario()->usb_ready_us;
    while (now_us() < ready) vTaskDelay(1);
    /* A transport that hangs after it starts: it never tells the OS it is ready and never reports. */
    if (qemu_os_scenario()->fault == QEMU_OS_FAULT_USB_SILENT) for (;;) vTaskSuspend(NULL);
    os_application_usb_ready();
    const uint64_t stall = now_us() + 100000;
    for (;;) {
        BOARD_PROGRESS(BOARD_SERVICE_USB0);
        if (qemu_os_scenario()->fault == QEMU_OS_FAULT_USB_STALL && now_us() >= stall) for (;;) vTaskSuspend(NULL);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---- Application observation -------------------------------------------- */
static atomic_uint_fast64_t entered_at;
static atomic_uint entered;
void qemu_os_app_entry(const char *name, const struct ss_grant *grant)
{
    struct line l = {.used = 0};
    put(&l, "APP-MAIN name="); put(&l, name);
    put(&l, " core="); put_dec(&l, core_id());
    put(&l, " live="); put_dec(&l, (uint64_t)(int64_t)ss_live() & 0xff);
    put(&l, " throttled="); put_dec(&l, (uint64_t)ss_throttled());
    put(&l, " t_us="); put_dec(&l, now_us());
    put(&l, " regions="); put_dec(&l, grant->region_count);
    flush(&l);
    atomic_store(&entered_at, now_us());
    atomic_store(&entered, 1);
    /* A linger of one microsecond ends the run at the application's first instruction. */
    if (qemu_os_scenario()->entry_linger_us == 1) platform_exit(0);
}
/* EL0 task of the board, not of the OS: it watches the run and ends it. */
static void monitor(void *unused)
{
    (void)unused;
    const struct qemu_os_scenario *s = qemu_os_scenario();
    struct line l = {.used = 0};
    int cleared = 0;
    for (;;) {
        vTaskDelay(1);
        const uint64_t t = now_us();
        const uint64_t failure = atomic_load(&failed_at);
        if (failure && t >= failure + FAILURE_GRACE_US) {
            put(&l, "APP-TASK-STATE state="); put_dec(&l, (uint64_t)eTaskGetState((TaskHandle_t)ss_application.task));
            flush(&l);
            platform_exit(3);
        }
        if (qemu_os_watchdog_expired()) {
            put(&l, "WATCHDOG-RESET t_us="); put_dec(&l, t); flush(&l);
            platform_exit(5);
        }
        if (!cleared && !ss_throttled()) {
            cleared = 1;
            put(&l, "THROTTLE-CLEARED t_us="); put_dec(&l, t); flush(&l);
        }
        if (os_application_finished() && (cleared || t > s->limit_us) && t >= s->hold_us) {
            {   /* The address space of the application's tasks and of the OS task that its call started: the main task
                 * is tagged when the application has a space, the SD owner (created by the application's call, through
                 * the creator's tag) is not. */
                struct line s = {.used = 0};
                TaskHandle_t owner = os_sd_owner_task ? os_sd_owner_task() : 0;
                put(&s, "TASK-SPACE main="); put_dec(&s, xTaskGetApplicationTaskTag(os_application_main_task()) != 0);
                if (owner) {
                    put(&s, " sdowner="); put_dec(&s, xTaskGetApplicationTaskTag(owner) != 0);
                    put(&s, " sdentry="); put_dec(&s, os_sd_owner_entry_ttbr() != os_space_kernel_ttbr);   /* 1: it began in a space */
                }
                flush(&s);
            }
            put(&l, "APP-FINISHED t_us="); put_dec(&l, t);
            const struct ss_result *r = (const void *)ss_result_record;
            put(&l, " status="); put_dec(&l, r->status);
            put(&l, " payload_bytes="); put_dec(&l, r->payload_bytes);
            put(&l, " result=");
            for (uint64_t i = 0; i < r->payload_bytes && l.used < sizeof(l.text) - 4; ++i) {
                const uint8_t b = ((const uint8_t *)(r + 1))[i];
                l.text[l.used++] = "0123456789abcdef"[b >> 4];
                l.text[l.used++] = "0123456789abcdef"[b & 15];
            }
            flush(&l);
            /* A payload too long for the line above is repeated whole, 96 bytes to a line. */
            for (uint64_t at = 0; r->payload_bytes > 100 && at < r->payload_bytes; at += 96) {
                put(&l, "APP-RESULT offset="); put_dec(&l, at); put(&l, " hex=");
                for (uint64_t i = at; i < r->payload_bytes && i < at + 96; ++i) {
                    const uint8_t b = ((const uint8_t *)(r + 1))[i];
                    l.text[l.used++] = "0123456789abcdef"[b >> 4];
                    l.text[l.used++] = "0123456789abcdef"[b & 15];
                }
                flush(&l);
            }
            platform_exit(0);
        }
        if (atomic_load(&entered) && s->entry_linger_us && t >= atomic_load(&entered_at) + s->entry_linger_us) {
            put(&l, "RUN-ENDED after entry t_us="); put_dec(&l, t); put(&l, " throttled="); put_dec(&l, (uint64_t)ss_throttled());
            flush(&l);
            platform_exit(0);
        }
        if (t > s->limit_us) {
            put(&l, "TIMEOUT t_us="); put_dec(&l, t); put(&l, " entered="); put_dec(&l, (uint64_t)atomic_load(&entered));
            put(&l, " finished="); put_dec(&l, (uint64_t)os_application_finished());
            flush(&l);
            platform_exit(4);
        }
    }
}
static StaticTask_t monitor_task;
static StackType_t monitor_stack[2048];

/* ---- Start -------------------------------------------------------------- */
extern void a733_smp_diagnostic_start(void);
void kernel_main(void)
{
    platform_init();
    qemu_os_registers_init();
    /* The stage-2 loader: stamp the contract, then admit and seal the session. */
    struct a733_smp_contract c;
    memcpy(&c, (const void *)(uintptr_t)QEMU_OS_CONTRACT_ADDRESS, sizeof(c));
    const uint64_t start = a733_smp_counter();
    c.armed_at = start;
    c.startup_deadline = start + A733_BOARD_STARTUP_SECONDS * A733_SMP_COUNTER_HZ;
    if (qemu_os_scenario()->lifetime == A733_LIFETIME_RESIDENT) {
        /* The resident loader arms the four-second watchdog; no diagnostic deadline, no FEL return. */
        c.diagnostic_deadline = A733_SMP_NO_DEADLINE;
        c.expires_at = start + 4 * A733_SMP_COUNTER_HZ;
        c.watchdog_mode = 0x41; c.watchdog_configuration = 1; c.watchdog_output = 2;
        c.magic = A733_OS_RESIDENT_CONTRACT_MAGIC;
        qemu_os_watchdog_arm();
    } else {
        c.diagnostic_deadline = start + A733_BOARD_SUPERVISED_SECONDS * A733_SMP_COUNTER_HZ;
        c.expires_at = c.diagnostic_deadline;
        c.magic = A733_OS_CONTRACT_MAGIC;
    }
    if (!a733_smp_contract_valid(&c, a733_smp_counter())) platform_panic("launch contract refused", 1);
    if (!board_session_prepare(&c)) platform_panic("session seal refused", 2);
    a733_cached_runtime[0] = (struct a733_cached_runtime){.epoch = c.epoch, .startup_deadline = c.startup_deadline,
        .diagnostic_deadline = c.diagnostic_deadline, .redistributor = 0x080a0000};
    struct line l = {.used = 0};
    put(&l, "BOOT board=qemu-os cores="); put_dec(&l, configNUMBER_OF_CORES);
    put(&l, " epoch="); put_dec(&l, c.epoch); put(&l, " workload="); put_dec(&l, board_session_snapshot()->workload);
    put(&l, " t_us="); put_dec(&l, now_us());
    flush(&l);
    configASSERT(xTaskCreateStaticAffinitySet(monitor, "board-monitor", 2048, NULL, 1, monitor_stack,
        &monitor_task, 1u << 1));
    a733_smp_diagnostic_start();
}

#ifdef QEMU_OS_UPPER
#include "app_large.h"
#ifndef QEMU_OS_SD_OWNER             /* an image with the SD owner has the owner's device, not this RAM disk */
/* The virtual board's block device: a 40 MiB RAM disk that the check loads at 0x48000000 (a bare FAT32 volume). */
#define LARGE_DISK ((const uint8_t *)(uintptr_t)0x48000000)
#define LARGE_DISK_SECTORS 81920u
static uint8_t large_buffers[3 * OS_LARGE_BUFFER_BYTES] __attribute__((aligned(64)));
static unsigned large_polls;
static int large_read(void *unused, uint64_t sector, uint32_t count, uint8_t *buffer)
{
    (void)unused;
    if (!count || sector + count > LARGE_DISK_SECTORS) return 0;
    if (qemu_os_scenario()->fault == QEMU_OS_FAULT_DISK_MOUNT && sector < 64) return 0;
    memcpy(buffer, LARGE_DISK + sector * 512u, (size_t)count * 512u);
    return 1;
}
static unsigned large_requests;
static int large_submit(void *unused, uint64_t sector, uint32_t count, uint8_t *buffer)
{
    large_polls = 0;
    ++large_requests;
    if (qemu_os_scenario()->fault == QEMU_OS_FAULT_DISK_SUBMIT) return 0;
    return large_read(unused, sector, count, buffer);
}
/* The request completes on the second poll: the loader's busy path runs. */
static int large_poll(void *unused)
{
    (void)unused;
    if (qemu_os_scenario()->fault == QEMU_OS_FAULT_DISK_MIDWAY && large_requests == 5) return -1;
    return ++large_polls >= 2;
}
const struct os_large_device *os_large_device(void)
{
    if (qemu_os_scenario()->fault == QEMU_OS_FAULT_DISK_ABSENT) return 0;
    static const struct os_large_device device = {{0, large_read, LARGE_DISK_SECTORS}, {0, large_submit, large_poll},
                                                  0, LARGE_DISK_SECTORS, large_buffers};
    return &device;
}
#endif
void os_large_report(const struct large_slot *slot, const struct app_space *space)
{
    struct line l = {.used = 0};
    put(&l, "LARGE-ADMITTED index="); put_dec(&l, slot->index);
    put(&l, " base="); put_hex(&l, slot->base);
    put(&l, " bytes="); put_hex(&l, slot->bytes);
    put(&l, " flags="); put_dec(&l, slot->flags);
    put(&l, " state="); put_dec(&l, slot->state);
    put(&l, " tables="); put_dec(&l, space->tables);
    put(&l, " asid="); put_dec(&l, space->asid);
    flush(&l);
}
#endif
