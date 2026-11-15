/* g2d-check: the G2D board check on the SunstoneOS interface (include/sunstone_g2d.h, docs/g2d-os.md). It opens the
 * engine, which powers, clocks and resets it and reads its identity; runs two copies from the large region "source"
 * into the large region "surface" in one session, the first converting ARGB8888 to BGRA8888 and the second
 * unconverted into a rectangle that overlaps the first; closes the engine; and returns the open, run and close records
 * with the CRC-32 of both surfaces before and after. The geometry, both rectangles and both surfaces' contents all
 * come from one seed, from which apps/g2d-check/host.py computes the expected CRC-32 values and decodes the result. */
#include "g2d_check-app.h"
#include "sunstone_g2d.h"
#include "sunstone_large.h"
#include <string.h>

#define G2D_CHECK_MAGIC UINT32_C(0x4b433247) /* "G2CK" */
#define G2D_CHECK_SEED UINT32_C(0xbb67ae85)
#define G2D_CHECK_OPS 2u
enum {G2D_CHECK_OPENED = 1, G2D_CHECK_REFUSED = 2, G2D_CHECK_CLOSE_FAILED = 3, G2D_CHECK_RUN_FAILED = 4,
      G2D_CHECK_NO_SURFACE = 5};
/* One operation: its rectangle and formats, its result, and six output words after it: the rectangle's first and
 * last pixels, the pixel before the first (or the row's last padding word when the rectangle starts at x 0), the
 * first row's padding word after the last pixel, and the output at the first source pixel (in row order within the
 * rectangle) whose alpha byte is 0 and at the first whose alpha byte is 0xff. transparent and opaque are those two
 * pixels' indices within the rectangle, all ones when the rectangle has none. */
struct g2d_check_op {
    uint32_t result, sx, sy, dx, dy, w, h, format, output_format, transparent, opaque, reserved;
    uint32_t sample[6];
    struct ss_g2d_done done;
};
struct g2d_check_plan {
    uint32_t seed, width, height, stride, output_crc, source_crc, output_before_crc, source_before_crc;
};
struct g2d_check_result {
    uint32_t magic, stage, status, open_result, close_result, fault_step, fault_value, reserved;
    struct ss_g2d_info info;
    struct ss_g2d_closed closed;
    struct g2d_check_plan plan;
    struct g2d_check_op op[G2D_CHECK_OPS];
};
_Static_assert(sizeof(struct g2d_check_result) <= 2048, "the result fits the manifest's output_limit");

