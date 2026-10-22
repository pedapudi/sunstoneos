#ifndef SUNSTONE_PVR_COMPUTE_MIMUS_READOUT_H
#define SUNSTONE_PVR_COMPUTE_MIMUS_READOUT_H
/* CPU read-out of GPU sums for mimus linear layers (docs/gpu-mimus.md).
 *
 * Each output is one exact integer sum, converted to float once and then
 * scaled in mimus's fixed float order (GemmCombine in
 * gemm_combine_portable.cc, Combine in kernels.cc):
 *   value = (0 + (float(sum) * ws[n]) * xs[r]) + (bias ? bias[n] : 0)
 * and for residual layers y = y + alpha * value. The subsampling output's
 * Int40ToFloat rounds the exact sum once, so it gives float(sum) too.
 *
 * Build with -ffp-contract=off: a fused multiply-add changes the result.
 * int64 sums convert through double, which is exact for |sum| < 2^53 (GPU
 * sums stay below 2^38), so the single rounding to float equals
 * float(sum) and the conversion vectorizes. Portable C, usable from C++. */
#include <stdint.h>

struct pvr_mimus_readout {
    const void *sums;  /* int32_t or int64_t, rows by n_gpu, row-major */
    const float *ws;   /* per channel */
    const float *xs;   /* per row */
    const float *bias; /* per channel, or NULL */
    float *y;          /* row r at y + r * y_stride */
    uint32_t rows, n_gpu, y_stride;
    float alpha;       /* residual read-outs only */
};

#define PVR_MIMUS_READOUT(name, type, convert, residual)                         \
static inline void name(const struct pvr_mimus_readout *a)                       \
{                                                                                \
    const type *sums = (const type *)a->sums;                                    \
    for (uint32_t r = 0; r < a->rows; ++r) {                                     \
        const type *s = sums + (uint64_t)r * a->n_gpu;                           \
        const float xs = a->xs[r], alpha = a->alpha;                             \
        float *y = a->y + (uint64_t)r * a->y_stride;                             \
        (void)alpha;                                                             \
        if (a->bias) {                                                           \
            for (uint32_t n = 0; n < a->n_gpu; ++n) {                            \
                float acc = 0.0f;                                                \
                acc += convert(s[n]) * a->ws[n] * xs;                            \
                const float value = acc + a->bias[n];                            \
                y[n] = residual ? y[n] + alpha * value : value;                  \
            }                                                                    \
        } else {                                                                 \
            for (uint32_t n = 0; n < a->n_gpu; ++n) {                            \
                float acc = 0.0f;                                                \
                acc += convert(s[n]) * a->ws[n] * xs;                            \
                const float value = acc + 0.0f;                                  \
                y[n] = residual ? y[n] + alpha * value : value;                  \
            }                                                                    \
        }                                                                        \
    }                                                                            \
}
#define PVR_MIMUS_FROM_I32(s) ((float)(s))
#define PVR_MIMUS_FROM_I64(s) ((float)(double)(s))
PVR_MIMUS_READOUT(pvr_mimus_readout_i32, int32_t, PVR_MIMUS_FROM_I32, 0)
PVR_MIMUS_READOUT(pvr_mimus_readout_i64, int64_t, PVR_MIMUS_FROM_I64, 0)
PVR_MIMUS_READOUT(pvr_mimus_readout_i32_accumulate, int32_t, PVR_MIMUS_FROM_I32, 1)
PVR_MIMUS_READOUT(pvr_mimus_readout_i64_accumulate, int64_t, PVR_MIMUS_FROM_I64, 1)
#undef PVR_MIMUS_READOUT
#endif
