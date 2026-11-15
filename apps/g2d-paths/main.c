/* g2d-paths: the G2D exact-copy experiment (docs/g2d-os.md#copy-paths). It opens the engine and copies one 96 by 40
 * ARGB8888 rectangle of the large region "source", unconverted, into five disjoint rectangles of the large region
 * "surface", one per blender path: the vendor path, then the four exact-copy candidates (sunstone_g2d.h
 * SS_G2D_PATH_*). After each copy it takes the CRC-32 of the copy's output rectangle and the output at three source
 * pixels of known alpha: the first with alpha 0, the first with alpha 0xff and the first with any other alpha. It stops
 * at the first copy that fails, so the record names that copy and its path, then closes the engine and returns the
 * open, copy and close records with the CRC-32 of both surfaces before and after. Both surfaces' contents come from one
 * seed, from which apps/g2d-paths/host.py computes every alternative output and decodes the result. */
#include "g2d_paths-app.h"
#include "sunstone_g2d.h"
#include "sunstone_large.h"
#include <string.h>

#define G2D_PATHS_MAGIC UINT32_C(0x50443247) /* "G2DP" */
#define G2D_PATHS_SEED UINT32_C(0x3c6ef372)
#define G2D_PATHS_OPS 5u
#define G2D_PATHS_WIDTH 400u
#define G2D_PATHS_HEIGHT 260u
#define G2D_PATHS_STRIDE (G2D_PATHS_WIDTH * 4u + 64u)
/* The source rectangle every copy reads, and the column and first row of the copies' output rectangles, which are
 * G2D_PATHS_STEP rows apart. */
#define G2D_PATHS_SX 24u
#define G2D_PATHS_SY 16u
#define G2D_PATHS_W 96u
#define G2D_PATHS_H 40u
#define G2D_PATHS_DX 150u
#define G2D_PATHS_DY 52u
#define G2D_PATHS_STEP 42u
enum {G2D_PATHS_DONE = 1, G2D_PATHS_REFUSED = 2, G2D_PATHS_CLOSE_FAILED = 3, G2D_PATHS_RUN_FAILED = 4,
      G2D_PATHS_NO_SURFACE = 5};
static const uint32_t paths[G2D_PATHS_OPS] = {SS_G2D_PATH_VENDOR, SS_G2D_PATH_MARKERS, SS_G2D_PATH_PREMULTIPLIED,
                                             SS_G2D_PATH_ROP_COPY, SS_G2D_PATH_NO_FILL};
/* One copy: its path and rectangles, its result, the CRC-32 of its output rectangle (w * 4 bytes of each of its h
 * rows, top to bottom), the indices in row order within the rectangle of the three pixels of known alpha (all ones when
 * there is none), and four output words: at those three pixels, and the pixel before the rectangle's first. */
struct g2d_paths_op {
    uint32_t result, path, sx, sy, dx, dy, w, h, rect_crc, alpha_0, alpha_ff, partial;
    uint32_t sample[4];
    struct ss_g2d_done done;
};
struct g2d_paths_plan {
    uint32_t seed, width, height, stride, output_crc, source_crc, output_before_crc, source_before_crc;
};
struct g2d_paths_result {
    uint32_t magic, stage, status, open_result, close_result, fault_step, fault_value, reserved;
    struct ss_g2d_info info;
    struct ss_g2d_closed closed;
    struct g2d_paths_plan plan;
    struct g2d_paths_op op[G2D_PATHS_OPS];
};
_Static_assert(sizeof(struct g2d_paths_result) <= 2048, "the result fits the manifest's output_limit");
_Static_assert(G2D_PATHS_DY + (G2D_PATHS_OPS - 1) * G2D_PATHS_STEP + G2D_PATHS_H <= G2D_PATHS_HEIGHT &&
               G2D_PATHS_STEP >= G2D_PATHS_H, "the output rectangles are disjoint and inside the surface");

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
/* The first pixel in row order of the source rectangle whose alpha byte satisfies the test; all ones when none does. */
static uint32_t find(const uint8_t *source, int (*test)(uint32_t))
{
    for (uint32_t i = 0; i < G2D_PATHS_W * G2D_PATHS_H; ++i) {
        uint32_t v;
        memcpy(&v, source + (size_t)(G2D_PATHS_SY + i / G2D_PATHS_W) * G2D_PATHS_STRIDE +
               (size_t)(G2D_PATHS_SX + i % G2D_PATHS_W) * 4, 4);
        if (test(v >> 24)) return i;
    }
    return UINT32_MAX;
}
static int transparent(uint32_t a) { return a == 0; }
static int opaque(uint32_t a) { return a == 0xff; }
static int partial(uint32_t a) { return a != 0 && a != 0xff; }

