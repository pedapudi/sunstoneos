/* g2d-convert: the third camera-frame conversion stage (docs/g2d-os.md#third-conversion-stage). It converts one
 * 640 by 480 NV16 frame, the luma-sweep frame, to ARGB8888 with each of the four tables: BT.709 full, BT.601 full,
 * BT.709 limited and BT.601 limited. Then it scales a second 640 by 480 NV16 frame, the scale frame, to 320 by 240 and
 * to 224 by 224 with the BT.601 limited table. Last, one A76 core converts the luma-sweep frame with the BT.709 full
 * table in C, three times, as the CPU reference.
 *
 * The luma-sweep frame's luma is the seed's sequence, except every fourth row, which steps through every value from 0
 * to 255 across the exact region. Its chroma is constant along each row in the left half (pixels 0 to 319) and the
 * seed's sequence pair by pair in the right half, so pixels 8 to 311 of every row (the exact region) take no
 * interpolated chroma. There the output is the table (tools/g2d-csc-tables.py) applied to each pixel's luma and its
 * row's chroma, with luma either kept (the prediction: csc0 converts at pipe 0's input) or raised to 16 (the blender
 * working in YUV). The application counts both candidates' mismatches there, and stops before the next operation when
 * a conversion does not follow the first. The scale frame is one flat colour in its left half and a luma ramp over
 * neutral chroma in its right half, the same on every row. Every output rectangle starts filled with 0x5a bytes, and
 * each output row has 64 bytes of padding.
 *
 * After each operation, also one that fails, the application records the CRC-32 of the output rectangle, of the
 * exact region and of the padding, the alpha bytes, two whole output rows of each conversion and the middle row of
 * each scale, with the counts described at struct g2d_convert_op. It stops at the first operation that fails, closes
 * the engine and returns the records. apps/g2d-convert/host.py rebuilds both frames from the seed and decodes the
 * result. */
#include "g2d_convert-app.h"
#include "sunstone_g2d.h"
#include "sunstone_large.h"
#include <string.h>
#include "a733_g2d_csc_tables.h"

#define G2D_CONVERT_MAGIC UINT32_C(0x56433247) /* "G2CV" */
#define G2D_CONVERT_SEED UINT32_C(0x6a09e667)
#define FW 640u
#define FH 480u
#define FRAME_A UINT64_C(0)                     /* luma; chroma at + CHROMA */
#define FRAME_B UINT64_C(0x100000)
#define CHROMA UINT64_C(0x50000)
#define CONV_STRIDE (FW * 4u + 64u)
#define REF_STRIDE (FW * 4u)
#define EXACT_X0 8u
#define EXACT_X1 312u
#define FLAT_Y 150u
#define FLAT_U 90u
#define FLAT_V 170u
#define OPS 6u
#define CONVERSIONS 4u
#define BACKGROUND 0x5au
enum {DONE = 1, REFUSED = 2, CLOSE_FAILED = 3, RUN_FAILED = 4, NO_SURFACE = 5, MISSED = 6};
/* The six operations: the table, the output size and stride, the output's offset in the region "surface", and the
 * frame. The conversions share one output rectangle, refilled before each. */
static const struct {uint32_t csc, ow, oh, stride, offset, frame;} plan[OPS] = {
    {SS_G2D_CSC_BT709_FULL, FW, FH, CONV_STRIDE, 0, 0},
    {SS_G2D_CSC_BT601_FULL, FW, FH, CONV_STRIDE, 0, 0},
    {SS_G2D_CSC_BT709_LIMITED, FW, FH, CONV_STRIDE, 0, 0},
    {SS_G2D_CSC_BT601_LIMITED, FW, FH, CONV_STRIDE, 0, 0},
    {SS_G2D_CSC_BT601_LIMITED, 320, 240, 320 * 4 + 64, 0x280000, 1},
    {SS_G2D_CSC_BT601_LIMITED, 224, 224, 224 * 4 + 64, 0x300000, 1}};
static const uint32_t conv_rows[2] = {100, 333};
/* One operation's record. Conversions: mismatch[k] counts the exact region's pixels whose R, G and B bytes differ from
 * candidate rule k + 1 (1 luma kept, 2 luma below 16 raised to 16; mismatch[2] and [3] stay 0), and rule is the first
 * candidate with none, 0 for none. Scales: flat_value is the flat colour under rule 1, flat_mismatch
 * counts the words of the flat region (x from 4 to ow / 2 - 8, rows 2 to oh - 3) that differ from it, rows_differ
 * counts rows 2 to oh - 3 that differ from the middle row (oh / 2), edge_rows has bit k set when row 0, 1, oh - 2 or
 * oh - 1 differs from it, and mono[c] counts the steps of the middle row's ramp (x from ow / 2 + 8 to ow - 5) where
 * channel c (R, G, B) decreases. alpha_ff counts the output rectangle's words with alpha 0xff and alpha_other is the
 * first other word. */
