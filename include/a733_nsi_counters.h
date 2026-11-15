#ifndef SUNSTONE_A733_NSI_COUNTERS_H
#define SUNSTONE_A733_NSI_COUNTERS_H
#include <stdint.h>
/* Traffic counters of two NSI initiator ports: 6 (GPU) and 18 (NPU). Each port's bank holds nine words at
 * 0x02020000 + 0x200 * port + 0xc0: ENABLE, CLR and CYCLE, which the owner writes, then the read and write request,
 * data (16-byte units) and latency counters, which it only reads (vendor sunxi-nsi.h MBUS_PMU_*, lines 377-385; the
 * unit is ia_pmu_data_unit in sun60iw2p1.dtsi). The sequence follows __nsi_pmu_timer_store_v2 in sunxi-nsi.c: period,
 * disable, clear, enable, 10 us. The period (CYCLE) is the caller's, in NSI clock cycles, and must not be zero. No QoS,
 * priority, mode or limit word is touched.
 *
 * The owner reads all eighteen words before any write and keeps them as the reset values. A bank whose enable bit is
 * already set belongs to another counter user: open then refuses with no write. Every write is read back. A phase
 * programs a bank, and ending the phase restores CYCLE, CLR and ENABLE to their reset values with readbacks; a
 * failed readback restores every programmed bank and refuses later writes until the next open. Restore puts one bank
 * back alone, for the power-off of its master's domain (the vendor device tree places port 6 in GPU_TOP and port 18 in
 * the NPU domain).
 *
 * The counts latch. On the board, each count stays 0 until the first period end after enable. At each period end it
 * steps to that period's total and holds that value until the next. A caller therefore gives every measured phase a
 * CYCLE that ends inside it and samples just after each period end. CYCLE counts the 600 MHz NSI clock, so
 * 60,000,000 cycles is 100 ms. A phase shorter than one period reads 0
 * (docs/designs/concurrent-accel.md, "NSI counters"). */
#define A733_NSI_COUNTER_BASE UINT32_C(0x02020000)
#define A733_NSI_COUNTER_BANKS 2u   /* ports 6 and 18, the two accelerator ports this diagnostic owns */
#define A733_NSI_COUNTER_WORDS 9u   /* ENABLE, CLR, CYCLE, RQ_RD, RQ_WR, DT_RD, DT_WR, LA_RD, LA_WR */
#define A733_NSI_COUNTER_VALUES 6u  /* RQ_RD .. LA_WR */
enum { A733_NSI_W_ENABLE, A733_NSI_W_CLR, A733_NSI_W_CYCLE, A733_NSI_W_RQ_RD };
enum a733_nsi_counter_status { A733_NSI_C_OK, A733_NSI_C_REFUSED, A733_NSI_C_BUSY, A733_NSI_C_IO };
enum a733_nsi_counter_op { A733_NSI_OPEN = 1, A733_NSI_BEGIN, A733_NSI_SAMPLE, A733_NSI_END, A733_NSI_CLOSE };

struct a733_nsi_counter_ops {
    int (*read)(void *, uintptr_t, uint32_t *);
    int (*write)(void *, uintptr_t, uint32_t);
    uint64_t (*ticks)(void *);   /* 24 MHz architectural counter */
};
struct a733_nsi_counters {
    struct a733_nsi_counter_ops ops;
    void *context;
    uint32_t reset[A733_NSI_COUNTER_BANKS][A733_NSI_COUNTER_WORDS];
    uint32_t opened, failed, programmed;   /* programmed: one bit per bank */
    uint32_t fail_address, fail_value;
};
/* The application's call (through the GPU call gateway, gpu_queue_nsi_counters). OPEN fills reset; BEGIN takes cycle
 * and fills clear, the CLR readback; SAMPLE fills value; END fills value (just after the disable) and held (10 us
 * later), then restores; CLOSE restores any programmed bank. */
struct a733_nsi_counter_call {
    uint32_t op, bank, status, clear, cycle, reserved;
    uint32_t reset[A733_NSI_COUNTER_BANKS][A733_NSI_COUNTER_WORDS];
    uint32_t value[A733_NSI_COUNTER_VALUES], held[A733_NSI_COUNTER_VALUES];
    uint32_t fail_address, fail_value;
};

uintptr_t a733_nsi_counter_address(unsigned bank, unsigned word);
int a733_nsi_counter_writable(uintptr_t address);
int a733_nsi_counter_readable(uintptr_t address);
int a733_nsi_counters_open(struct a733_nsi_counters *, const struct a733_nsi_counter_ops *, void *context,
                           uint32_t reset[A733_NSI_COUNTER_BANKS][A733_NSI_COUNTER_WORDS]);
int a733_nsi_counters_begin(struct a733_nsi_counters *, unsigned bank, uint32_t cycle, uint32_t *clear_readback);
int a733_nsi_counters_sample(struct a733_nsi_counters *, unsigned bank, uint32_t value[A733_NSI_COUNTER_VALUES]);
int a733_nsi_counters_end(struct a733_nsi_counters *, unsigned bank, uint32_t value[A733_NSI_COUNTER_VALUES],
                          uint32_t held[A733_NSI_COUNTER_VALUES]);
int a733_nsi_counters_restore(struct a733_nsi_counters *, unsigned bank);
int a733_nsi_counters_close(struct a733_nsi_counters *);
int a733_nsi_counters_call(struct a733_nsi_counters *, const struct a733_nsi_counter_ops *, void *context,
                           struct a733_nsi_counter_call *);
#endif
