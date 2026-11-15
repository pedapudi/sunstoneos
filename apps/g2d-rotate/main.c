/* g2d-rotate: the first rotate-module stage (docs/g2d-os.md#rotate-module). It opens the engine and rotates one 96 by
 * 40 ARGB8888 rectangle of the large region "source" into four disjoint rectangles of the large region "surface", by
 * 0, 180, 90 and 270 degrees in that order, with no flip. After each rotate it takes the CRC-32 of the output
 * rectangle (its rotated width times 4 bytes of each of its rows, top to bottom) and the output words at the
 * rectangle's four corners and just before its first pixel. It stops at the first rotate that fails, so the record
 * names that rotate and its degrees, then closes the engine and returns the open, rotate and close records with the
 * CRC-32 of both surfaces before and after. Both surfaces' contents come from one seed, from which
 * apps/g2d-rotate/host.py computes every candidate output and decodes the result. */
#include "g2d_rotate-app.h"
#include "sunstone_g2d.h"
#include "sunstone_large.h"
#include <string.h>

#define G2D_ROTATE_MAGIC UINT32_C(0x52543247) /* "G2TR" */
#define G2D_ROTATE_SEED UINT32_C(0x3c6ef372)
#define G2D_ROTATE_OPS 4u
#define G2D_ROTATE_WIDTH 400u
#define G2D_ROTATE_HEIGHT 260u
/* A multiple of 8 bytes, as the rotate module's output pitch must be. */
#define G2D_ROTATE_STRIDE (G2D_ROTATE_WIDTH * 4u + 64u)
#define G2D_ROTATE_SX 24u
#define G2D_ROTATE_SY 16u
#define G2D_ROTATE_W 96u
#define G2D_ROTATE_H 40u
enum {G2D_ROTATE_DONE = 1, G2D_ROTATE_REFUSED = 2, G2D_ROTATE_CLOSE_FAILED = 3, G2D_ROTATE_RUN_FAILED = 4,
      G2D_ROTATE_NO_SURFACE = 5};
/* The degrees and output origin of each rotate. 0 and 180 give 96 by 40 rectangles at (150, 52) and (150, 100); 90 and
 * 270 give 40 by 96 rectangles at (260, 52) and (310, 52). */
static const uint32_t plan[G2D_ROTATE_OPS][3] = {{0, 150, 52}, {180, 150, 100}, {90, 260, 52}, {270, 310, 52}};
/* One rotate: its job, its result, the CRC-32 of its output rectangle, and five output words: the rectangle's top-left,
 * top-right, bottom-left and bottom-right pixels and the pixel before its first. */
struct g2d_rotate_op {
    uint32_t result, degrees, flip, sx, sy, dx, dy, w, h, rect_crc;
    uint32_t sample[5];
    uint32_t reserved;
    struct ss_g2d_done done;
};
struct g2d_rotate_plan {
    uint32_t seed, width, height, stride, output_crc, source_crc, output_before_crc, source_before_crc;
};
struct g2d_rotate_result {
    uint32_t magic, stage, status, open_result, close_result, fault_step, fault_value, reserved;
    struct ss_g2d_info info;
    struct ss_g2d_closed closed;
    struct g2d_rotate_plan plan;
    struct g2d_rotate_op op[G2D_ROTATE_OPS];
};
_Static_assert(sizeof(struct g2d_rotate_result) <= 2048, "the result fits the manifest's output_limit");
_Static_assert(G2D_ROTATE_STRIDE % 8u == 0, "the output pitch is a multiple of 8 bytes");

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
static uint32_t word(const uint8_t *surface, uint32_t x, uint32_t y)
{
    uint32_t v;
    memcpy(&v, surface + (size_t)y * G2D_ROTATE_STRIDE + (size_t)x * 4, 4);
    return v;
}