static uint32_t next(uint32_t *x)
{
    uint32_t v = *x;
    v ^= v << 13; v ^= v >> 17; v ^= v << 5;
    return *x = v;
}
/* CRC-32 (IEEE 802.3, reflected, as zlib.crc32), one table lookup per byte. */
static uint32_t crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (unsigned k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xffffffffu;
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

/* Geometry and both rectangles from the seed (host.py repeats this sequence). The surfaces' rows carry 64 bytes of
 * padding, which the copies must leave alone. Operation 1 converts ARGB8888 (0) to BGRA8888 (3); operation 2 copies
 * ARGB8888 unconverted. */
static void plan(struct g2d_check_result *r, uint32_t *x)
{
    r->plan.width = 320 + next(x) % 321;     /* 320..640 */
    r->plan.height = 180 + next(x) % 181;    /* 180..360 */
    r->plan.stride = r->plan.width * 4 + 64;
    for (unsigned k = 0; k < G2D_CHECK_OPS; ++k) {
        struct g2d_check_op *o = &r->op[k];
        o->w = 1 + next(x) % r->plan.width;
        o->h = 1 + next(x) % r->plan.height;
        o->sx = next(x) % (r->plan.width - o->w + 1);
        o->sy = next(x) % (r->plan.height - o->h + 1);
        o->dx = next(x) % (r->plan.width - o->w + 1);
        o->dy = next(x) % (r->plan.height - o->h + 1);
        o->format = 0;
        o->output_format = k == 0 ? 3 : 0;
    }
}
/* The index, in row order within the copy's source rectangle, of the first ARGB8888 pixel whose alpha byte is alpha;
 * all ones when there is none. */
static uint32_t find_alpha(const uint8_t *source, uint32_t stride, const struct g2d_check_op *o, uint32_t alpha)
{
    for (uint32_t i = 0; i < o->w * o->h; ++i) {
        uint32_t v;
        memcpy(&v, source + (size_t)(o->sy + i / o->w) * stride + (size_t)(o->sx + i % o->w) * 4, 4);
        if (v >> 24 == alpha) return i;
    }
    return UINT32_MAX;
}
static uint8_t *region(unsigned index, size_t bytes)
{
    int ready;
    do { ss_progress(); ready = ss_large_wait(index, 100000); } while (ready == 0 && ss_live() >= 0);
    size_t capacity = 0;
    uint8_t *base = ready == 1 ? ss_large_base(index, &capacity) : NULL;
    return base && capacity >= bytes ? base : NULL;
}

void g2d_check_main(const struct ss_grant *grant)
{
    (void)grant;
    static struct g2d_check_result result;
    result.magic = G2D_CHECK_MAGIC;
    result.stage = 3;
    uint32_t x = G2D_CHECK_SEED;
    result.plan.seed = G2D_CHECK_SEED;
    plan(&result, &x);
    const size_t bytes = (size_t)result.plan.stride * result.plan.height;
    /* The regions have no source: the loader marks them ready after the application starts. */
    uint8_t *source = region(G2D_CHECK_LARGE_SOURCE, bytes), *surface = region(G2D_CHECK_LARGE_SURFACE, bytes);
    if (!source || !surface) {
        result.status = G2D_CHECK_NO_SURFACE;
        ss_finish(&result, sizeof(result));
        return;
    }
    /* The source, then the output's background: the seed's sequence continued, one word per 4 bytes, padding
     * included. */
    for (unsigned k = 0; k < 2; ++k) {
        uint8_t *p = k ? surface : source;
        for (size_t i = 0; i < bytes; i += 4) {
            const uint32_t v = next(&x);
            memcpy(p + i, &v, 4);
        }
    }
    result.plan.source_before_crc = crc32(source, bytes);
    result.plan.output_before_crc = crc32(surface, bytes);
    result.open_result = (uint32_t)finish_call(open_call, &result.info);
    if ((int)result.open_result != 1) {
        result.status = G2D_CHECK_REFUSED;
        (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
        ss_finish(&result, sizeof(result));
        return;
    }
    const uint32_t width = result.plan.width, height = result.plan.height, stride = result.plan.stride;
    int failed = 0;
    for (unsigned k = 0; k < G2D_CHECK_OPS && !failed; ++k) {
        struct g2d_check_op *o = &result.op[k];
        job = (struct ss_g2d_job){.op = SS_G2D_BLIT,
            .source = {G2D_CHECK_LARGE_SOURCE, o->format, 0, width, height, stride, 0},
            .output = {G2D_CHECK_LARGE_SURFACE, o->output_format, 0, width, height, stride, 0},
            .sx = o->sx, .sy = o->sy, .dx = o->dx, .dy = o->dy, .w = o->w, .h = o->h};
        o->result = (uint32_t)finish_call(run_call, &o->done);
        if ((int)o->result != 1) {
            failed = 1;
            (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
            break;
        }
        const size_t first = (size_t)o->dy * stride + (size_t)o->dx * 4;
        const size_t last = (size_t)(o->dy + o->h - 1) * stride + (size_t)(o->dx + o->w - 1) * 4;
        const size_t pad = (size_t)o->dy * stride + (size_t)width * 4;
        memcpy(&o->sample[0], surface + first, 4);
        memcpy(&o->sample[1], surface + last, 4);
        memcpy(&o->sample[2], surface + (o->dx ? first - 4 : (size_t)o->dy * stride + stride - 4), 4);
        memcpy(&o->sample[3], surface + pad, 4);
        o->transparent = find_alpha(source, stride, o, 0);
        o->opaque = find_alpha(source, stride, o, 0xff);
        const uint32_t pick[2] = {o->transparent, o->opaque};
        for (unsigned n = 0; n < 2; ++n)
            if (pick[n] != UINT32_MAX)
                memcpy(&o->sample[4 + n], surface + (size_t)(o->dy + pick[n] / o->w) * stride +
                       (size_t)(o->dx + pick[n] % o->w) * 4, 4);
    }
    if (!failed) {
        result.plan.output_crc = crc32(surface, bytes);
        result.plan.source_crc = crc32(source, bytes);
    }
    result.close_result = (uint32_t)finish_call(close_call, &result.closed);
    result.status = failed ? G2D_CHECK_RUN_FAILED :
                    (int)result.close_result == 1 ? G2D_CHECK_OPENED : G2D_CHECK_CLOSE_FAILED;
    if (!failed) (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
    ss_finish(&result, sizeof(result));
}
