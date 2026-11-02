/* The single SD0 owner task of a write-capable OS image (OS_SD_WRITE; docs/designs/storage-write.md).
 *
 * The native adapter admits register access only from the task that opened the card, so one task, the owner, performs
 * every card operation. It serves two mailboxes, each holding at most one request: the loader's reads (high priority)
 * and the writer's reads and write pieces (low priority). A write piece is at most OS_SD_WRITE_PIECE_SECTORS sectors, and
 * the owner looks at the loader's mailbox between pieces, so a model-loading read waits for at most the piece in flight.
 * Callers sleep on a task notification until the owner finishes their request; the owner sleeps on one until a request
 * arrives. Nothing polls on the tick: the card itself is polled inside a request, as in apps/os/large_sd.c.
 *
 * os_large_device() here replaces the read-only binding's entry for the loader (large_sd.c names its own
 * os_sd_open_owner_device in this build) and returns a proxy whose requests the owner runs. */
#include "FreeRTOS.h"
#include "task.h"
#include "sd_hw.h"
#include "../board/application.h"
#include "sunstone_crumb.h"
#include "fat32_rw.h"
#include "sunstone_deadline.h"
#include "sunstone_storage.h"
#include <stdatomic.h>
#include <string.h>

#define OWNER_STACK_WORDS 2048u
#define OWNER_PRIORITY (tskIDLE_PRIORITY + 2)
#define READY_TIMEOUT_US 10000000u
#define WAIT_INDEX 1u              /* notification index a caller sleeps on; index 0 is the owner's */

enum kind { K_READ, K_ASYNC_READ, K_WRITE, K_ENABLE, K_UNIT, K_REGION, K_CLOSE };
struct request {
    enum kind kind;
    uint64_t sector;
    uint32_t count;
    uint8_t *buffer;
    const uint8_t *source;
    size_t bytes;
    TaskHandle_t waiter;
    uint32_t value;
    atomic_int done;               /* 0 pending, 1 succeeded, -1 failed */
};
static _Atomic(struct request *) loader_slot, writer_slot;
static TaskHandle_t owner, ready_waiter;
static StaticTask_t owner_tcb;
static StackType_t owner_stack[OWNER_STACK_WORDS];
static const struct os_sd_hw *hw;
static atomic_int ready, closed;           /* 0 starting, 1 open, -1 failed */
static uint32_t unit_sectors;
static _Atomic uint64_t idle_us, wake_count, first_us;
static _Atomic uint32_t owner_core_mask, caller_core_mask;
static struct os_large_device proxy;
extern uint64_t os_sd_diag[4];

static void finish(struct request *r, int ok)
{
    TaskHandle_t waiter = r->waiter;
    atomic_store_explicit(&r->done, ok ? 1 : -1, memory_order_release);
    if (waiter) xTaskNotifyGiveIndexed(waiter, WAIT_INDEX);
}

/* The note for a refused request or a failed open (the first-failure record's value): bits 7:0 the stage, 23:8 its detail (the low 16 bits:
 * result | platform stage << 8), 26:24 INTID 193 before the owner masked it (enabled, pending, active), 30:28 after the mask settled. */
static uint32_t diag_word(void)
{
    if (os_sd_diag[0] >= 30)                         /* a refused request: stage in 5:0, then the 26 bits of large_sd.c PLATX */
        return (uint32_t)(os_sd_diag[0] & 0x3f) | (uint32_t)(os_sd_diag[1] & 0x3ffffff) << 6;
    return (uint32_t)(os_sd_diag[0] & 0xff) | (uint32_t)(os_sd_diag[1] & 0xffff) << 8 | (uint32_t)(os_sd_diag[2] & 7) << 24 |
           (uint32_t)(os_sd_diag[3] & 7) << 28;
}
static void serve(struct request *r)
{
    const struct os_large_device *d = hw->device;
    int ok = 0;
    switch (r->kind) {
    case K_READ: ok = d->sync.read(d->sync.context, r->sector, r->count, r->buffer) == 1; break;
    case K_ASYNC_READ:
        /* The loader's pipelined request: start it, sleep until it finishes, and leave the verdict in `done`. */
        ok = d->async.submit(d->async.context, r->sector, r->count, r->buffer) == 1 && hw->wait_async();
        break;
    case K_WRITE: ok = hw->write_piece(r->sector, r->count, r->source); break;
    case K_ENABLE: ok = hw->enable_writes(r->source, r->bytes); break;
    case K_UNIT: ok = hw->allocation_unit(&r->value); break;
    case K_REGION: ok = hw->set_write_region(r->buffer, r->bytes); break;
    case K_CLOSE: hw->close(); atomic_store(&closed, 1); ok = 1; break;
    }
    /* A refused request leaves its stage as a note, except the enable refusals (stages 20 to 24: preconditions, parse, verify mismatch, binding), which are results the
     * application reports. The first failure record names the SD0 binding when it came first, and a thermal or
     * watchdog stop that follows the controller's retention cannot hide it. */
    if (!ok && !(os_sd_diag[0] >= 20 && os_sd_diag[0] <= 24)) board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(APPLICATION), diag_word(), SUNSTONE_CRUMB_HERE());
    /* A failed async read reports -1 from poll; the proxy's poll reads the stored result. */
    finish(r, ok);
}

