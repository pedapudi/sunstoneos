/* g2d-check: the G2D board check on the SunstoneOS interface (include/sunstone_g2d.h, docs/g2d-os.md). It opens the
 * engine, which powers, clocks and resets it and reads its identity; runs one fill into a surface in the application's
 * large region "surface"; closes the engine; and returns the open, run and close records with the CRC-32 of the
 * surface. The surface's background pattern, its geometry, the rectangle and the color all come from one seed, from
 * which apps/g2d-check/host.py computes the expected CRC-32 and decodes the result. */
#include "g2d_check-app.h"
#include "sunstone_g2d.h"
#include "sunstone_large.h"
#include <string.h>

#define G2D_CHECK_MAGIC UINT32_C(0x4b433247) /* "G2CK" */
#define G2D_CHECK_SEED UINT32_C(0x6a09e667)
#define G2D_CHECK_REGION 0u
enum {G2D_CHECK_OPENED = 1, G2D_CHECK_REFUSED = 2, G2D_CHECK_CLOSE_FAILED = 3, G2D_CHECK_RUN_FAILED = 4,
      G2D_CHECK_NO_SURFACE = 5};
struct g2d_check_run {
    uint32_t result, seed, width, height, stride, dx, dy, w, h, color, crc, reserved;
    /* After the fill: the rectangle's first and last pixels, the pixel before the first (or the row's last padding
     * word when the rectangle starts at x 0), and the first row's padding word after the last pixel. */
    uint32_t sample[4];
    struct ss_g2d_done done;
};
struct g2d_check_result {
    uint32_t magic, stage, status, open_result, close_result, fault_step, fault_value, reserved;
    struct ss_g2d_info info;
    struct ss_g2d_closed closed;
    struct g2d_check_run run;
};
_Static_assert(sizeof(struct g2d_check_result) <= 1024, "the result fits the manifest's output_limit");

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

/* Geometry, rectangle and color from the seed (host.py repeats this sequence); the surface's rows carry 64 bytes of
 * padding, which the fill must leave alone. */
static void plan(struct g2d_check_run *r, uint32_t *x)
{
    r->width = 320 + next(x) % 321;          /* 320..640 */
    r->height = 180 + next(x) % 181;         /* 180..360 */
    r->stride = r->width * 4 + 64;
    r->w = 1 + next(x) % r->width;
    r->h = 1 + next(x) % r->height;
    r->dx = next(x) % (r->width - r->w + 1);
    r->dy = next(x) % (r->height - r->h + 1);
    r->color = next(x);
}

void g2d_check_main(const struct ss_grant *grant)
{
    (void)grant;
    static struct g2d_check_result result;
    result.magic = G2D_CHECK_MAGIC;
    result.stage = 2;
    struct g2d_check_run *r = &result.run;
    uint32_t x = G2D_CHECK_SEED;
    r->seed = G2D_CHECK_SEED;
    plan(r, &x);
    /* The region has no source: the loader marks it ready after the application starts. */
    int ready;
    do { ss_progress(); ready = ss_large_wait(G2D_CHECK_REGION, 100000); } while (ready == 0 && ss_live() >= 0);
    size_t capacity = 0;
    uint8_t *surface = ready == 1 ? ss_large_base(G2D_CHECK_REGION, &capacity) : NULL;
    const size_t bytes = (size_t)r->stride * r->height;
    if (!surface || capacity < bytes) {
        result.status = G2D_CHECK_NO_SURFACE;
        ss_finish(&result, sizeof(result));
        return;
    }
    /* The background: the seed's sequence continued, one word per 4 bytes, padding included. */
    for (size_t i = 0; i < bytes; i += 4) {
        const uint32_t v = next(&x);
        memcpy(surface + i, &v, 4);
    }
    result.open_result = (uint32_t)finish_call(open_call, &result.info);
    if ((int)result.open_result != 1) {
        result.status = G2D_CHECK_REFUSED;
        (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
        ss_finish(&result, sizeof(result));
        return;
    }
    job = (struct ss_g2d_job){.op = SS_G2D_FILL, .color = r->color,
        .output = {G2D_CHECK_REGION, 0, 0, r->width, r->height, r->stride, 0},
        .dx = r->dx, .dy = r->dy, .w = r->w, .h = r->h};
    r->result = (uint32_t)finish_call(run_call, &r->done);
    if ((int)r->result == 1) {
        r->crc = crc32(surface, bytes);
        const size_t first = (size_t)r->dy * r->stride + (size_t)r->dx * 4;
        const size_t last = (size_t)(r->dy + r->h - 1) * r->stride + (size_t)(r->dx + r->w - 1) * 4;
        const size_t pad = (size_t)r->dy * r->stride + (size_t)r->width * 4;
        memcpy(&r->sample[0], surface + first, 4);
        memcpy(&r->sample[1], surface + last, 4);
        memcpy(&r->sample[2], surface + (r->dx ? first - 4 : (size_t)r->dy * r->stride + r->stride - 4), 4);
        memcpy(&r->sample[3], surface + pad, 4);
    }
    else (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
    result.close_result = (uint32_t)finish_call(close_call, &result.closed);
    result.status = (int)r->result != 1 ? G2D_CHECK_RUN_FAILED :
                    (int)result.close_result == 1 ? G2D_CHECK_OPENED : G2D_CHECK_CLOSE_FAILED;
    if ((int)r->result == 1) (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
    ss_finish(&result, sizeof(result));
}
