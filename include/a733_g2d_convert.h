/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_G2D_CONVERT_H
#define SUNSTONE_A733_G2D_CONVERT_H
/* The G2D mixer's RCQ command page for one YUV-to-RGB conversion with scaling (docs/g2d-os.md#camera-frame-conversion).
 *
 * The page follows the vendor's g2d_bsp_bitblt with flag G2D_BLT_NONE (g2d_mixer.c 304-371 in linux-orangepi
 * 2ac08e8c, bsp/drivers/g2d/g2d_rcq) for a 4:2:2 semi-planar source and a 32-bit RGB output: the video layer V0
 * (g2d_vlayer_set), the coarse decimation (g2d_ovl_v_calc_coarse), the video scaler, enabled for every YUV source
 * (g2d_vsu_para_set), the blender with source-over, and the write-back (g2d_wb_set). The colour conversion follows the
 * vendor's other path, g2d_bsp_bld (g2d_mixer.c 216-220 and 287): csc0 converts at pipe 0's input and the blender works
 * in RGB, which keeps luma below 16 (g2d_mixer.c 202 makes that path unreachable in the vendor driver). The tables are
 * derived from the BT.601 and BT.709 matrices (tools/g2d-csc-tables.py). The block headers and their order are those
 * of a733_g2d_commands_at (drivers/g2d/a733_g2d.c). */
#include <stddef.h>
#include <stdint.h>
/* The vendor's YUV422UVC_V1U1V0U0 and YUV422UVC_U1V1U0V0: a luma plane and a plane of interleaved chroma pairs. */
#define A733_G2D_YUV422_SP_A UINT32_C(0x24)
#define A733_G2D_YUV422_SP_B UINT32_C(0x25)
/* The size fields hold the size minus one in 13 bits. */
#define A733_G2D_CONVERT_MAX 8192u
/* The scaler's widest input after the coarse decimation (g2d_ovl_v_calc_coarse). */
#define A733_G2D_VSU_MAX_INPUT 2048u
/* The four YUV-to-RGB tables, by the source's colour space; the RGB output is full range. */
enum { A733_G2D_CSC_BT601_LIMITED = 1, A733_G2D_CSC_BT601_FULL, A733_G2D_CSC_BT709_LIMITED, A733_G2D_CSC_BT709_FULL };
/* One conversion: the w by h rectangle at sx, sy of a source whose luma plane starts at luma (luma_stride bytes a row)
 * and whose chroma plane starts at chroma (chroma_stride bytes a row), into the ow by oh rectangle at dx, dy of an
 * output of out_format (0 to 7) at output, output_stride bytes a row. Addresses are the engine's (IOMMU master 10). */
struct a733_g2d_convert {
    uint32_t luma, chroma, luma_stride, chroma_stride;
    uint32_t sx, sy, w, h, format;
    uint32_t output, output_stride, dx, dy, ow, oh, out_format;
    uint32_t csc;
};
/* The 12 csc0 words for a table (bld_csc_reg_set's 12-word copy: per output channel R, G, B the Y, U and V
 * coefficients with 10 fraction bits, then the constant scaled by 1024, tools/g2d-csc-tables.py), or null for an
 * unknown table. */
const int32_t *a733_g2d_csc_table(uint32_t csc);
/* The command page into memory (capacity 4096, 32-byte aligned) for the page at device address rcq_iova (4096-byte
 * aligned). 1, or 0 with nothing written for a job the vendor refuses or the registers cannot hold:
 * - a format other than the two 4:2:2 semi-planar codes, an out_format above 7, an unknown csc;
 * - w, h, ow or oh 0 or above 8192; an odd sx (the chroma pair would split);
 * - a stride narrower than its rectangle (luma w bytes, chroma w rounded up to even, output 4 * ow);
 * - an address not 4-byte aligned, or a rectangle's first byte at or above 2^32. */
int a733_g2d_convert_commands(void *memory, size_t capacity, const struct a733_g2d_convert *c, uint32_t rcq_iova);
#endif
