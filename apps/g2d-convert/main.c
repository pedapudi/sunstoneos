/* g2d-convert: the packed conversion stage, board stage 10 (docs/g2d-os.md#packed-conversion-stage). USB cameras
 * deliver packed 4:2:2 frames, two pixels in 4 bytes. The stage converts 640 by 480 packed frames to ARGB8888 with the
 * BT.601 limited table, the UVC default matrix, in six operations:
 * 1. frame P stored as YUYV, read with format 0x20, unscaled;
 * 2. the same pixels stored as UYVY, read with format 0x21, unscaled;
 * 3. frame R, read with 0x20 and scaled to 224 by 250 with the vendor's settings (the linear vertical luma bank and
 *    filter_type 0);
 * 4. frame R to 224 by 250 with the Lanczos vertical variant (the Lanczos bank of the vertical step and filter_type
 *    1, as for a semi-planar source);
 * 5. frame Q, read with 0x20 and scaled to 224 by 224 with the vendor's settings;
 * 6. frame Q to 224 by 224 with the Lanczos vertical variant.
 * Last, one A76 core converts frame P from YUYV in C, three times, as the CPU reference.
 *
 * Frame P is stage 9's luma-sweep frame from a new seed: random luma, except every fourth row, which steps through
 * every value from 0 to 255 across the exact region; chroma constant along each row in the left half and random pair
 * by pair in the right half. In the exact region (pixels 8 to 311 of every row) no chroma is interpolated, so there
 * each pixel's output follows from its own bytes once the byte order is known. An even pixel sits on its pair, where
 * the scaler's phase is 0 and its chroma is the pair's own, so the application counts, for each of the four packed
 * orders, the even pixels of the whole frame that differ from the table applied to their pair's bytes read in that
 * order: under the engine's order the count is 0 whatever the chroma interpolation is.
 * Frame R is constant along each row, and its luma and chroma ramp down the frame (7, 5 and 3 a row, wrapping at 256),
 * so its scaled output isolates the scaler's vertical pass. Its outputs are 250 rows tall, not 224: at 250 rows the
 * Lanczos taps of the first row read row -1 and those of the last row read row 480, so both edges are exercised, and
 * the positions' fractions are not all whole phases, so the phase rounding is too. Frame Q is random in every byte.
 *
 * Every output rectangle starts filled with 0x5a bytes, and each output row has 64 bytes of padding. After each
 * operation, also one that fails, the application records the CRC-32 of the output rectangle, of its interior (rows
 * and columns 2 to the size minus 3) and of the padding, the alpha bytes, two whole rows of each unscaled conversion,
 * column 112 of each frame-R scale with the count of its rows that are not constant, and the whole output of each
 * frame-Q scale. It stops at the first operation that fails, closes the engine and returns the records.
 * apps/g2d-convert/host.py rebuilds the frames from the seed and decodes the result. */
#include "g2d_convert-app.h"
#include "sunstone_g2d.h"
#include "sunstone_large.h"
#include <string.h>
#include "a733_g2d_csc_tables.h"

#define G2D_CONVERT_MAGIC UINT32_C(0x56433247) /* "G2CV" */
#define G2D_CONVERT_SEED UINT32_C(0xbb67ae85)
#define FW 640u
#define FH 480u
#define PACKED_STRIDE (FW * 2u)
#define FRAME_P UINT64_C(0)                     /* YUYV */
#define FRAME_PU UINT64_C(0x100000)             /* frame P's pixels as UYVY */
#define FRAME_R UINT64_C(0x200000)
#define FRAME_Q UINT64_C(0x300000)
#define SOURCE_BYTES 0x400000u
#define CONV_STRIDE (FW * 4u + 64u)
#define REF_STRIDE (FW * 4u)
#define EXACT_X0 8u
#define EXACT_X1 312u
#define SW 224u
#define SH 224u
#define RH 250u                                 /* frame R's output height */
#define SCALE_STRIDE (SW * 4u + 64u)
#define COLUMN 112u
#define OPS 6u
#define BACKGROUND 0x5au
/* The Lanczos vertical variant, A733_G2D_CONVERT_LANCZOS_VERTICAL (include/a733_g2d_convert.h), passed in the job's
 * reserved word; only this stage's image accepts it. */
#define LANCZOS_VERTICAL 1u
enum {DONE = 1, REFUSED = 2, CLOSE_FAILED = 3, RUN_FAILED = 4, NO_SURFACE = 5};
enum {UNSCALED = 0, RAMP = 1, RANDOM = 2};
/* The six operations: the packed format, the frame and its kind, the output size, stride and offset in the region
 * "surface", and the variant. Both unscaled conversions share one output rectangle, refilled before each. */
