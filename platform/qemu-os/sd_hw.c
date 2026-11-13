/* The virtual board's SD0 hardware operations for the write-capable OS image (apps/os/sd_hw.h), so that checks/qemu-os-boot.py runs
 * apps/os/sd_owner.c and the storage-write application through ss_storage_enable on the host.
 *
 * The card is a RAM disk that the check loads at 0x48000000: sector n of the card is at offset 512 n, laid out as on the real card
 * (the backup sectors at their numbers). enable_writes does what apps/os/large_sd.c does, with the same bounds object: copy the
 * runner's blob, parse it, re-read the listed backup sectors and compare their hash. It does NOT bind the native register filter
 * (a733_mmc_platform_native_bind_bounds needs the controller): the binding is the one step this board cannot model, and write_piece
 * stands in for it with a RAM copy limited by a733_storage_bounds_allows. */
#include "sd_hw.h"
#include "a733_storage_bounds.h"
#include <string.h>
#include <stdatomic.h>
#include "FreeRTOS.h"
#include "task.h"
#include "qemu_os.h"
#include "sunstone_deadline.h"

#include "a733_platform_locks.h"
#include "../../apps/board/application.h"
void os_application_usb_changed(void);               /* apps/os/application.c: xTaskNotifyGive(main_task), a real default-index hint */

#define DISK ((uint8_t *)(uintptr_t)0x48000000)
#define DISK_SECTORS 65536u
#define CARD_SECTORS UINT64_C(0x7a00000)

uint64_t os_sd_mode[5];                              /* as in apps/os/large_sd.c: the bus speed mode (the QEMU card has none) */
uint64_t os_sd_diag[4];                              /* as in apps/os/large_sd.c: stage, detail, first failed read + 1, reads */
uint32_t os_sd_bank_first[3], os_sd_bank_reopen[3];   /* as in apps/os/large_sd.c: the PF bank records; the QEMU card has no PF bank, so zero */
uint8_t os_sd_pf_open, os_sd_pf_closed;
#define DIAG(stage, detail) (os_sd_diag[0] = (stage), os_sd_diag[1] = (uint64_t)(detail), 0)

static struct a733_storage_bounds bounds;
static uint8_t blob_copy[65536];
static uint8_t *write_region;
static size_t write_region_bytes;
static uint8_t buffers[3 * OS_LARGE_BUFFER_BYTES] __attribute__((aligned(64)));
static unsigned polls;

static int ram_read(void *unused, uint64_t sector, uint32_t count, uint8_t *out)
{
    (void)unused;
    if (!count || sector + count > DISK_SECTORS) return 0;
    memcpy(out, DISK + sector * 512u, (size_t)count * 512u);
    return 1;
}
static int ram_submit(void *unused, uint64_t sector, uint32_t count, uint8_t *out) { polls = 0; return ram_read(unused, sector, count, out); }
static int ram_poll(void *unused) { (void)unused; return ++polls >= 2; }
static int bounds_read(void *unused, uint64_t sector, uint8_t *out)
{
    ++os_sd_diag[3];
    int ok = ram_read(unused, sector, 1, out);
    if (!ok && !os_sd_diag[2]) os_sd_diag[2] = sector + 1;
    return ok;
}

