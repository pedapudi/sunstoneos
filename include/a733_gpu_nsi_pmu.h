#ifndef SUNSTONE_A733_GPU_NSI_PMU_H
#define SUNSTONE_A733_GPU_NSI_PMU_H
#include <stdint.h>
/* Diagnostic access is confined to GPU IA6 in the NSI block. The register
 * identities and 16-byte data unit are pinned in docs/designs/gpu-nsi-traffic.md.
 * Raw data remain experimental until the board establishes live/latch behavior. */
enum a733_gpu_nsi_register { A733_NSI_ENABLE=0xcc0, A733_NSI_CLEAR=0xcc4,
    A733_NSI_CYCLE=0xcc8, A733_NSI_READ=0xcd4, A733_NSI_WRITE=0xcd8 };
enum a733_gpu_nsi_status { A733_NSI_UNSAMPLED, A733_NSI_SAMPLED,
    A733_NSI_BUSY, A733_NSI_CLOCK, A733_NSI_IO, A733_NSI_CLEAR_PENDING,
    A733_NSI_PERIOD_EXPIRED, A733_NSI_NONMONOTONIC };
struct a733_gpu_nsi_ops {
    int (*read)(void *, enum a733_gpu_nsi_register, uint32_t *);
    int (*write)(void *, enum a733_gpu_nsi_register, uint32_t);
    uint64_t (*ticks)(void *); /* Architectural counter, 24 MHz. */
};
struct a733_gpu_nsi_record {
    uint64_t begin, end;
    /* 0 inherited, 1 after idle settle, 2 after kick, 3 live completion,
     * 4 immediately disabled, 5 after disabled settle. */
    uint32_t read[6], write[6];
    uint32_t status, samples, hz, clock_raw, saved_enable, saved_cycle, clear_raw;
    uint32_t oscillator_raw, reference_raw, peri0_raw, peri_pattern0_raw, peri_pattern1_raw;
    uint32_t de_raw, pattern0_raw, pattern1_raw, clock_end_raw, gate_raw;
};
struct a733_gpu_nsi_pmu {
    struct a733_gpu_nsi_ops ops;
    void *context;
    uint64_t enabled;
    uint32_t saved_enable, saved_cycle, owned, failed;
};
/* Integer-mode clock decoding only. Zero rejects unsupported or changing state.
 * This establishes configured rate, not frequency measurement or clock ownership. */
uint32_t a733_gpu_nsi_hz(uint32_t oscillator, uint32_t reference, uint32_t peri0,
    uint32_t peri_pattern0, uint32_t peri_pattern1,
    uint32_t de, uint32_t de_pattern0, uint32_t de_pattern1, uint32_t clock);
void a733_gpu_nsi_begin(struct a733_gpu_nsi_pmu *, const struct a733_gpu_nsi_ops *,
    void *, uint32_t hz, uint32_t clock, struct a733_gpu_nsi_record *);
void a733_gpu_nsi_after_kick(struct a733_gpu_nsi_pmu *, struct a733_gpu_nsi_record *);
/* Restores saved disabled configuration even when the sample is invalid. */
void a733_gpu_nsi_end(struct a733_gpu_nsi_pmu *, struct a733_gpu_nsi_record *);
#endif
