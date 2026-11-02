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

/* ---- Platform functions the OS image takes from the cached platform ----- */
int a733_smp_device_preparation_admitted(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return el == 4 && core_id() == 0 && xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED;
}
int a733_cpu_coherence_publish(uint64_t base, uint64_t bytes, uint64_t deadline)
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
#ifdef SUNSTONE_BOARD_RESIDENT
        board_service_progress(BOARD_SERVICE_USB0);
#endif
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
        if (failure && t >= failure + FAILURE_GRACE_US) platform_exit(3);
        if (qemu_os_watchdog_expired()) {
            put(&l, "WATCHDOG-RESET t_us="); put_dec(&l, t); flush(&l);
            platform_exit(5);
        }
        if (!cleared && !ss_throttled()) {
            cleared = 1;
            put(&l, "THROTTLE-CLEARED t_us="); put_dec(&l, t); flush(&l);
        }
        if (os_application_finished() && (cleared || t > s->limit_us) && t >= s->hold_us) {
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
#ifdef SUNSTONE_BOARD_RESIDENT
    /* The resident loader arms the four-second watchdog; no diagnostic deadline, no FEL return. */
    c.diagnostic_deadline = A733_SMP_NO_DEADLINE;
    c.expires_at = start + 4 * A733_SMP_COUNTER_HZ;
    c.watchdog_mode = 0x41; c.watchdog_configuration = 1; c.watchdog_output = 2;
    qemu_os_watchdog_arm();
#else
    c.diagnostic_deadline = start + A733_BOARD_SUPERVISED_SECONDS * A733_SMP_COUNTER_HZ;
    c.expires_at = c.diagnostic_deadline;
#endif
    c.magic = A733_SMP_CONTRACT_MAGIC;
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
