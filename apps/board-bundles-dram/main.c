/* SPDX-License-Identifier: Apache-2.0 */
/* Bundle 8 of docs/designs/board-bundles-next.md: DRAM bus priority on the
 * returning single-core platform of rank-9-usb-v4.
 *
 * One task checks the mappings, reads the gates (R_CCU 0x1ac before the
 * domain 1 status, then CCU +0x580, +0x584 and +0x588) and, only when domain
 * 1 reads ON and the NSI gate and reset are open, makes the first NSI
 * accesses: the fields of ports 2, 14 and 15, the USB_PCIE port fields, the
 * table write and readback of ports 2, 14 and 15 through
 * a733_dram_priority(), and the USB_PCIE fields again. Every access is one
 * DSB SY-fenced 32-bit access timed with CNTPCT.
 *
 * Outcomes: status 0 with "COMPLETE board bundle 8: table written", or with
 * "COMPLETE board bundle 8: inconclusive" and the skipped stage when a gate is
 * closed or domain 1 is not ON (no NSI access). Status 1 with one FAIL
 * exception line when an NSI access takes a synchronous external abort
 * (inside bb8_access), or with "FAIL board bundle 8: NSI readback mismatch";
 * both are controlled outcomes of the bundle. Any other status 1 is not. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "sequence.h"

#define TRACE_ENTRIES 64u
static StaticTask_t task;
static StackType_t stack[4096];
static struct bb8_record record;
static struct trace { uint32_t address, value, write, ticks; uint64_t start; } trace[TRACE_ENTRIES];
static unsigned traced, nsi_started;

static void put(const char *name, uint64_t value)
{ platform_puts(name); platform_hex(value); platform_puts("\n"); }

/* Every register access of the bundle. An NSI access that aborts ends here,
 * so the controlled abort outcomes name this function's address range. */
__attribute__((noinline, used)) uint32_t bb8_access(uintptr_t address, uint32_t value, unsigned write)
{
    if (write) {
        a733_mmio_write32_dsb_sy_both(address, value);
        return value;
    }
    return a733_mmio_read32_dsb_sy_both(address);
}

static int timed(uintptr_t address, uint32_t value, unsigned write, uint32_t *out)
{
    if (traced >= TRACE_ENTRIES)
        return 0;
    const uint64_t start = counter();
    const uint32_t v = bb8_access(address, value, write);
    const uint64_t end = counter();
    trace[traced++] = (struct trace){(uint32_t)address, v, write, (uint32_t)(end - start), start};
    if (out)
        *out = v;
    return 1;
}
static int io_read(void *context, uintptr_t address, uint32_t *out)
{ (void)context; return timed(address, 0, 0, out); }
static int io_write(void *context, uintptr_t address, uint32_t value)
{ (void)context; return timed(address, value, 1, NULL); }

/* CCU, R_CCU and the PPU page of domain 1 are read; the two NSI pages that
 * hold ports 2, 13, 14 and 15 are read and written. Each must be an identity
 * Device-nGnRnE mapping. */
static int mapped(void)
{
    static const struct a733_mapping_rule read_rule = {A733_MAPPING_READ, 1, {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    static const struct a733_mapping_rule write_rule = {A733_MAPPING_READ_WRITE, 1, {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 1, 1};
    static const uintptr_t read[] = {0x02002000, 0x07010000, 0x07061000}, written[] = {0x02020000, 0x02021000};
    for (unsigned i = 0; i < 3; ++i)
        if (!a733_mapping_check(aarch64_device_translate, read[i], read[i], &read_rule))
            return 0;
    for (unsigned i = 0; i < 2; ++i)
        if (!a733_mapping_check(aarch64_device_translate, written[i], written[i], &write_rule))
            return 0;
    return 1;
}

static void report(void)
{
    put("R_CCU_PPU_CLOCK=", record.ppu_clock);
    if (record.domain1_read)
        put("DOMAIN1_STATUS=", record.domain1_status);
    put("NSI_CLK=", record.nsi_clock);
    put("NSI_BGR=", record.nsi_config);
    put("MBUS_CLK=", record.mbus_clock);
    if (nsi_started) {
        for (unsigned i = 0; i < BB8_FIELDS; ++i) {
            put("INHERITED address=", bb8_field(i));
            put("INHERITED word=", record.inherited[i]);
        }
        put("POSSIBLY_PROTECTED=", record.possibly_protected);
    }
    for (unsigned i = 0; i < traced; ++i) {
        put(trace[i].write ? "ACCESS write=" : "ACCESS read=", trace[i].address);
        put("  value=", trace[i].value);
        put("  start=", trace[i].start - trace[0].start);
        put("  ticks=", trace[i].ticks);
    }
}

static _Noreturn void stop(const char *text, unsigned status)
{
    report();
    platform_puts(text);
    platform_puts("\n");
    platform_exit(status);
}

static void bundle(void *unused)
{
    (void)unused;
    platform_puts("BEGIN board bundle 8: DRAM bus priority\n");
    if (!mapped())
        stop("FAIL board bundle 8: register windows are not Device-nGnRnE identity mappings", 1);
    const struct a733_dram_io io = {NULL, io_read, io_write};
    const int ready = bb8_gates(&io, &record);
    if (ready < 0)
        stop("FAIL board bundle 8: gate read refused", 1);
    if (!ready) {
        static const char *const stage[] = {"", "PPU clock", "domain 1 status", "NSI clock", "NSI configuration"};
        put("SKIPPED_STAGE=", record.skipped);
        platform_puts("SKIPPED ");
        platform_puts(stage[record.skipped]);
        platform_puts("\n");
        stop("COMPLETE board bundle 8: inconclusive, no NSI access", 0);
    }
    platform_puts("BEGIN board bundle 8 NSI access\n");
    nsi_started = 1;
    const enum bb8_result result = bb8_nsi(&io, &record);
    if (result != BB8_ACCESS_FAILED && result != BB8_GATE_CHANGED) {
        for (unsigned i = 0; i < 3; ++i) {
            put("USB_PCIE address=", bb8_usb_pcie(i));
            put("USB_PCIE before=", record.usb_pcie_before[i]);
            if (result != BB8_MISMATCH)
                put("USB_PCIE after=", record.usb_pcie_after[i]);
        }
        put("HOOK_RESULT=", record.block.result);
        put("HOOK_STOPPING_FIELD=", record.block.failed);
        for (unsigned i = 0; i < A733_DRAM_PORTS; ++i) {
            put("PORT_FIELDS inherited=", record.block.inherited[i]);
            put("PORT_FIELDS final=", record.block.final[i]);
        }
    }
    switch (result) {
    case BB8_WRITTEN: stop("COMPLETE board bundle 8: table written", 0);
    case BB8_MISMATCH: stop("FAIL board bundle 8: NSI readback mismatch", 1);
    case BB8_USB_PCIE_CHANGED: stop("FAIL board bundle 8: USB_PCIE port field changed", 1);
    case BB8_GATE_CHANGED: stop("FAIL board bundle 8: NSI gate closed after the field reads", 1);
    default: stop("FAIL board bundle 8: NSI access refused", 1);
    }
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{
    (void)handle;
    platform_panic(name, 0);
}

void kernel_main(void)
{
    platform_init();
    configASSERT(xTaskCreateStatic(bundle, "bundle8", 4096, NULL, 2, stack, &task) != NULL);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