static void owner_main(void *unused)
{
    (void)unused;
    hw = os_sd_open_owner();
    atomic_store(&first_us, sunstone_deadline_now_us());
    atomic_store(&ready, hw ? 1 : -1);
    if (ready_waiter) xTaskNotifyGiveIndexed(ready_waiter, WAIT_INDEX);
    if (!hw) {
        /* A failed open leaves the controller's state uncertain and the platform register lock retained, which the thermal service
         * then reports as its own failure. Note the open's stage first so that the record names the SD0 binding (the diagnostic
         * word: stage in bits 7:0, its detail above). */
        board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(APPLICATION), diag_word(), SUNSTONE_CRUMB_HERE());
        vTaskSuspend(NULL);
    }
    for (;;) {
        uint64_t slept_from = sunstone_deadline_now_us();
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        atomic_fetch_add(&idle_us, sunstone_deadline_now_us() - slept_from);
        atomic_fetch_add(&wake_count, 1);
        atomic_fetch_or(&owner_core_mask, 1u << portGET_CORE_ID());
        for (;;) {
            struct request *r = atomic_exchange(&loader_slot, NULL);        /* reads first */
            if (r) { serve(r); continue; }
            r = atomic_exchange(&writer_slot, NULL);                        /* then one write-side piece */
            if (r) { serve(r); continue; }
            break;
        }
    }
}

static int post(_Atomic(struct request *) *slot, struct request *r, int wait)
{
    if (atomic_load(&closed)) return 0;                                 /* the card is closed: no request is served */
    atomic_fetch_or(&caller_core_mask, 1u << portGET_CORE_ID());
    atomic_init(&r->done, 0);
    r->waiter = wait ? xTaskGetCurrentTaskHandle() : NULL;
    struct request *none = NULL;
    if (!atomic_compare_exchange_strong(slot, &none, r)) return 0;        /* one request per mailbox */
    xTaskNotifyGive(owner);
    if (!wait) return 1;
    while (!atomic_load_explicit(&r->done, memory_order_acquire)) ulTaskNotifyTakeIndexed(WAIT_INDEX, pdTRUE, portMAX_DELAY);
    return atomic_load(&r->done) == 1;
}

/* The owner reads the CCU and drives SMHC0, so the scheduler creates it at EL1 (board_application_task_privileged). The first
 * caller is the application's EL0 task, which would otherwise pass its own level on to the task it creates. */
int os_sd_owner_task_privileged(void (*entry)(void *), void *context) { (void)context; return entry == owner_main; }

/* Start the owner task; the caller is told when the card is open (os_large_device) or has failed. */
static int start(void)
{
    if (owner) return atomic_load(&ready) == 1;
    ready_waiter = xTaskGetCurrentTaskHandle();
    owner = xTaskCreateStatic(owner_main, "sd-owner", OWNER_STACK_WORDS, NULL, OWNER_PRIORITY, owner_stack, &owner_tcb);
    if (!owner) { os_sd_diag[0] = 10; return 0; }
    /* The owner gives the notification once the card is open or has failed. A wake is only a hint (any other notification would end a
     * single wait early), so wait on the state, on the caller's own index, until a time bound measured in microseconds. */
    uint64_t t0 = sunstone_deadline_now_us(), ticks0 = xTaskGetTickCount();
    while (atomic_load(&ready) == 0 && sunstone_deadline_now_us() - t0 < READY_TIMEOUT_US)
        (void)ulTaskNotifyTakeIndexed(WAIT_INDEX, pdTRUE, pdMS_TO_TICKS(50));
    ready_waiter = NULL;
    if (atomic_load(&ready) != 1 && !os_sd_diag[0]) {                         /* no answer from the owner in time */
        os_sd_diag[0] = 11; os_sd_diag[1] = sunstone_deadline_now_us() - t0;
        os_sd_diag[2] = (uint64_t)atomic_load(&ready) & 0xffffffffu; os_sd_diag[3] = xTaskGetTickCount() - ticks0;
    }
    return atomic_load(&ready) == 1;
}

