/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_jpeg.h"

static unsigned be16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static int huffman_valid(const struct a733_video_huffman *h, unsigned ac)
{
    unsigned total = 0, code = 0;
    uint8_t seen[256] = {0};
    if (!h->present || !h->count || h->count > 256) return 0;
    for (unsigned i = 0; i < 16; ++i) {
        total += h->counts[i];
        code += h->counts[i];
        /* An all-one code is reserved for JPEG padding. */
        if (code >= (1u << (i + 1))) return 0;
        code <<= 1;
    }
    if (total != h->count) return 0;
    for (unsigned i = 0; i < total; ++i) {
        unsigned s = h->symbols[i], size = s & 15u;
        if (seen[s]++) return 0;
        if (!ac && s > 11) return 0;
        if (ac && (size > 10 || (!size && s != 0 && s != 0xf0))) return 0;
    }
    return 1;
}

static int metadata_valid(const struct a733_video_jpeg *j)
{
    if (!j->width || !j->height || j->width > 16384 || j->height > 16384 ||
        !j->scan_bytes || j->file_bytes < 2 || j->scan_offset >= j->file_bytes ||
        j->scan_bytes != j->file_bytes - j->scan_offset) return 0;
    for (unsigned c = 0; c < 3; ++c) {
        unsigned q = j->quant_select[c], d = j->dc_select[c], a = j->ac_select[c];
        if (q > 1 || d > 1 || a > 1 || !j->quant_present[q] ||
            !huffman_valid(&j->huffman[0][d], 0) ||
            !huffman_valid(&j->huffman[1][a], 1)) return 0;
        for (unsigned i = 0; i < 64; ++i) if (!j->quant[q][i]) return 0;
    }
    return j->quant_select[1] == j->quant_select[2] &&
           j->dc_select[1] == j->dc_select[2] &&
           j->ac_select[1] == j->ac_select[2];
}

