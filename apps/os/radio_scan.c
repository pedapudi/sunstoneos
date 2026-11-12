/* The scanning radio station's end: shutdown on return, the published result and the status call
 * (docs/designs/radio-service-stages.md). Everything here runs in the radio owner task, except the
 * two atomics the application and the return supervisor read. */
#include "os.h"
#include "../board/application.h"
#include "cpu-return.h"
#include "radio-quiesce.h"
#include "a733_radio_result.h"
#include "os_radio_result.h"
#include "sunstone_crumb.h"
#include <string.h>

/* The host reads this symbol after the return (tools/radio_result_channel.py). It lives in BSS, zero
 * until the record is published. */
struct os_radio_result_descriptor os_radio_result_descriptor __attribute__((used,retain,aligned(8)));
enum { RUNNING, SCANNED, DOWN, FAILED };
static _Atomic unsigned state, returning, stage_seen;
static unsigned shutdown_started, published;
/* The shutdown has this long to run: every step is a bounded register or PMIC transaction. */
#define OS_RADIO_SHUTDOWN_US 3000000u

static void clean(const void *base, size_t bytes)
{
    for (uintptr_t a = (uintptr_t)base & ~(uintptr_t)63; a < (uintptr_t)base + bytes; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
/* One heap block sized to the encoded record: the scan list decides its length. The host reads the
 * block from DRAM, so the lines are cleaned before the descriptor that names them. */
static void publish(struct board_radio *r)
{
    const struct a733_radio_report *report = board_radio_report(r);
    if (published || !report) return;
    published = 1;
    size_t bytes = a733_radio_report_bytes(report);
    uint8_t *block = pvPortMalloc(bytes);
    if (!block || a733_radio_report_encode(report, block, bytes) != bytes) return;
    clean(block, bytes);
    os_radio_result_descriptor.address = (uint64_t)(uintptr_t)block;
    os_radio_result_descriptor.bytes = bytes;
    os_radio_result_descriptor.magic = OS_RADIO_RESULT_MAGIC;
    clean(&os_radio_result_descriptor, sizeof(os_radio_result_descriptor));
}
void os_radio_scan_step(struct board_radio *r, int result)
{
    unsigned current = atomic_load_explicit(&state, memory_order_acquire);
    if (r->config.entropy_only) {
        /* The CE entropy stage replaces the radio's own work: no platform poll, no shutdown to do. */
        if (current == RUNNING) {
            os_ce_probe_start();
            os_ce_probe_step();
            if (os_ce_probe_done()) atomic_store_explicit(&state, os_ce_probe_failed() ? FAILED : DOWN, memory_order_release);
        }
        return;
    }
    const struct a733_radio_report *seen = board_radio_report(r);
    if (seen) atomic_store_explicit(&stage_seen, seen->head.stage, memory_order_release);
    if (result == BOARD_RADIO_SCANNED && current == RUNNING) {
        atomic_store_explicit(&state, SCANNED, memory_order_release);
        current = SCANNED;
    }
    if (result == BOARD_RADIO_SCANNED && current == SCANNED && !shutdown_started &&
        atomic_load_explicit(&returning, memory_order_acquire)) {
        uint64_t now = a733_smp_counter() / 24;
        shutdown_started = 1;
        /* Refused (a busy transport) leaves the station scanned; the retry is the next pass. */
        if (!board_radio_shutdown(r, now + OS_RADIO_SHUTDOWN_US)) shutdown_started = 0;
    } else if (result == BOARD_RADIO_DOWN && current != DOWN) {
        publish(r);
        atomic_store_explicit(&state, DOWN, memory_order_release);
    } else if ((result == BOARD_RADIO_RETAINED || result == BOARD_RADIO_INVALID) && current != FAILED) {
        /* The first failure record names where the platform failed (source line and stage), so a run that
         * cannot return still says it; the descriptor's record carries the same site. */
        uint32_t value = r->platform.fail_line ? r->platform.fail_line << 16 | (r->platform.fail_stage & 0xffffu) :
            r->session.fail_line << 16 | 0x100u | (r->session.fail_stage & 0xfeffu);
        if(!os_radio_quiesce_fault(value,__builtin_return_address(0)))
            board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(BOARD) | SUNSTONE_CRUMB_FAIL_RETURN,
                value,__builtin_return_address(0));
        publish(r);
        atomic_store_explicit(&state, FAILED, memory_order_release);
    }
}
void os_radio_scan_return_request(void){atomic_store_explicit(&returning, 1, memory_order_release);}
/* A published failure does not establish that the module stopped. */
int os_radio_scan_return_ready(void)
{
    unsigned s = atomic_load_explicit(&state, memory_order_acquire);
    return s == DOWN;
}
void ss_radio_status(struct ss_radio_status *status)
{
    unsigned s = atomic_load_explicit(&state, memory_order_acquire);
    *status = (struct ss_radio_status){0, s == FAILED, s != RUNNING, 0};
    status->stage = atomic_load_explicit(&stage_seen, memory_order_acquire);
}