/* --- the loader's view -------------------------------------------------------------------------------------------- */
static struct request loader_sync, loader_async;
static int proxy_read(void *c, uint64_t sector, uint32_t count, uint8_t *out)
{
    (void)c;
    loader_sync = (struct request){.kind = K_READ, .sector = sector, .count = count, .buffer = out};
    return post(&loader_slot, &loader_sync, 1);
}
static int proxy_submit(void *c, uint64_t sector, uint32_t count, uint8_t *buffer)
{
    (void)c;
    loader_async = (struct request){.kind = K_ASYNC_READ, .sector = sector, .count = count, .buffer = buffer};
    return post(&loader_slot, &loader_async, 0);                          /* returns once queued; the loader polls */
}
static int proxy_poll(void *c)
{
    (void)c;
    int d = atomic_load_explicit(&loader_async.done, memory_order_acquire);
    return d;                                                              /* 0 busy, 1 done, -1 failed */
}
const struct os_large_device *os_large_device(void)
{
    if (!start()) return 0;
    proxy = (struct os_large_device){{NULL, proxy_read, hw->sectors}, {NULL, proxy_submit, proxy_poll},
                                     hw->device->partition_first, hw->device->partition_sectors, hw->device->buffers};
    return &proxy;
}
/* The card stays open for the writer; the loader's release changes nothing. */
void os_large_device_close(void) {}

/* --- the writer's view -------------------------------------------------------------------------------------------- */
static struct request writer_request;
static int writer_read(void *c, uint64_t sector, uint32_t count, uint8_t *out)
{
    (void)c;
    for (uint32_t done = 0; done < count;) {
        uint32_t n = count - done < OS_SD_WRITE_PIECE_SECTORS ? count - done : OS_SD_WRITE_PIECE_SECTORS;
        writer_request = (struct request){.kind = K_READ, .sector = sector + done, .count = n, .buffer = out + (size_t)done * 512u};
        if (!post(&writer_slot, &writer_request, 1)) return 0;
        done += n;
    }
    return 1;
}
static int writer_write(void *c, uint64_t sector, uint32_t count, const uint8_t *data)
{
    (void)c;
    if (!count || count > OS_SD_WRITE_PIECE_SECTORS) return 0;            /* the service splits at its max_write_sectors */
    writer_request = (struct request){.kind = K_WRITE, .sector = sector, .count = count, .source = data};
    return post(&writer_slot, &writer_request, 1);
}
/* Admit the writes: the runner-delivered bounds blob (the first input, kept unchanged), verified against the card. */
static int writes_enable(const uint8_t *blob, size_t bytes)
{
    if (!start()) return 0;
    writer_request = (struct request){.kind = K_ENABLE, .source = blob, .bytes = bytes};
    if (!post(&writer_slot, &writer_request, 1)) return 0;
    writer_request = (struct request){.kind = K_UNIT};
    if (!post(&writer_slot, &writer_request, 1)) return 0;
    unit_sectors = writer_request.value;
    return 1;
}
/* Register the application's flush buffer so the FAT32 service flushes from it straight into the controller. */
static int write_region(uint8_t *base, size_t bytes)
{
    if (!start()) return 0;
    writer_request = (struct request){.kind = K_REGION, .buffer = base, .bytes = bytes};
    return post(&writer_slot, &writer_request, 1);
}
/* The block device for the FAT32 write service; call after os_sd_writes_enable. */
static int writer_device(struct fwr_device *d)
{
    if (!hw || atomic_load(&ready) != 1) return 0;
    *d = (struct fwr_device){.context = NULL, .read = writer_read, .write = writer_write, .yield = NULL, .sectors = hw->sectors,
                             .au_sectors = unit_sectors, .max_write_sectors = OS_SD_WRITE_PIECE_SECTORS};
    return 1;
}

/* --- the application kit (include/sunstone_storage.h) ------------------------------------------------------------------- */
int ss_storage_enable(const void *bounds, size_t bytes) { return writes_enable(bounds, bytes); }
int ss_storage_region(void *base, size_t bytes) { return write_region(base, bytes); }
int ss_storage_device(struct fwr_device *d) { return writer_device(d); }
int ss_storage_close(void)
{
    if (!hw) return 0;
    writer_request = (struct request){.kind = K_CLOSE};
    return post(&writer_slot, &writer_request, 1);
}
int ss_storage_diagnostic(uint64_t out[4])
{
    if (!out) return 0;
    for (unsigned i = 0; i < 4; ++i) out[i] = os_sd_diag[i];
    return 1;
}
int ss_storage_owner_stats(struct ss_storage_owner_stats *out)
{
    if (!out || !hw) return 0;
    uint64_t now = sunstone_deadline_now_us();
    uint64_t slept = atomic_load(&idle_us) + hw->owner_sleep_us();
    *out = (struct ss_storage_owner_stats){now - first_us, slept, atomic_load(&wake_count) + hw->owner_wakes(),
                                                     0, atomic_load(&owner_core_mask), atomic_load(&caller_core_mask)};
    hw->irq_counts(out->irq_entries, &out->irq_stray);
    for (unsigned c = 0; c < 8; ++c) if (out->irq_entries[c]) out->irq_cores |= 1u << c;
    return 1;
}
