#ifndef SUNSTONE_A733_G2D_ROTATE_H
#define SUNSTONE_A733_G2D_ROTATE_H
/* The G2D rotate module's register words for one job (docs/g2d-os.md#rotate-module).
 *
 * The rotator is programmed by direct register writes at G2D + 0x28000, not through the mixer's RCQ command page.
 * a733_g2d_rotate_words gives the words in the order of the vendor's g2d_rotate_set_para (g2d_rotate.c in
 * linux-orangepi 2ac08e8c, bsp/drivers/g2d/g2d_rcq): the control word without its start bit, the input format,
 * size, pitches and addresses, the output size, pitches and addresses, the finish-interrupt enable, and last the
 * control word with its start bit (bit 31). The caller writes them in that order. */
#include <stdint.h>
#define A733_G2D_ROT_BASE UINT32_C(0x28000)
#define A733_G2D_ROT_CTL 0x00u
#define A733_G2D_ROT_INT 0x04u
#define A733_G2D_ROT_TIMEOUT 0x08u
#define A733_G2D_ROT_START (UINT32_C(1) << 31)
/* ROT_INT: the finish-interrupt enable (bit 16) and the finish status (bit 0, cleared by writing the read word back). */
#define A733_G2D_ROT_INT_ENABLE UINT32_C(0x10000)
#define A733_G2D_ROT_INT_DONE UINT32_C(1)
/* The size fields hold w - 1 and h - 1 in 13 bits. */
#define A733_G2D_ROT_MAX 8192u
#define A733_G2D_ROT_FLIP_H 1u
#define A733_G2D_ROT_FLIP_V 2u
/* 24 words: the control word, the format, the size, 3 pitches, 6 address words, the output size, 3 pitches,
 * 6 address words, the interrupt enable and the start. */
#define A733_G2D_ROT_WORDS 24u
struct a733_g2d_rot_word { uint32_t offset, value; };
/* One rotate job on single-plane 32-bit pixels (formats 0 to 7, the eight RGB orders). The source rectangle is w by h
 * at sx, sy of a surface at source with source_stride bytes per line; the output rectangle starts at dx, dy of a
 * surface at output with output_stride bytes per line, and is h by w for 90 and 270 degrees. Addresses are the
 * engine's (IOMMU master 10) and may use 40 bits. */
struct a733_g2d_rotate {
    uint64_t source, output;
    uint32_t source_stride, output_stride;
    uint32_t sx, sy, dx, dy, w, h;
    uint32_t format, degrees, flip;
};
/* The words of the job into out, A733_G2D_ROT_WORDS of them, offsets relative to the G2D base; the count, or 0 for a
 * job the vendor refuses or the registers cannot hold, with out unchanged:
 * - a format above 7, degrees other than 0, 90, 180 or 270, a flip above 3;
 * - w or h 0 or above 8192, or a rectangle wider than its stride;
 * - an input or output origin not 4-byte aligned, or an output stride not 8-byte aligned (both refused by the
 *   vendor, g2d_rotate.c 223-226, 296-299 and 314-317);
 * - an origin at or above 2^40. */
unsigned a733_g2d_rotate_words(const struct a733_g2d_rotate *, struct a733_g2d_rot_word out[A733_G2D_ROT_WORDS]);
#endif
