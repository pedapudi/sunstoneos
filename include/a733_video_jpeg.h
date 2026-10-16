/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_JPEG_H
#define SUNSTONE_A733_VIDEO_JPEG_H
#include <stddef.h>
#include <stdint.h>

/* First profile: baseline, single interleaved scan, 8-bit YCbCr 4:2:0.
 * Components 1/2/3 are Y/Cb/Cr. Both chroma components share tables.
 * Tables must be explicit; abbreviated MJPEG/default Huffman tables are refused.
 * No entropy decoding, allocation, MMIO, or ownership change occurs here.
 */
struct a733_video_huffman {
    uint8_t counts[16], symbols[256];
    uint16_t count;
    uint8_t present;
};
struct a733_video_jpeg {
    uint16_t width, height, restart_interval;
    uint8_t quant[2][64], quant_present[2]; /* JPEG zigzag order. */
    struct a733_video_huffman huffman[2][2]; /* [DC/AC][table ID]. */
    uint8_t quant_select[3], dc_select[3], ac_select[3];
    uint32_t scan_offset, scan_bytes, file_bytes;
};
struct a733_video_jpeg_tables {
    uint32_t quant[128]; /* Sequential writes to decoder +0x180. */
    uint32_t huffman[512]; /* +0x1e0=0, then sequential +0x1e4 writes. */
    uint32_t size; /* Decoder +0x1b8: MCU rows/columns minus one. */
    uint32_t restart; /* Decoder +0x1c0. */
    uint8_t format; /* Byte write to +0x11b; 4:2:0 JPEG = 3. */
};
/* Returns 1 on success, 0 for malformed/unsupported input. Output is unchanged
 * on refusal. Input/output storage must be valid and nonoverlapping. Input may
 * be released after parsing; scan_offset/scan_bytes describe the original file.
 * Headers, tables, marker framing and restart count are checked; entropy-coded
 * coefficient validity and device access bounds require separate admission.
 */
int a733_video_jpeg_parse(const uint8_t *, size_t, struct a733_video_jpeg *);
/* Revalidates public metadata; unchanged output on refusal. Source reference:
 * libawmjpeg.so in the pinned A733/T736 libcedarc package; see video-codecs.md.
 */
int a733_video_jpeg_prepare(const struct a733_video_jpeg *,
                           struct a733_video_jpeg_tables *);
#endif