static int allocation_unit(uint32_t *sectors) { *sectors = 8192; return 1; }
static int enable_writes(const uint8_t *blob, size_t bytes)
{
    if (qemu_os_scenario()->fault == QEMU_OS_FAULT_LOCK_RETAINED) {
        /* A failed controller transaction retains the shared register lock (a733_mmc_platform_retain): no task can reach registers
         * again, the thermal service among them. The enable goes on waiting, as a stuck owner would. */
        if (a733_platform_lock_enter(&providers.shared.locks, A733_PLATFORM_REGISTER_LOCK, sunstone_deadline_now_us() + 1000000) != 1 ||
            a733_platform_lock_retain(&providers.shared.locks, A733_PLATFORM_REGISTER_LOCK) != 1) return 0;
        vTaskDelay(pdMS_TO_TICKS(9000));
    }
    if (qemu_os_scenario()->fault == QEMU_OS_FAULT_CLOCKS_FAILED) {
        atomic_store_explicit(&providers.shared.clocks.failed, 1, memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    if (!blob || !bytes) return DIAG(20, 0);
    if (bytes > sizeof blob_copy) return DIAG(21, bytes);
    memcpy(blob_copy, blob, bytes);
    if (!a733_storage_bounds_parse(&bounds, blob_copy, bytes)) return DIAG(22, bytes);
    os_sd_diag[3] = 0;
    if (!a733_storage_bounds_verify(&bounds, bounds_read, NULL)) return DIAG(23, bounds.state);
    return 1;                                         /* stage 24, the native binding, is not modeled */
}
static int write_piece(uint64_t sector, uint32_t count, const uint8_t *src)
{
    if (!count || count > OS_SD_WRITE_PIECE_SECTORS || sector + count > DISK_SECTORS || !a733_storage_bounds_allows(&bounds, sector, count)) return 0;
    memcpy(DISK + sector * 512u, src, (size_t)count * 512u);
    return 1;
}
static int live(void) { return 1; }
static int set_write_region(uint8_t *base, size_t bytes)
{
    if (write_region || !base || ((uintptr_t)base & 63u) || bytes < 4096 || (bytes & 511u)) return 0;
    write_region = base; write_region_bytes = bytes;
    return 1;
}
static int wait_async(void) { return 1; }
static uint64_t owner_sleep_us(void) { return 0; }
static uint64_t owner_wakes(void) { return 0; }
static void irq_counts(uint32_t entries[8], uint32_t *stray, uint32_t *recheck) { for (unsigned c = 0; c < 8; ++c) entries[c] = 0; *stray = 0; *recheck = 0; }
static atomic_uint closed;
void os_sd_close_owner(void) { atomic_store_explicit(&closed,1,memory_order_release); }
static int hw_closed(void) { return atomic_load_explicit(&closed,memory_order_acquire)!=0; }

static struct os_large_device device;
static struct os_sd_hw hw;
const struct os_sd_hw *os_sd_open_owner(void)
{
    /* The real open takes the PMIC, the rail and identification: the application waits for it. A hint on its default notification
     * index (a USB attachment change, the USB-ready event, a deadline hint) may arrive meanwhile and must not end the wait. */
    if (qemu_os_scenario()->fault == QEMU_OS_FAULT_STRAY_NOTIFY) {
        vTaskDelay(pdMS_TO_TICKS(10));
        /* The OS gives index 0 to the application only inside its own waits (docs/notifications.md), so the hint comes
         * from another source of notifications to the application task, as a device hint or a task of the application. */
        extern TaskHandle_t os_application_main_task(void);
        xTaskNotifyGive(os_application_main_task());
        vTaskDelay(pdMS_TO_TICKS(40));
    } else {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (qemu_os_scenario()->fault == QEMU_OS_FAULT_OPEN_FAILS) { (void)DIAG(7, 0x0903); return NULL; }
    device = (struct os_large_device){{0, ram_read, CARD_SECTORS}, {0, ram_submit, ram_poll}, 0, 0, buffers};
    hw = (struct os_sd_hw){&device, CARD_SECTORS, 1, allocation_unit, enable_writes, write_piece, live, set_write_region, wait_async,
                           owner_sleep_us, owner_wakes, irq_counts, os_sd_close_owner, hw_closed};
    return &hw;
}

/* The boot-time CPU observation snapshot is not published on this board, as in the first board runs of the A76 return image
 * (the call returned 0 for core 6): the application then takes its C SHA-256 path. */
#include "sunstone_app.h"
int ss_cpu_observations(unsigned core, struct ss_cpu_observation *out) { (void)core; (void)out; return 0; }