int a733_video_jpeg_parse(const uint8_t *data, size_t bytes,
                          struct a733_video_jpeg *out)
{
    struct a733_video_jpeg j = {0};
    size_t pos = 2;
    unsigned have_frame = 0;
    if (!data || !out || bytes < 4 || bytes > UINT32_MAX / 8u ||
        data[0] != 0xff || data[1] != 0xd8) return 0;
    while (pos < bytes) {
        if (data[pos++] != 0xff) return 0;
        while (pos < bytes && data[pos] == 0xff) ++pos;
        if (pos == bytes) return 0;
        unsigned marker = data[pos++];
        if (bytes - pos < 2) return 0;
        unsigned length = be16(data + pos);
        if (length < 2 || length > bytes - pos) return 0;
        const uint8_t *p = data + pos + 2;
        unsigned n = length - 2;
        pos += length;
        if (marker == 0xdb) {
            while (n) {
                unsigned id = *p++;
                if (id > 1 || n < 65) return 0;
                for (unsigned i = 0; i < 64; ++i) {
                    if (!p[i]) return 0;
                    j.quant[id][i] = p[i];
                }
                j.quant_present[id] = 1;
                p += 64; n -= 65;
            }
        } else if (marker == 0xc4) {
            while (n) {
                unsigned id = *p++, count = 0;
                if (n < 17 || (id & ~0x11u)) return 0;
                struct a733_video_huffman h = {0};
                for (unsigned i = 0; i < 16; ++i) count += h.counts[i] = p[i];
                p += 16; n -= 17;
                if (count > 256 || count > n) return 0;
                h.count = (uint16_t)count; h.present = 1;
                for (unsigned i = 0; i < count; ++i) h.symbols[i] = p[i];
                if (!huffman_valid(&h, id >> 4)) return 0;
                j.huffman[id >> 4][id & 1] = h;
                p += count; n -= count;
            }
        } else if (marker == 0xc0) {
            if (have_frame || n != 15 || p[0] != 8 || p[5] != 3) return 0;
            j.height = (uint16_t)be16(p + 1); j.width = (uint16_t)be16(p + 3);
            for (unsigned c = 0; c < 3; ++c) {
                if (p[6 + c * 3] != c + 1 ||
                    p[7 + c * 3] != (c ? 0x11 : 0x22) || p[8 + c * 3] > 1)
                    return 0;
                j.quant_select[c] = p[8 + c * 3];
            }
            have_frame = 1;
        } else if (marker == 0xdd) {
            if (n != 2) return 0;
            j.restart_interval = (uint16_t)be16(p);
        } else if (marker == 0xda) {
            if (!have_frame || n != 10 || p[0] != 3 || p[7] || p[8] != 63 || p[9])
                return 0;
            for (unsigned c = 0; c < 3; ++c) {
                if (p[1 + c * 2] != c + 1 || (p[2 + c * 2] & ~0x11u)) return 0;
                j.dc_select[c] = p[2 + c * 2] >> 4;
                j.ac_select[c] = p[2 + c * 2] & 15;
            }
            j.scan_offset = (uint32_t)pos;
            j.scan_bytes = (uint32_t)(bytes - pos); /* Includes EOI, like reference. */
            j.file_bytes = (uint32_t)bytes;
            if (!metadata_valid(&j)) return 0;
            unsigned restarts = 0, entropy = 0;
            while (pos < bytes) {
                if (data[pos++] != 0xff) { ++entropy; continue; }
                unsigned fill = 0;
                while (pos < bytes && data[pos] == 0xff) { ++pos; ++fill; }
                if (pos == bytes) return 0;
                unsigned m = data[pos++];
                if (m == 0) {
                    if (fill) return 0;
                    ++entropy;
                } else if (m >= 0xd0 && m <= 0xd7) {
                    if (!j.restart_interval || !entropy || m != 0xd0 + (restarts & 7))
                        return 0;
                    ++restarts; entropy = 0;
                } else if (m == 0xd9) {
                    unsigned mcus = ((j.width + 15u) / 16u) * ((j.height + 15u) / 16u);
                    unsigned expected = j.restart_interval ? (mcus - 1) / j.restart_interval : 0;
                    if (pos != bytes || !entropy || restarts != expected) return 0;
                    *out = j;
                    return 1;
                } else return 0;
            }
            return 0;
        } else if (marker >= 0xe0 && marker <= 0xef) {
            /* Adobe APP14 explicitly distinguishes RGB from YCbCr. */
            if (marker == 0xee && n >= 5 && p[0] == 'A' && p[1] == 'd' &&
                p[2] == 'o' && p[3] == 'b' && p[4] == 'e' &&
                (n != 12 || p[11] != 1)) return 0;
        } else if (marker != 0xfe) return 0;
    }
    return 0;
}

static void put_byte(uint32_t *words, unsigned off, unsigned value)
{
    words[off / 4] |= (uint32_t)(value & 255u) << ((off % 4) * 8);
}

int a733_video_jpeg_prepare(const struct a733_video_jpeg *j,
                            struct a733_video_jpeg_tables *out)
{
    struct a733_video_jpeg_tables t = {0};
    if (!j || !out || !metadata_valid(j)) return 0;
    for (unsigned i = 0; i < 64; ++i) {
        t.quant[i] = 0x4000u | (i << 8) | j->quant[j->quant_select[0]][i];
        t.quant[64 + i] = (i << 8) | j->quant[j->quant_select[1]][i];
    }
    for (unsigned slot = 0; slot < 4; ++slot) {
        unsigned c = slot / 2, ac = slot % 2;
        unsigned id = ac ? j->ac_select[c] : j->dc_select[c];
        const struct a733_video_huffman *h = &j->huffman[ac][id];
        unsigned code = 0, total = 0, last = 15;
        while (!h->counts[last]) --last;
        for (unsigned i = 0; i < 16; ++i) {
            unsigned first = i <= last ? code : 0xffffu;
            put_byte(t.huffman, slot * 64 + i * 2, first);
            put_byte(t.huffman, slot * 64 + i * 2 + 1, first >> 8);
            put_byte(t.huffman, slot * 64 + 32 + i, total);
            total += h->counts[i];
            code = (code + h->counts[i]) << 1;
        }
        for (unsigned i = 0; i < h->count; ++i)
            put_byte(t.huffman, 1024 + slot * 256 + i, h->symbols[i]);
    }
    t.size = (((j->height + 15u) / 16u - 1u) << 16) |
             ((j->width + 15u) / 16u - 1u);
    t.restart = j->restart_interval;
    t.format = 3;
    *out = t;
    return 1;
}
