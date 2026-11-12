/* Test application of checks/qemu-os-boot.py for large regions (docs/designs/large-app-regions.md).
 * Its input region holds a mode (u64) and the XOR of the 64-bit words that the file region must hold
 * (u64). It waits for large region 0 to load, reads it at EL0 and reports what happened:
 *   mode 0: finish with the result {tag, status or fold}; a region that failed to load is reported, not hidden.
 *   mode 1: after the read, start a task that runs in no application space and reads the region: it must fault.
 *   mode 2: measure the task switch: two tasks on one core hand a notification to each other, first with both in the
 *           application's space (no TTBR0 write) and then with the second in no space (two writes per round trip).
 * The region's file is 4 MiB. Before it reads the region the main task writes and reads back both ends of the capture buffer,
 * which admission took from the ledger as a kernel extent that the application's tables map (apps/os/capture.c). */
#include "large_test-app.h"
#include "FreeRTOS.h"
#include "task.h"

#define FILE_BYTES (4u << 20)
#define TAG_OK UINT64_C(0x4c4152474f4b)         /* "LARGOK" */
#define TAG_FAILED UINT64_C(0x4c415247464c)     /* "LARGFL" */
#define TAG_MISMATCH UINT64_C(0x4c4152474d4d)   /* "LARGMM" */
extern uint8_t *ss_capture_buffer;
extern const size_t ss_capture_bytes;

static StaticTask_t intruder_state;
static StackType_t intruder_stack[512];
static volatile uint8_t *region;
static StaticTask_t peer_state;
static StackType_t peer_stack[512];
static TaskHandle_t main_handle, peer_handle;

static void peer(void *unused)
{
    (void)unused;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xTaskNotifyGive(main_handle);
    }
}

#define ROUNDS 20000u
static uint64_t round_trips(void)
{
    uint64_t start = ss_counter_ticks();
    for (unsigned i = 0; i < ROUNDS; ++i) {
        xTaskNotifyGive(peer_handle);
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    return ss_counter_ticks() - start;
}

static void intruder(void *unused)
{
    (void)unused;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* until the creator has taken it out of the space */
    (void)*region;                                /* no application space: a permission fault at EL0 */
    ss_fail(0xe02);                               /* never reached */
}

void large_test_main(const struct ss_grant *grant)
{
    const uint64_t *input = (const uint64_t *)grant->regions[LARGE_TEST_REGION_INPUT].base;
    uint64_t mode = input[0], expected = input[1];
    volatile uint8_t *capture = ss_capture_buffer;
    if (!capture || ss_capture_bytes < 4096) ss_fail(0xe03);
    capture[0] = 0x5a; capture[ss_capture_bytes - 1] = 0xa5;
    if (capture[0] != 0x5a || capture[ss_capture_bytes - 1] != 0xa5) ss_fail(0xe04);
    int ready = ss_large_wait(LARGE_TEST_LARGE_WEIGHTS, 8000000);
    struct ss_large_status status;
    ss_large_status(LARGE_TEST_LARGE_WEIGHTS, &status);
    if (ready != 1) {
        const uint64_t failed[2] = {TAG_FAILED, status.error};
        ss_finish(failed, sizeof(failed));
    }
    size_t bytes;
    volatile const uint64_t *p = ss_large_base(LARGE_TEST_LARGE_WEIGHTS, &bytes);
    if (!p || bytes != (1u << 30) || status.loaded != FILE_BYTES || status.total != FILE_BYTES) ss_fail(0xe01);
    if (mode == 2) {
        main_handle = xTaskGetCurrentTaskHandle();
        peer_handle = xTaskCreateStaticAffinitySet(peer, "peer", 512, NULL, 3, peer_stack, &peer_state,
                                                   (UBaseType_t)1 << grant->main_core);
        ss_large_attach(peer_handle);                      /* both tasks in the application's space */
        (void)round_trips();                               /* warm up */
        uint64_t same = round_trips();
        vTaskSetApplicationTaskTag(peer_handle, 0);       /* the peer back in the kernel's table */
        uint64_t different = round_trips();
        const uint64_t bench[4] = {UINT64_C(0x4c4152474245), same, different, ROUNDS};
        ss_finish(bench, sizeof(bench));
    }
    uint64_t fold = 0;
    for (size_t i = 0; i < FILE_BYTES / 8; ++i) fold ^= p[i];
    if (fold != expected) {
        const uint64_t bad[2] = {TAG_MISMATCH, fold};
        ss_finish(bad, sizeof(bad));
    }
    if (mode == 1) {
        region = (volatile uint8_t *)p;
        /* A task takes the space of its creator, so the intruder leaves the space before it reads. */
        TaskHandle_t intruder_handle = xTaskCreateStatic(intruder, "intruder", 512, NULL, 3, intruder_stack, &intruder_state);
        vTaskSetApplicationTaskTag(intruder_handle, 0);
        xTaskNotifyGive(intruder_handle);
        for (;;) vTaskDelay(10);
    }
    const uint64_t ok[2] = {TAG_OK, fold};
    ss_finish(ok, sizeof(ok));
}
