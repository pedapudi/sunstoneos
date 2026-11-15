/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The G2D rotate module's register words, in the order of the vendor's g2d_rotate_set_para (g2d_rotate.c 106-346 in
 * linux-orangepi 2ac08e8c, bsp/drivers/g2d/g2d_rcq; Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd.).
 * include/a733_g2d_rotate.h states the job and its refusals. checks/g2d-rotate-words.py compares every word with a
 * transcription of the vendor function. Integer only, so it may be built with -mgeneral-regs-only. */
#include "a733_g2d_rotate.h"
#include <stddef.h>

/* g2d_rotate.c's register offsets, relative to the G2D base. */
enum {
    IFMT = 0x20, ISIZE = 0x24, IPITCH0 = 0x30, IPITCH1 = 0x34, IPITCH2 = 0x38,
    ILADD0 = 0x40, IHADD0 = 0x44, ILADD1 = 0x48, IHADD1 = 0x4c, ILADD2 = 0x50, IHADD2 = 0x54,
    OSIZE = 0x84, OPITCH0 = 0x90, OPITCH1 = 0x94, OPITCH2 = 0x98,
    OLADD0 = 0xa0, OHADD0 = 0xa4, OLADD1 = 0xa8, OHADD1 = 0xac, OLADD2 = 0xb0, OHADD2 = 0xb4,
};
#define ADDRESS_LIMIT (UINT64_C(1) << 40)

unsigned a733_g2d_rotate_words(const struct a733_g2d_rotate *r, struct a733_g2d_rot_word out[A733_G2D_ROT_WORDS])
{
    if (!r || !out) return 0;
    const uint32_t w = r->w, h = r->h, degrees = r->degrees;
    /* g2d_byte_cal (g2d.c 351-357): 4 bytes per pixel for formats 0 to 7, one plane. */
    if (r->format > 7u || r->flip > 3u || (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) ||
        w - 1u >= A733_G2D_ROT_MAX || h - 1u >= A733_G2D_ROT_MAX) return 0;
    const int swap = degrees == 90 || degrees == 270;
    const uint32_t ow = swap ? h : w, oh = swap ? w : h;
    if ((uint64_t)w * 4 > r->source_stride || (uint64_t)ow * 4 > r->output_stride) return 0;
    /* The rectangles' origins: the surface address plus pitch0 * y + ycnt * x (g2d_rotate.c 210-211, 301-302). */
    const uint64_t in = r->source + (uint64_t)r->source_stride * r->sy + (uint64_t)r->sx * 4;
    const uint64_t to = r->output + (uint64_t)r->output_stride * r->dy + (uint64_t)r->dx * 4;
    if (r->source >= ADDRESS_LIMIT || r->output >= ADDRESS_LIMIT || in >= ADDRESS_LIMIT || to >= ADDRESS_LIMIT ||
        (in & 3) || (to & 3) || (r->output_stride & 7)) return 0;
    /* Mode 1, the flips and the degree field (g2d_rotate.c 152-166): hflip bit 7, vflip bit 6, degree bits 4-5. */
    uint32_t ctl = 1u;
    if (r->flip & A733_G2D_ROT_FLIP_H) ctl |= 1u << 7;
    if (r->flip & A733_G2D_ROT_FLIP_V) ctl |= 1u << 6;
    ctl |= (degrees / 90u) << 4;
    const struct a733_g2d_rot_word words[A733_G2D_ROT_WORDS] = {
        {A733_G2D_ROT_CTL, ctl},
        {IFMT, r->format & 0x3fu},
        {ISIZE, ((h - 1u) & 0x1fffu) << 16 | ((w - 1u) & 0x1fffu)},
        /* A single-plane format has no second or third plane: its pitches and addresses are 0. */
        {IPITCH0, r->source_stride}, {IPITCH1, 0}, {IPITCH2, 0},
        {ILADD0, (uint32_t)in}, {IHADD0, (uint32_t)(in >> 32) & 0xffu}, {ILADD1, 0}, {IHADD1, 0},
        {ILADD2, 0}, {IHADD2, 0},
        {OSIZE, ((oh - 1u) & 0x1fffu) << 16 | ((ow - 1u) & 0x1fffu)},
        {OPITCH0, r->output_stride}, {OPITCH1, 0}, {OPITCH2, 0},
        {OLADD0, (uint32_t)to}, {OHADD0, (uint32_t)(to >> 32) & 0xffu}, {OLADD1, 0}, {OHADD1, 0},
        {OLADD2, 0}, {OHADD2, 0},
        /* The finish-interrupt enable, then the start bit by read-modify-write of the control word (327-330). */
        {A733_G2D_ROT_INT, A733_G2D_ROT_INT_ENABLE},
        {A733_G2D_ROT_CTL, ctl | A733_G2D_ROT_START},
    };
    for (size_t i = 0; i < A733_G2D_ROT_WORDS; ++i)
        out[i] = (struct a733_g2d_rot_word){A733_G2D_ROT_BASE + words[i].offset, words[i].value};
    return A733_G2D_ROT_WORDS;
}