static const struct {uint32_t format; uint64_t frame; uint32_t kind, ow, oh, stride, offset, variant;} plan[OPS] = {
    {SS_G2D_FORMAT_YUYV, FRAME_P, UNSCALED, FW, FH, CONV_STRIDE, 0, 0},
    {SS_G2D_FORMAT_UYVY, FRAME_PU, UNSCALED, FW, FH, CONV_STRIDE, 0, 0},
    {SS_G2D_FORMAT_YUYV, FRAME_R, RAMP, SW, RH, SCALE_STRIDE, 0x140000, 0},
    {SS_G2D_FORMAT_YUYV, FRAME_R, RAMP, SW, RH, SCALE_STRIDE, 0x180000, LANCZOS_VERTICAL},
    {SS_G2D_FORMAT_YUYV, FRAME_Q, RANDOM, SW, SH, SCALE_STRIDE, 0x1c0000, 0},
    {SS_G2D_FORMAT_YUYV, FRAME_Q, RANDOM, SW, SH, SCALE_STRIDE, 0x200000, LANCZOS_VERTICAL}};
static const uint32_t conv_rows[2] = {100, 333};
/* The byte positions of Y0, U, Y1 and V in a 4-byte pair for each packed order, the order of the format codes: YUYV,
 * UYVY, YVYU, VYUY. */
static const uint8_t order_bytes[4][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {0, 3, 2, 1}, {1, 2, 3, 0}};
/* One operation's record. Unscaled conversions: mismatch[k] counts the even pixels whose R, G and B bytes differ from
 * the table applied to their pair's bytes read in packed order k (YUYV, UYVY, YVYU, VYUY), and order is the first
 * order with none plus one, 0 for none. Frame-R scales: nonconstant_rows counts output rows whose words are
 * not all equal. alpha_ff counts the output rectangle's words with alpha 0xff and alpha_other is the first other word.
 */
struct g2d_convert_op {
    uint32_t result, csc, format, variant, w, h, ow, oh, out_offset, out_stride;
    uint32_t rect_crc, exact_crc, interior_crc, padding_crc, alpha_ff, alpha_other;
    uint32_t mismatch[4], order, nonconstant_rows, kind, reserved;
    struct ss_g2d_done done;
};
struct g2d_convert_result {
    uint32_t magic, stage, status, open_result, close_result, fault_step, fault_value, ops_run;
    uint32_t seed, main_core, clock_ready, reserved0;
    uint64_t start_ticks, clock_ready_ticks;
    uint32_t source_crc_before, source_crc_after, cpu_ticks[3], cpu_crc, cpu_exact_crc, reserved1;
    char clock_line[128];
    struct ss_g2d_info info;
    struct ss_g2d_closed closed;
    struct g2d_convert_op op[OPS];
    uint8_t conv_row[2][2][FW * 4];
    uint32_t column[2][RH];
    uint8_t scaled[2][SW * SH * 4];
};
_Static_assert(sizeof(struct g2d_convert_op) == 344 && offsetof(struct g2d_convert_op, done) == 96,
               "apps/g2d-convert/host.py decodes this layout");
_Static_assert(offsetof(struct g2d_convert_result, clock_line) == 96 && offsetof(struct g2d_convert_result, info) == 224 &&
               offsetof(struct g2d_convert_result, closed) == 616 && offsetof(struct g2d_convert_result, op) == 648 &&
               offsetof(struct g2d_convert_result, conv_row) == 2712 && sizeof(struct g2d_convert_result) == 416360,
               "apps/g2d-convert/host.py decodes this layout");

