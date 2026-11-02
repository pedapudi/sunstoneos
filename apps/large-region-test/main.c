/* large-region-test: a 1 GiB region loaded from the card, read at EL0 in the application's own address space
 * (docs/designs/large-app-regions.md). It waits for /sunstone-test/sunstone-region-test.bin (512 MiB, the
 * stream of tools/region_test_file.py) to load, then reads the region with the 128-byte vector loads of
 * apps/dram-bandwidth/kernel.c and times the passes with the 24 MHz counter. It also times the task switch
 * between a task in its space and one in none. When the file is absent, the loader fails, or the digest
 * differs, it reports that and returns normally; it runs the measurements as soon as the file is there.
 *
 * The bandwidth is a figure only for an image at the approved top operating points (run it in the OS image
 * with the CPU grant in its session); the result records the counter and the clock record is the host's. */
#include "large_region-app.h"
#include "FreeRTOS.h"
#include "task.h"

#define MIB (UINT64_C(1) << 20)
#define CONTROL_BYTES (40 * MIB)
#define CONTROL_PASSES 10u        /* two warm-up passes, then eight timed passes, as apps/dram-bandwidth */
#define FULL_PASSES 3u
#define SWITCH_ROUNDS 20000u
#define TAG_DONE UINT64_C(0x4c52544f4e45)     /* "LRTONE" */
#define TAG_NOT_READY UINT64_C(0x4c52544e4f54)/* "LRTNOT" */

/* Identical to bw_read of apps/dram-bandwidth/kernel.c. */
__attribute__((noinline)) static uint64_t bw_read(const uint64_t *p, size_t bytes)
{
    uint64_t lo, hi;
    const uint64_t *end = p + bytes / 8;
    __asm__ volatile(
        ".arch_extension simd\n"
        "movi v16.16b,#0\n movi v17.16b,#0\n movi v18.16b,#0\n movi v19.16b,#0\n"
        "1:\n"
        "ld1 {v0.16b-v3.16b},[%[p]],#64\n"
        "ld1 {v4.16b-v7.16b},[%[p]],#64\n"
        "eor v16.16b,v16.16b,v0.16b\n eor v17.16b,v17.16b,v1.16b\n"
        "eor v18.16b,v18.16b,v2.16b\n eor v19.16b,v19.16b,v3.16b\n"
        "eor v16.16b,v16.16b,v4.16b\n eor v17.16b,v17.16b,v5.16b\n"
        "eor v18.16b,v18.16b,v6.16b\n eor v19.16b,v19.16b,v7.16b\n"
        "cmp %[p],%[end]\n b.lo 1b\n"
        "eor v16.16b,v16.16b,v17.16b\n eor v18.16b,v18.16b,v19.16b\n eor v16.16b,v16.16b,v18.16b\n"
        "mov %[lo],v16.d[0]\n mov %[hi],v16.d[1]\n"
        : [p] "+r"(p), [lo] "=r"(lo), [hi] "=r"(hi)
        : [end] "r"(end)
        : "v0","v1","v2","v3","v4","v5","v6","v7","v16","v17","v18","v19","cc","memory");
    return lo ^ hi;
}

static StaticTask_t peer_state;
static StackType_t peer_stack[512];
static TaskHandle_t main_handle, peer_handle;
static void peer(void *unused)
{
    (void)unused;
    for (;;) { (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY); xTaskNotifyGive(main_handle); }
}
static uint64_t round_trips(void)
{
    uint64_t start = ss_counter_ticks();
    for (unsigned i = 0; i < SWITCH_ROUNDS; ++i) { xTaskNotifyGive(peer_handle); (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY); }
    return ss_counter_ticks() - start;
}

/* The result, little-endian, 512 bytes. */
struct large_region_test_result {
    uint64_t tag, state, error, file_bytes, load_us;
    uint64_t switch_rounds, switch_same_ticks, switch_different_ticks;
    uint64_t control_ticks[CONTROL_PASSES], control_bytes;
    uint64_t full_ticks[FULL_PASSES], full_bytes;
    uint64_t fold_control, fold_full;
    uint64_t reserved[512 / 8 - 8 - CONTROL_PASSES - 1 - FULL_PASSES - 1 - 2];
};
_Static_assert(sizeof(struct large_region_test_result) == 512, "result is the manifest's output_limit");

void large_region_main(const struct ss_grant *grant)
{
    static struct large_region_test_result r;
    while (!ss_clock_ready()) { ss_progress(); vTaskDelay(1); }
    int ready;
    do { ss_progress(); ready = ss_large_wait(LARGE_REGION_LARGE_WEIGHTS, 1000000); } while (ready == 0 && ss_live() >= 0);
    struct ss_large_status status;
    ss_large_status(LARGE_REGION_LARGE_WEIGHTS, &status);
    r.state = status.state; r.error = status.error; r.file_bytes = status.total; r.load_us = status.elapsed_us;
    if (ready != 1) {
        r.tag = TAG_NOT_READY;
        ss_finish(&r, sizeof r);
    }
    r.tag = TAG_DONE;
    size_t bytes;
    const uint64_t *base = ss_large_base(LARGE_REGION_LARGE_WEIGHTS, &bytes);
    if (!base || bytes != (1u << 30)) ss_fail(0x1e01);
    /* The task switch with and without a TTBR0 write. */
    main_handle = xTaskGetCurrentTaskHandle();
    peer_handle = xTaskCreateStaticAffinitySet(peer, "peer", 512, NULL, 3, peer_stack, &peer_state, (UBaseType_t)1 << grant->main_core);
    ss_large_attach(peer_handle);
    (void)round_trips();
    r.switch_rounds = SWITCH_ROUNDS;
    r.switch_same_ticks = round_trips();
    vTaskSetApplicationTaskTag(peer_handle, 0);
    r.switch_different_ticks = round_trips();
    /* Control: a 40 MiB window of the region, ten passes. Full: the 1 GiB, three passes. */
    for (unsigned i = 0; i < CONTROL_PASSES; ++i) {
        uint64_t t0 = ss_counter_ticks();
        r.fold_control ^= bw_read(base, CONTROL_BYTES);
        r.control_ticks[i] = ss_counter_ticks() - t0;
        ss_progress();
    }
    r.control_bytes = CONTROL_BYTES;
    for (unsigned i = 0; i < FULL_PASSES; ++i) {
        uint64_t t0 = ss_counter_ticks();
        r.fold_full ^= bw_read(base, bytes);
        r.full_ticks[i] = ss_counter_ticks() - t0;
        ss_progress();
    }
    r.full_bytes = bytes;
    ss_finish(&r, sizeof r);
}