struct g2d_convert_op {
    uint32_t result, csc, w, h, ow, oh, out_offset, out_stride;
    uint32_t rect_crc, exact_crc, padding_crc, alpha_ff, alpha_other;
    uint32_t mismatch[4], rule, flat_value, flat_mismatch, rows_differ, edge_rows, mono[3], kind, reserved[2];
    struct ss_g2d_done done;
};
struct g2d_convert_result {
    uint32_t magic, stage, status, open_result, close_result, fault_step, fault_value, ops_run;
    uint32_t seed, main_core, clock_ready, stopped;
    uint64_t start_ticks, clock_ready_ticks;
    uint32_t source_crc_before, source_crc_after, cpu_ticks[3], cpu_crc, cpu_exact_crc, cpu_rule;
    char clock_line[128];
    struct ss_g2d_info info;
    struct ss_g2d_closed closed;
    struct g2d_convert_op op[OPS];
    uint8_t conv_row[CONVERSIONS][2][FW * 4];
    uint8_t scale_row[(320 + 224) * 4];
};
_Static_assert(sizeof(struct g2d_convert_op) == 360 && offsetof(struct g2d_convert_op, done) == 112,
               "apps/g2d-convert/host.py decodes this layout");
_Static_assert(offsetof(struct g2d_convert_result, clock_line) == 96 && offsetof(struct g2d_convert_result, info) == 224 &&
               offsetof(struct g2d_convert_result, closed) == 616 && offsetof(struct g2d_convert_result, op) == 648 &&
               offsetof(struct g2d_convert_result, conv_row) == 2808 && sizeof(struct g2d_convert_result) == 25464,
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
/* The CRC-32 of columns x0 to x1 - 1 (4 bytes each) of rows 0 to rows - 1, row by row. */
static uint32_t rect_crc(const uint8_t *p, uint32_t stride, uint32_t x0, uint32_t x1, uint32_t rows)
{
    uint32_t c = 0;
    for (uint32_t y = 0; y < rows; ++y) c = crc32(c, p + (size_t)y * stride + x0 * 4, (size_t)(x1 - x0) * 4);
    return c;
}
static int32_t clamp8(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
/* The table applied to one pixel under a candidate rule (1 luma kept, 2 luma below 16 raised to 16): ARGB8888 with
 * alpha 0xff. The table's constants round to nearest (tools/g2d-csc-tables.py). */
static uint32_t csc(const int32_t *t, int32_t y, int32_t u, int32_t v, uint32_t rule)
{
    if (rule == 2 && y < 16) y = 16;
    uint32_t word = UINT32_C(0xff) << 24;
    for (unsigned c = 0; c < 3; ++c)
        word |= (uint32_t)clamp8((t[4 * c] * y + t[4 * c + 1] * u + t[4 * c + 2] * v + t[4 * c + 3]) >> 10)
                << (16 - 8 * c);
    return word;
}
static const int32_t *table(uint32_t c) { return a733_g2d_csc_tables[c - 1]; }
/* The CPU conversion of a frame under the engine's model: rule 1, the chroma of an odd pixel interpolated from four pairs with the scaler's half-phase taps (-4, 36, 36, -4) / 64,
 * rounded to nearest, with the edge pairs repeated, and the row's last pixel taking its own pair. */
static void cpu_convert(const uint8_t *luma, const uint8_t *chroma, uint8_t *out, const int32_t *t)
{
    for (uint32_t y = 0; y < FH; ++y) {
        const uint8_t *l = luma + (size_t)y * FW, *c = chroma + (size_t)y * FW;
        uint32_t *o = (uint32_t *)(void *)(out + (size_t)y * REF_STRIDE);
        for (uint32_t x = 0; x < FW; x += 2) {
            const uint32_t k = x >> 1, a = k ? k - 1 : 0, b = k + 1 < FW / 2 ? k + 1 : k, d = k + 2 < FW / 2 ? k + 2 : FW / 2 - 1;
            const int32_t u = c[2 * k], v = c[2 * k + 1];
            int32_t ui = clamp8((-4 * c[2 * a] + 36 * u + 36 * c[2 * b] - 4 * c[2 * d] + 32) >> 6);
            int32_t vi = clamp8((-4 * c[2 * a + 1] + 36 * v + 36 * c[2 * b + 1] - 4 * c[2 * d + 1] + 32) >> 6);
            if (k == FW / 2 - 1) { ui = u; vi = v; }
            o[x] = csc(t, l[x], u, v, 1);
            o[x + 1] = csc(t, l[x + 1], ui, vi, 1);
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
/* The luma-sweep frame (FRAME_A): luma from the seed, 4 bytes a word, then every fourth row replaced by
 * (x - 8 + y) mod 256, so each such row's exact region holds every value; then per row one chroma pair for the left
 * half and one pair per two pixels of the right half, each from one word (U the low byte, V the next). The scale frame
 * (FRAME_B): flat left, ramp right. */
static void frames(uint8_t *source)
{
    uint32_t x = G2D_CONVERT_SEED;
    uint8_t *luma = source + FRAME_A, *chroma = source + FRAME_A + CHROMA;
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
    luma = source + FRAME_B; chroma = source + FRAME_B + CHROMA;
    for (uint32_t y = 0; y < FH; ++y)
        for (uint32_t i = 0; i < FW; ++i) {
            const int right = i >= FW / 2;
            luma[(size_t)y * FW + i] = (uint8_t)(right ? 16 + (i - FW / 2) * 219 / (FW / 2 - 1) : FLAT_Y);
            chroma[(size_t)y * FW + i] = (uint8_t)(right ? 128 : i & 1 ? FLAT_V : FLAT_U);
        }
}
static uint32_t source_crc(const uint8_t *source)
{
    uint32_t c = 0;
    const uint64_t planes[4] = {FRAME_A, FRAME_A + CHROMA, FRAME_B, FRAME_B + CHROMA};
    for (unsigned k = 0; k < 4; ++k) c = crc32(c, source + planes[k], (size_t)FW * FH);
    return c;
}
/* The conversion counts: each candidate's mismatches over the exact region, against the frame's row chroma. */
static void conversion_counts(struct g2d_convert_op *o, const uint8_t *out, const uint8_t *source)
{
    const int32_t *t = table(o->csc);
    for (uint32_t y = 0; y < FH; ++y) {
        const uint8_t *l = source + FRAME_A + (size_t)y * FW, *c = source + FRAME_A + CHROMA + (size_t)y * FW;
        for (uint32_t x = EXACT_X0; x < EXACT_X1; ++x) {
            const uint32_t got = word(out + (size_t)y * o->out_stride + x * 4) & 0xffffffu;
            for (unsigned k = 0; k < 2; ++k) o->mismatch[k] += (csc(t, l[x], c[0], c[1], k + 1) & 0xffffffu) != got;
        }
    }
    for (unsigned k = 0; k < 2 && !o->rule; ++k)
        if (!o->mismatch[k]) o->rule = k + 1;
}
static void scale_counts(struct g2d_convert_op *o, const uint8_t *out)
{
    const uint32_t ow = o->ow, oh = o->oh, mid = oh / 2;
    o->flat_value = csc(table(o->csc), FLAT_Y, FLAT_U, FLAT_V, 1);
    const uint8_t *middle = out + (size_t)mid * o->out_stride;
    for (uint32_t y = 0; y < oh; ++y) {
        const uint8_t *row = out + (size_t)y * o->out_stride;
        const int differs = memcmp(row, middle, (size_t)ow * 4) != 0;
        if (y >= 2 && y + 2 < oh) {
            o->rows_differ += differs;
            for (uint32_t x = 4; x + 8 < ow / 2; ++x) o->flat_mismatch += word(row + x * 4) != o->flat_value;
        } else
            o->edge_rows |= (uint32_t)differs << (y < 2 ? y : 4 - (oh - y));
    }
    for (uint32_t x = ow / 2 + 9; x + 4 < ow; ++x)
        for (unsigned c = 0; c < 3; ++c)
            o->mono[c] += middle[x * 4 + 2 - c] < middle[(x - 1) * 4 + 2 - c];
}
static void measure(struct g2d_convert_op *o, const uint8_t *out, const uint8_t *source)
{
    o->rect_crc = rect_crc(out, o->out_stride, 0, o->ow, o->oh);
    uint32_t c = 0;
    for (uint32_t y = 0; y < o->oh; ++y) c = crc32(c, out + (size_t)y * o->out_stride + o->ow * 4, o->out_stride - o->ow * 4);
    o->padding_crc = c;
    for (uint32_t y = 0; y < o->oh; ++y)
        for (uint32_t x = 0; x < o->ow; ++x) {
            const uint32_t v = word(out + (size_t)y * o->out_stride + x * 4);
            if (v >> 24 == 0xff) ++o->alpha_ff;
            else if (!o->alpha_other) o->alpha_other = v;
        }
    if (!o->kind) {
        o->exact_crc = rect_crc(out, o->out_stride, EXACT_X0, EXACT_X1, FH);
        conversion_counts(o, out, source);
    } else
        scale_counts(o, out);
}

void g2d_convert_main(const struct ss_grant *grant)
{
    static struct g2d_convert_result result;
    result.magic = G2D_CONVERT_MAGIC;
    result.stage = 9;
    result.seed = G2D_CONVERT_SEED;
    result.main_core = grant ? grant->main_core : UINT32_MAX;
    result.start_ticks = ss_counter_ticks();
    uint8_t *surface = region(G2D_CONVERT_LARGE_SURFACE, 0x400000), *source = region(G2D_CONVERT_LARGE_SOURCE, 0x200000);
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
    uint8_t *scale_row = result.scale_row;
    for (unsigned k = 0; k < OPS && !failed && !result.stopped; ++k) {
        struct g2d_convert_op *o = &result.op[k];
        const uint64_t frame = plan[k].frame ? FRAME_B : FRAME_A;
        memset(surface + plan[k].offset, BACKGROUND, (size_t)plan[k].stride * plan[k].oh);
        *o = (struct g2d_convert_op){.csc = plan[k].csc, .w = FW, .h = FH, .ow = plan[k].ow, .oh = plan[k].oh,
                                     .out_offset = plan[k].offset, .out_stride = plan[k].stride, .kind = plan[k].frame};
        job = (struct ss_g2d_job){.op = SS_G2D_CONVERT,
            .source = {.region = G2D_CONVERT_LARGE_SOURCE, .format = SS_G2D_FORMAT_NV16, .offset = frame, .width = FW,
                       .height = FH, .stride = FW, .chroma_offset = frame + CHROMA, .chroma_stride = FW},
            .output = {.region = G2D_CONVERT_LARGE_SURFACE, .format = 0, .offset = plan[k].offset, .width = plan[k].ow,
                       .height = plan[k].oh, .stride = plan[k].stride},
            .w = FW, .h = FH, .ow = plan[k].ow, .oh = plan[k].oh, .csc = plan[k].csc};
        o->result = (uint32_t)finish_call(run_call, &o->done);
        ++result.ops_run;
        /* Measured after a failed operation too: the output lines were invalidated before the launch. */
        const uint8_t *out = surface + plan[k].offset;
        measure(o, out, source);
        if (!o->kind)
            for (unsigned r = 0; r < 2; ++r) memcpy(result.conv_row[k][r], out + (size_t)conv_rows[r] * o->out_stride, FW * 4);
        else {
            memcpy(scale_row, out + (size_t)(o->oh / 2) * o->out_stride, (size_t)o->ow * 4);
            scale_row += (size_t)o->ow * 4;
        }
        if ((int)o->result != 1) {
            failed = 1;
            (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
        }
        /* Every conversion is predicted to follow rule 1 (luma kept). */
        if (!o->kind && o->rule != 1) result.stopped = 1;
    }
    result.close_result = (uint32_t)finish_call(close_call, &result.closed);
    if (!failed) (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
    /* The CPU reference after the engine's work: the first conversion, three times, on this core. */
    result.cpu_rule = 1;
    for (unsigned k = 0; k < 3; ++k) {
        const uint64_t t0 = ss_counter_ticks();
        cpu_convert(source + FRAME_A, source + FRAME_A + CHROMA, reference, table(SS_G2D_CSC_BT709_FULL));
        result.cpu_ticks[k] = (uint32_t)(ss_counter_ticks() - t0);
        ss_progress();
    }
    result.cpu_crc = rect_crc(reference, REF_STRIDE, 0, FW, FH);
    result.cpu_exact_crc = rect_crc(reference, REF_STRIDE, EXACT_X0, EXACT_X1, FH);
    result.source_crc_after = source_crc(source);
    result.status = failed ? RUN_FAILED : (int)result.close_result != 1 ? CLOSE_FAILED : result.stopped ? MISSED : DONE;
    ss_finish(&result, sizeof(result));
}