static uint32_t next(uint32_t *x)
{
    uint32_t v = *x;
    v ^= v << 13; v ^= v >> 17; v ^= v << 5;
    return *x = v;
}
/* CRC-32 (IEEE 802.3, reflected, as zlib.crc32), continued from crc. */
static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (unsigned k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = crc ^ 0xffffffffu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
/* The CRC-32 of columns x0 to x1 - 1 (4 bytes each) of rows y0 to y1 - 1, row by row. */
static uint32_t rect_crc(const uint8_t *p, uint32_t stride, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
{
    uint32_t c = 0;
    for (uint32_t y = y0; y < y1; ++y) c = crc32(c, p + (size_t)y * stride + x0 * 4, (size_t)(x1 - x0) * 4);
    return c;
}
static int32_t clamp8(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
/* The table applied to one pixel: ARGB8888 with alpha 0xff. The table's constants round to nearest
 * (tools/g2d-csc-tables.py). */
static uint32_t csc(const int32_t *t, int32_t y, int32_t u, int32_t v)
{
    uint32_t word = UINT32_C(0xff) << 24;
    for (unsigned c = 0; c < 3; ++c)
        word |= (uint32_t)clamp8((t[4 * c] * y + t[4 * c + 1] * u + t[4 * c + 2] * v + t[4 * c + 3]) >> 10)
                << (16 - 8 * c);
    return word;
}
static const int32_t *table(uint32_t c) { return a733_g2d_csc_tables[c - 1]; }
/* The CPU conversion of a YUYV frame under the engine's model: the chroma of an odd pixel interpolated from four pairs
 * with the scaler's half-phase taps (-4, 36, 36, -4) / 64, rounded to nearest, with the edge pairs repeated, and the
 * row's last pixel taking its own pair. */
static void cpu_convert(const uint8_t *frame, uint8_t *out, const int32_t *t)
{
    for (uint32_t y = 0; y < FH; ++y) {
        const uint8_t *p = frame + (size_t)y * PACKED_STRIDE;
        uint32_t *o = (uint32_t *)(void *)(out + (size_t)y * REF_STRIDE);
        for (uint32_t x = 0; x < FW; x += 2) {
            const uint32_t k = x >> 1, a = k ? k - 1 : 0, b = k + 1 < FW / 2 ? k + 1 : k, d = k + 2 < FW / 2 ? k + 2 : FW / 2 - 1;
            const int32_t u = p[4 * k + 1], v = p[4 * k + 3];
            int32_t ui = clamp8((-4 * p[4 * a + 1] + 36 * u + 36 * p[4 * b + 1] - 4 * p[4 * d + 1] + 32) >> 6);
            int32_t vi = clamp8((-4 * p[4 * a + 3] + 36 * v + 36 * p[4 * b + 3] - 4 * p[4 * d + 3] + 32) >> 6);
            if (k == FW / 2 - 1) { ui = u; vi = v; }
            o[x] = csc(t, p[4 * k], u, v);
            o[x + 1] = csc(t, p[4 * k + 2], ui, vi);
        }
    }
}
static int finish_call(int (*call)(void *), void *arg)
{
    for (;;) {
        const int r = call(arg);
        if (r) return r;
        if (ss_live() < 0) ss_fail(1);
        ss_progress();
        ss_g2d_wait(100000);
    }
}
static int open_call(void *info) { return ss_g2d_open(info); }
static int close_call(void *closed) { return ss_g2d_close(closed); }
static struct ss_g2d_job job;
static int run_call(void *done) { return ss_g2d_run(&job, done); }
static uint8_t *region(unsigned index, size_t bytes)
{
    int ready;
    do { ss_progress(); ready = ss_large_wait(index, 100000); } while (ready == 0 && ss_live() >= 0);
    size_t capacity = 0;
    uint8_t *base = ready == 1 ? ss_large_base(index, &capacity) : NULL;
    return base && capacity >= bytes ? base : NULL;
}
static uint32_t word(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
/* Frame P: luma from the seed, 4 bytes a word, then every fourth row replaced by (x - 8 + y) mod 256, so each such
 * row's exact region holds every value; then per row one chroma pair for the left half and one pair per two pixels of
 * the right half, each from one word (U the low byte, V the next). It is stored as YUYV at FRAME_P and as UYVY at
 * FRAME_PU. Frame R: row y has luma 7y, U 64 + 5y and V 200 + 3y, each mod 256. Frame Q: the seed's words. */
static void frames(uint8_t *source)
{
    static uint8_t luma[FW * FH], chroma[FW * FH];
    uint32_t x = G2D_CONVERT_SEED;
    for (size_t i = 0; i < (size_t)FW * FH; i += 4) { const uint32_t v = next(&x); memcpy(luma + i, &v, 4); }
    for (uint32_t y = 0; y < FH; y += 4)
        for (uint32_t i = 0; i < FW; ++i) luma[(size_t)y * FW + i] = (uint8_t)(i - EXACT_X0 + y);
    for (uint32_t y = 0; y < FH; ++y) {
        uint8_t *c = chroma + (size_t)y * FW;
        const uint32_t row = next(&x);
        for (uint32_t k = 0; k < FW / 4; ++k) { c[2 * k] = (uint8_t)row; c[2 * k + 1] = (uint8_t)(row >> 8); }
        for (uint32_t k = FW / 4; k < FW / 2; ++k) {
            const uint32_t v = next(&x);
            c[2 * k] = (uint8_t)v; c[2 * k + 1] = (uint8_t)(v >> 8);
        }
    }
    for (size_t i = 0; i < (size_t)FW * FH; i += 2) {
        const uint8_t y0 = luma[i], y1 = luma[i + 1], u = chroma[i], v = chroma[i + 1];
        uint8_t *yuyv = source + FRAME_P + 2 * i, *uyvy = source + FRAME_PU + 2 * i;
        yuyv[0] = y0; yuyv[1] = u; yuyv[2] = y1; yuyv[3] = v;
        uyvy[0] = u; uyvy[1] = y0; uyvy[2] = v; uyvy[3] = y1;
    }
    for (uint32_t y = 0; y < FH; ++y)
        for (uint32_t k = 0; k < FW / 2; ++k) {
            uint8_t *p = source + FRAME_R + (size_t)y * PACKED_STRIDE + 4 * k;
            p[0] = p[2] = (uint8_t)(7 * y); p[1] = (uint8_t)(64 + 5 * y); p[3] = (uint8_t)(200 + 3 * y);
        }
    for (size_t i = 0; i < (size_t)PACKED_STRIDE * FH; i += 4) {
        const uint32_t v = next(&x);
        memcpy(source + FRAME_Q + i, &v, 4);
    }
}
static uint32_t source_crc(const uint8_t *source)
{
    uint32_t c = 0;
    const uint64_t frame[4] = {FRAME_P, FRAME_PU, FRAME_R, FRAME_Q};
    for (unsigned k = 0; k < 4; ++k) c = crc32(c, source + frame[k], (size_t)PACKED_STRIDE * FH);
    return c;
}
/* The byte-order counts over the even pixels: each pair's bytes read in each packed order. */
static void order_counts(struct g2d_convert_op *o, const uint8_t *out, const uint8_t *frame)
{
    const int32_t *t = table(o->csc);
    for (uint32_t y = 0; y < FH; ++y)
        for (uint32_t x = 0; x < FW; x += 2) {
            const uint8_t *p = frame + (size_t)y * PACKED_STRIDE + 2 * x;
            const uint32_t got = word(out + (size_t)y * o->out_stride + x * 4) & 0xffffffu;
            for (unsigned k = 0; k < 4; ++k) {
                const uint8_t *b = order_bytes[k];
                o->mismatch[k] += (csc(t, p[b[0]], p[b[1]], p[b[3]]) & 0xffffffu) != got;
            }
        }
    for (unsigned k = 0; k < 4 && !o->order; ++k)
        if (!o->mismatch[k]) o->order = k + 1;
}
static void measure(struct g2d_convert_op *o, const uint8_t *out, const uint8_t *frame)
{
    o->rect_crc = rect_crc(out, o->out_stride, 0, o->ow, 0, o->oh);
    o->interior_crc = rect_crc(out, o->out_stride, 2, o->ow - 2, 2, o->oh - 2);
    uint32_t c = 0;
    for (uint32_t y = 0; y < o->oh; ++y) c = crc32(c, out + (size_t)y * o->out_stride + o->ow * 4, o->out_stride - o->ow * 4);
    o->padding_crc = c;
    for (uint32_t y = 0; y < o->oh; ++y)
        for (uint32_t x = 0; x < o->ow; ++x) {
            const uint32_t v = word(out + (size_t)y * o->out_stride + x * 4);
            if (v >> 24 == 0xff) ++o->alpha_ff;
            else if (!o->alpha_other) o->alpha_other = v;
        }
    if (o->kind == UNSCALED) {
        o->exact_crc = rect_crc(out, o->out_stride, EXACT_X0, EXACT_X1, 0, FH);
        order_counts(o, out, frame);
    } else if (o->kind == RAMP)
        for (uint32_t y = 0; y < o->oh; ++y) {
            const uint8_t *row = out + (size_t)y * o->out_stride;
            for (uint32_t x = 1; x < o->ow; ++x)
                if (word(row + x * 4) != word(row)) { ++o->nonconstant_rows; break; }
        }
}

void g2d_convert_main(const struct ss_grant *grant)
{
    static struct g2d_convert_result result;
    result.magic = G2D_CONVERT_MAGIC;
    result.stage = 10;
    result.seed = G2D_CONVERT_SEED;
    result.main_core = grant ? grant->main_core : UINT32_MAX;
    result.start_ticks = ss_counter_ticks();
    uint8_t *surface = region(G2D_CONVERT_LARGE_SURFACE, 0x400000), *source = region(G2D_CONVERT_LARGE_SOURCE, SOURCE_BYTES);
    uint8_t *reference = region(G2D_CONVERT_LARGE_REFERENCE, (size_t)REF_STRIDE * FH);
    if (!surface || !source || !reference) {
        result.status = NO_SURFACE;
        ss_finish(&result, sizeof(result));
        return;
    }
    frames(source);
    result.source_crc_before = source_crc(source);
    /* The CPU clock owner finishes the grant's requests (CPU-B to its top point) before any timing; the line it
     * reports is recorded, not required. */
    while (!ss_clock_ready()) {
        if (ss_live() < 0) ss_fail(2);
        ss_progress();
        ss_wait_until(ss_counter_ticks() / 24 + 1000);
    }
    result.clock_ready = 1;
    result.clock_ready_ticks = ss_counter_ticks();
    (void)ss_clock_info(result.clock_line, sizeof(result.clock_line));
    result.open_result = (uint32_t)finish_call(open_call, &result.info);
    if ((int)result.open_result != 1) {
        result.status = REFUSED;
        (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
        ss_finish(&result, sizeof(result));
        return;
    }
    int failed = 0;
    for (unsigned k = 0; k < OPS && !failed; ++k) {
        struct g2d_convert_op *o = &result.op[k];
        memset(surface + plan[k].offset, BACKGROUND, (size_t)plan[k].stride * plan[k].oh);
        *o = (struct g2d_convert_op){.csc = SS_G2D_CSC_BT601_LIMITED, .format = plan[k].format,
                                     .variant = plan[k].variant, .w = FW, .h = FH, .ow = plan[k].ow, .oh = plan[k].oh,
                                     .out_offset = plan[k].offset, .out_stride = plan[k].stride, .kind = plan[k].kind};
        job = (struct ss_g2d_job){.op = SS_G2D_CONVERT,
            .source = {.region = G2D_CONVERT_LARGE_SOURCE, .format = plan[k].format, .offset = plan[k].frame,
                       .width = FW, .height = FH, .stride = PACKED_STRIDE},
            .output = {.region = G2D_CONVERT_LARGE_SURFACE, .format = 0, .offset = plan[k].offset, .width = plan[k].ow,
                       .height = plan[k].oh, .stride = plan[k].stride},
            .w = FW, .h = FH, .ow = plan[k].ow, .oh = plan[k].oh, .csc = SS_G2D_CSC_BT601_LIMITED,
            .reserved = plan[k].variant};
        o->result = (uint32_t)finish_call(run_call, &o->done);
        ++result.ops_run;
        /* Measured after a failed operation too: the output lines were invalidated before the launch. */
        const uint8_t *out = surface + plan[k].offset;
        measure(o, out, source + plan[k].frame);
        if (o->kind == UNSCALED)
            for (unsigned r = 0; r < 2; ++r) memcpy(result.conv_row[k][r], out + (size_t)conv_rows[r] * o->out_stride, FW * 4);
        else if (o->kind == RAMP)
            for (uint32_t y = 0; y < RH; ++y) result.column[k - 2][y] = word(out + (size_t)y * o->out_stride + COLUMN * 4);
        else
            for (uint32_t y = 0; y < SH; ++y)
                memcpy(result.scaled[k - 4] + (size_t)y * SW * 4, out + (size_t)y * o->out_stride, SW * 4);
        if ((int)o->result != 1) {
            failed = 1;
            (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
        }
    }
    result.close_result = (uint32_t)finish_call(close_call, &result.closed);
    if (!failed) (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
    /* The CPU reference after the engine's work: the first conversion, three times, on this core. */
    for (unsigned k = 0; k < 3; ++k) {
        const uint64_t t0 = ss_counter_ticks();
        cpu_convert(source + FRAME_P, reference, table(SS_G2D_CSC_BT601_LIMITED));
        result.cpu_ticks[k] = (uint32_t)(ss_counter_ticks() - t0);
        ss_progress();
    }
    result.cpu_crc = rect_crc(reference, REF_STRIDE, 0, FW, 0, FH);
    result.cpu_exact_crc = rect_crc(reference, REF_STRIDE, EXACT_X0, EXACT_X1, 0, FH);
    result.source_crc_after = source_crc(source);
    result.status = failed ? RUN_FAILED : (int)result.close_result != 1 ? CLOSE_FAILED : DONE;
    ss_finish(&result, sizeof(result));
}
