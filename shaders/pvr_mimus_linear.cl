/* Exact sums of int16 activation codes times int8 weight codes for one mimus
 * linear layer job (docs/gpu-mimus.md). Unified registers 0-1 hold the
 * 32-byte argument record and 2-3 the output region: 64 KiB of tag words,
 * then the sums.
 *
 * Argument record, little-endian words:
 *   0-1 weight codes of channel 0 (GPU address), one channel per stride
 *   2-3 activation codes of row 0 (GPU address), one row per stride
 *   4   K | n_gpu << 16
 *   5   rows
 *   6   weight stride | activation stride << 16, both in bytes
 *   7   sequence
 * The worker admits K, both addresses and both strides as multiples of 4, so
 * every load below is an aligned 32-bit word inside the declared ranges.
 *
 * Each lane computes 4 rows by 4 channels; a (4, 8) workgroup covers 16
 * channels by 32 rows. Loads clamp to the last row and channel, so padded
 * lanes read admitted memory. A chunk of at most 512 products fits int32
 * (|x| <= 32767, |w| <= 128). Chunk sums c are added into lo, the sum modulo
 * 2^32, and into t, the sum of c >> 8; the high word is then
 * (t >> 24) + ((uint)(t << 8) > lo), exact while K < 2^16.
 *
 * Every sum store carries an issue-data fence, so it reaches memory before
 * the lane's tag store; the pinned compiler removes mem_fence for 32-lane
 * workgroups. A store outside the job's channels, and the high word of a
 * 32-bit sum, goes to the lane's own tag word, which the tag store then
 * overwrites, so the stores need no branches. */
#include "pco/usclib/libcl.h"

#define SUMS 65536u

static inline int lo16(uint v) { return (int)(v << 16) >> 16; }
static inline int hi16(uint v) { return (int)v >> 16; }
static inline int byte8(uint v, uint i) { return (int)(v << (24 - 8 * i)) >> 24; }

KERNEL2D(4, 8)
cs_mimus_linear_common(void)
{
    uint2 args = (uint2)(nir_load_vtxin_pco(1, 0), nir_load_vtxin_pco(1, 1));
    uint2 out = (uint2)(nir_load_vtxin_pco(1, 2), nir_load_vtxin_pco(1, 3));
    uint4 bases = nir_dma_ld_pco(4, args);
    uint4 shape = nir_dma_ld_pco(4, nir_uadd64_32(args.x, args.y, 16));
    uint k = shape.x & 0xffffu, n = shape.x >> 16, rows = shape.y;
    uint wstride = shape.z & 0xffffu, xstride = shape.z >> 16;
    uint gx = get_group_id(0), gy = get_group_id(1);
    uint lx = get_local_id(0), ly = get_local_id(1);
    uint c0 = gx * 16 + lx * 4, r0 = gy * 32 + ly * 4;
    uint2 wp[4], xp[4];
    for (uint i = 0; i < 4; ++i) {
        wp[i] = nir_uadd64_32(bases.x, bases.y, min(c0 + i, n - 1) * wstride);
        xp[i] = nir_uadd64_32(bases.z, bases.w, min(r0 + i, rows - 1) * xstride);
    }
    uint lo[16];
    int t[16];
    for (uint i = 0; i < 16; ++i) { lo[i] = 0; t[i] = 0; }
    for (uint k0 = 0; k0 < k; k0 += 512) {
        int c[16];
        for (uint i = 0; i < 16; ++i) c[i] = 0;
        uint kend = min(k, k0 + 512);
        for (uint kk = k0; kk < kend; kk += 4) {
            uint w[4], xa[4], xb[4];
            for (uint i = 0; i < 4; ++i) {
                w[i] = nir_dma_ld_pco(1, nir_uadd64_32(wp[i].x, wp[i].y, kk));
                xa[i] = nir_dma_ld_pco(1, nir_uadd64_32(xp[i].x, xp[i].y, 2 * kk));
                xb[i] = nir_dma_ld_pco(1, nir_uadd64_32(xp[i].x, xp[i].y, 2 * kk + 4));
            }
            for (uint r = 0; r < 4; ++r) {
                int x0 = lo16(xa[r]), x1 = hi16(xa[r]), x2 = lo16(xb[r]), x3 = hi16(xb[r]);
                for (uint ch = 0; ch < 4; ++ch) {
                    int s = c[r * 4 + ch];
                    s += x0 * byte8(w[ch], 0);
                    s += x1 * byte8(w[ch], 1);
                    s += x2 * byte8(w[ch], 2);
                    s += x3 * byte8(w[ch], 3);
                    c[r * 4 + ch] = s;
                }
            }
        }
        for (uint i = 0; i < 16; ++i) {
            lo[i] += (uint)c[i];
            t[i] += c[i] >> 8;
        }
    }
    uint groups_x = (n + 15) >> 4;
    uint tags = groups_x * ((rows + 31) >> 5) * 32;
    uint tag = SUMS - 4 * tags + 4 * ((gy * groups_x + gx) * 32 + ly * 4 + lx);
    uint wide = k > 512;
    /* The trip count is not a constant, so the compiler keeps one copy of the
     * stores; each pass stores one row, then moves the next row's sums up. */
    uint rend = min(r0 + 4, rows);
    for (uint row = r0; row < rend; ++row) {
        for (uint ch = 0; ch < 4; ++ch) {
            uint inside = c0 + ch < n;
            uint at = SUMS + (row * n + c0 + ch) * (4 << wide);
            uint hi = (uint)(t[ch] >> 24) + ((uint)t[ch] << 8 > lo[ch]);
            nir_dma_st_pco(true, nir_uadd64_32(out.x, out.y, inside ? at : tag), lo[ch]);
            nir_dma_st_pco(true, nir_uadd64_32(out.x, out.y, inside && wide ? at + 4 : tag), hi);
        }
        for (uint i = 0; i < 12; ++i) { lo[i] = lo[i + 4]; t[i] = t[i + 4]; }
    }
    nir_dma_st_pco(false, nir_uadd64_32(out.x, out.y, tag), shape.w);
}