void g2d_paths_main(const struct ss_grant *grant)
{
    (void)grant;
    static struct g2d_paths_result result;
    result.magic = G2D_PATHS_MAGIC;
    result.stage = 5;
    result.plan = (struct g2d_paths_plan){.seed = G2D_PATHS_SEED, .width = G2D_PATHS_WIDTH,
                                          .height = G2D_PATHS_HEIGHT, .stride = G2D_PATHS_STRIDE};
    const size_t bytes = (size_t)G2D_PATHS_STRIDE * G2D_PATHS_HEIGHT;
    uint8_t *source = region(G2D_PATHS_LARGE_SOURCE, bytes), *surface = region(G2D_PATHS_LARGE_SURFACE, bytes);
    if (!source || !surface) {
        result.status = G2D_PATHS_NO_SURFACE;
        ss_finish(&result, sizeof(result));
        return;
    }
    /* The source, then the output's background: the seed's sequence, one word per 4 bytes, padding included. */
    uint32_t x = G2D_PATHS_SEED;
    for (unsigned k = 0; k < 2; ++k) {
        uint8_t *p = k ? surface : source;
        for (size_t i = 0; i < bytes; i += 4) {
            const uint32_t v = next(&x);
            memcpy(p + i, &v, 4);
        }
    }
    result.plan.source_before_crc = crc32(0, source, bytes);
    result.plan.output_before_crc = crc32(0, surface, bytes);
    const uint32_t pick[3] = {find(source, transparent), find(source, opaque), find(source, partial)};
    result.open_result = (uint32_t)finish_call(open_call, &result.info);
    if ((int)result.open_result != 1) {
        result.status = G2D_PATHS_REFUSED;
        (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
        ss_finish(&result, sizeof(result));
        return;
    }
    int failed = 0;
    for (unsigned k = 0; k < G2D_PATHS_OPS && !failed; ++k) {
        struct g2d_paths_op *o = &result.op[k];
        *o = (struct g2d_paths_op){.path = paths[k], .sx = G2D_PATHS_SX, .sy = G2D_PATHS_SY,
            .dx = G2D_PATHS_DX, .dy = G2D_PATHS_DY + k * G2D_PATHS_STEP, .w = G2D_PATHS_W, .h = G2D_PATHS_H,
            .alpha_0 = pick[0], .alpha_ff = pick[1], .partial = pick[2]};
        job = (struct ss_g2d_job){.op = SS_G2D_BLIT, .path = o->path,
            .source = {G2D_PATHS_LARGE_SOURCE, 0, 0, G2D_PATHS_WIDTH, G2D_PATHS_HEIGHT, G2D_PATHS_STRIDE, 0},
            .output = {G2D_PATHS_LARGE_SURFACE, 0, 0, G2D_PATHS_WIDTH, G2D_PATHS_HEIGHT, G2D_PATHS_STRIDE, 0},
            .sx = o->sx, .sy = o->sy, .dx = o->dx, .dy = o->dy, .w = o->w, .h = o->h};
        o->result = (uint32_t)finish_call(run_call, &o->done);
        if ((int)o->result != 1) {
            failed = 1;
            (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
            break;
        }
        const uint8_t *first = surface + (size_t)o->dy * G2D_PATHS_STRIDE + (size_t)o->dx * 4;
        uint32_t c = 0;
        for (uint32_t y = 0; y < o->h; ++y) c = crc32(c, first + (size_t)y * G2D_PATHS_STRIDE, (size_t)o->w * 4);
        o->rect_crc = c;
        for (unsigned n = 0; n < 3; ++n)
            if (pick[n] != UINT32_MAX)
                memcpy(&o->sample[n], first + (size_t)(pick[n] / o->w) * G2D_PATHS_STRIDE + (size_t)(pick[n] % o->w) * 4, 4);
        memcpy(&o->sample[3], first - 4, 4);
    }
    if (!failed) {
        result.plan.output_crc = crc32(0, surface, bytes);
        result.plan.source_crc = crc32(0, source, bytes);
    }
    result.close_result = (uint32_t)finish_call(close_call, &result.closed);
    result.status = failed ? G2D_PATHS_RUN_FAILED :
                    (int)result.close_result == 1 ? G2D_PATHS_DONE : G2D_PATHS_CLOSE_FAILED;
    if (!failed) (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
    ss_finish(&result, sizeof(result));
}