void g2d_rotate_main(const struct ss_grant *grant)
{
    (void)grant;
    static struct g2d_rotate_result result;
    result.magic = G2D_ROTATE_MAGIC;
    result.stage = 6;
    result.plan = (struct g2d_rotate_plan){.seed = G2D_ROTATE_SEED, .width = G2D_ROTATE_WIDTH,
                                           .height = G2D_ROTATE_HEIGHT, .stride = G2D_ROTATE_STRIDE};
    const size_t bytes = (size_t)G2D_ROTATE_STRIDE * G2D_ROTATE_HEIGHT;
    uint8_t *source = region(G2D_ROTATE_LARGE_SOURCE, bytes), *surface = region(G2D_ROTATE_LARGE_SURFACE, bytes);
    if (!source || !surface) {
        result.status = G2D_ROTATE_NO_SURFACE;
        ss_finish(&result, sizeof(result));
        return;
    }
    /* The source, then the output's background: the seed's sequence, one word per 4 bytes, padding included. */
    uint32_t x = G2D_ROTATE_SEED;
    for (unsigned k = 0; k < 2; ++k) {
        uint8_t *p = k ? surface : source;
        for (size_t i = 0; i < bytes; i += 4) {
            const uint32_t v = next(&x);
            memcpy(p + i, &v, 4);
        }
    }
    result.plan.source_before_crc = crc32(0, source, bytes);
    result.plan.output_before_crc = crc32(0, surface, bytes);
    result.open_result = (uint32_t)finish_call(open_call, &result.info);
    if ((int)result.open_result != 1) {
        result.status = G2D_ROTATE_REFUSED;
        (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
        ss_finish(&result, sizeof(result));
        return;
    }
    int failed = 0;
    for (unsigned k = 0; k < G2D_ROTATE_OPS && !failed; ++k) {
        struct g2d_rotate_op *o = &result.op[k];
        *o = (struct g2d_rotate_op){.degrees = plan[k][0], .sx = G2D_ROTATE_SX, .sy = G2D_ROTATE_SY,
            .dx = plan[k][1], .dy = plan[k][2], .w = G2D_ROTATE_W, .h = G2D_ROTATE_H};
        job = (struct ss_g2d_job){.op = SS_G2D_ROTATE,
            .source = {G2D_ROTATE_LARGE_SOURCE, 0, 0, G2D_ROTATE_WIDTH, G2D_ROTATE_HEIGHT, G2D_ROTATE_STRIDE, 0},
            .output = {G2D_ROTATE_LARGE_SURFACE, 0, 0, G2D_ROTATE_WIDTH, G2D_ROTATE_HEIGHT, G2D_ROTATE_STRIDE, 0},
            .sx = o->sx, .sy = o->sy, .dx = o->dx, .dy = o->dy, .w = o->w, .h = o->h, .degrees = o->degrees,
            .flip = o->flip};
        o->result = (uint32_t)finish_call(run_call, &o->done);
        if ((int)o->result != 1) {
            failed = 1;
            (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
            break;
        }
        const int swap = o->degrees == 90 || o->degrees == 270;
        const uint32_t ow = swap ? o->h : o->w, oh = swap ? o->w : o->h;
        const uint8_t *first = surface + (size_t)o->dy * G2D_ROTATE_STRIDE + (size_t)o->dx * 4;
        uint32_t c = 0;
        for (uint32_t y = 0; y < oh; ++y) c = crc32(c, first + (size_t)y * G2D_ROTATE_STRIDE, (size_t)ow * 4);
        o->rect_crc = c;
        o->sample[0] = word(surface, o->dx, o->dy);
        o->sample[1] = word(surface, o->dx + ow - 1, o->dy);
        o->sample[2] = word(surface, o->dx, o->dy + oh - 1);
        o->sample[3] = word(surface, o->dx + ow - 1, o->dy + oh - 1);
        o->sample[4] = word(surface, o->dx - 1, o->dy);
    }
    if (!failed) {
        result.plan.output_crc = crc32(0, surface, bytes);
        result.plan.source_crc = crc32(0, source, bytes);
    }
    result.close_result = (uint32_t)finish_call(close_call, &result.closed);
    result.status = failed ? G2D_ROTATE_RUN_FAILED :
                    (int)result.close_result == 1 ? G2D_ROTATE_DONE : G2D_ROTATE_CLOSE_FAILED;
    if (!failed) (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
    ss_finish(&result, sizeof(result));
}
