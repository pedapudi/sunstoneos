/* A host SDK for apps/g2d-convert/main.c (checks/g2d-convert-host.py). The large regions are host buffers, the clock
 * is ready with the approved points, and ss_g2d_run performs each convert on the CPU with the engine's model: the
 * scaler model (the job's steps and Lanczos banks, as drivers/g2d/a733_g2d_convert.c programs them, each position
 * rounded to the nearest of 32 phases, taps at positions -1 to +2, the edge samples repeated, each pass rounded to
 * nearest and clamped; an unscaled row's last pixel takes its own chroma pair), then the programmed table
 * (tools/g2d-csc-tables.py) with luma kept, clamped to 0..255, with U the first byte of each chroma pair. Mutants:
 * - -DFLOOR raises luma below 16 to 16 (the blender working in YUV);
 * - -DNO_LOW_CLAMP and -DNO_HIGH_CLAMP drop one bound of the output clamp, keeping the low byte;
 * - -DNEAREST_CHROMA gives odd pixels the chroma of their own pair (no interpolation);
 * - -DTRUNC_PHASE truncates each scaler position to its phase instead of rounding it;
 * - -DMISS changes one pixel of the first conversion's exact region;
 * - -DFAIL_OP=n fails operation n (counted from 0) at step 62 (a timeout) with nothing written.
 * ss_finish writes the result to the file named by the first argument. */
#include "g2d_convert-app.h"
#include "sunstone_g2d.h"
#include "a733_g2d_convert.c"
#include <stdio.h>
#include <stdlib.h>

static uint8_t surface[0x400000], source[0x200000], reference[0x200000];
static const char *path;
static unsigned runs, interrupts;
static uint32_t fault_step, fault_value;
static uint64_t ticks;

void *ss_large_base(unsigned index, size_t *bytes)
{
    uint8_t *base[3] = {surface, source, reference};
    const size_t size[3] = {sizeof(surface), sizeof(source), sizeof(reference)};
    if (index > 2) return NULL;
    *bytes = size[index];
    return base[index];
}
int ss_large_wait(unsigned index, uint64_t timeout_us) { (void)timeout_us; return index < 3 ? 1 : -1; }
int ss_live(void) { return 0; }
void ss_progress(void) {}
uint64_t ss_counter_ticks(void) { return ticks += 24; }
void ss_wait_until(uint64_t absolute_us) { (void)absolute_us; }
int ss_clock_ready(void) { return 1; }
int ss_clock_info(char *line, size_t bytes)
{
    return snprintf(line, bytes, "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 ");
}
void ss_g2d_wait(uint32_t wait_us) { (void)wait_us; }
void ss_finish(const void *result, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(result, 1, bytes, f) != bytes || fclose(f)) exit(2);
    exit(0);
}
void ss_fail(uint32_t detail) { fprintf(stderr, "ss_fail %u\n", detail); exit(3); }
int ss_g2d_open(struct ss_g2d_info *info)
{
    memset(info, 0, sizeof(*info));
    info->magic = SS_G2D_INFO_MAGIC; info->bytes = sizeof(*info);
    return 1;
}
int ss_g2d_close(struct ss_g2d_closed *closed) { memset(closed, 0, sizeof(*closed)); return 1; }
int ss_g2d_fault(uint32_t *step, uint32_t *value) { *step = fault_step; *value = fault_value; return 1; }

static int32_t clip(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
/* A position with 20 fraction bits as (integer part << 5) | phase, rounded to the nearest of 32 phases. */
static uint64_t phase_of(uint64_t position)
{
#ifdef TRUNC_PHASE
    return position >> 15;
#else
    return (position + (1u << 14)) >> 15;
#endif
}
/* One sample through a 4-tap phase: positions i - 1 to i + 2 of n samples spaced by stride, the edges repeated. */
static int32_t filter(const uint8_t *p, int32_t n, size_t stride, int32_t i, uint32_t word)
{
    int32_t acc = 0;
    for (int k = 0; k < 4; ++k) {
        int32_t at = i - 1 + k;
        at = at < 0 ? 0 : at >= n ? n - 1 : at;
        acc += (int8_t)(word >> (8 * k)) * p[(size_t)at * stride];
    }
    return clip((acc + 32) >> 6);
}
/* The scaler model for one plane component: w by h samples (spacing step bytes, rows stride bytes apart) to ow by oh,
 * with the horizontal register step (20 fraction bits) and bank, the vertical step and bank. */
static void scale_plane(const uint8_t *in, int32_t w, int32_t h, size_t spacing, size_t stride, uint8_t *out,
                        uint32_t ow, uint32_t oh, uint32_t hreg, uint32_t hbank, uint32_t vreg, uint32_t vbank)
{
    uint8_t *rows = malloc((size_t)ow * (size_t)h);
    for (int32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < ow; ++x) {
            const uint64_t q = phase_of((uint64_t)x * hreg);
            rows[(size_t)y * ow + x] = (uint8_t)filter(in + (size_t)y * stride, w, spacing, (int32_t)(q >> 5),
                                                       lan2coefftab32_full[hbank + (q & 31)]);
        }
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x) {
            const uint64_t q = phase_of((uint64_t)y * vreg);
            out[(size_t)y * ow + x] = (uint8_t)filter(rows + x, h, ow, (int32_t)(q >> 5),
                                                      lan2coefftab32_full[vbank + (q & 31)]);
        }
    free(rows);
}
int ss_g2d_run(const struct ss_g2d_job *j, struct ss_g2d_done *done)
{
    if (j->op != SS_G2D_CONVERT || j->source.region != 1 || j->output.region != 0 ||
        j->source.format != SS_G2D_FORMAT_NV16 || j->output.format || j->sx || j->sy || j->dx || j->dy)
        return -1;
    memset(done, 0, sizeof(*done));
    done->magic = SS_G2D_DONE_MAGIC; done->bytes = sizeof(*done);
    const unsigned run = runs++;
    (void)run;
#ifdef FAIL_OP
    if (run == FAIL_OP) { fault_step = 62; fault_value = 0x4; return -1; }
#endif
    const int32_t *t = a733_g2d_csc_table(j->csc);
    const uint32_t w = j->w, h = j->h, ow = j->ow, oh = j->oh;
    const uint32_t hstep = (w << 19) / ow, vstep = (h << 19) / oh;
    uint8_t *y = malloc((size_t)ow * oh), *u = malloc((size_t)ow * oh), *v = malloc((size_t)ow * oh);
    const uint8_t *luma = source + j->source.offset, *chroma = source + j->source.chroma_offset;
    scale_plane(luma, (int32_t)w, (int32_t)h, 1, j->source.stride, y, ow, oh, hstep << 1, fir_coef(hstep),
                vstep << 1, fir_coef(vstep));
    const unsigned first = 0;
#ifdef NEAREST_CHROMA
    /* Each pixel takes its own pair: the chroma plane scaled as 640 samples, each pair repeated. */
    for (uint32_t r = 0; r < oh; ++r)
        for (uint32_t x = 0; x < ow; ++x) {
            u[(size_t)r * ow + x] = chroma[(size_t)r * j->source.chroma_stride + 2 * (x / 2) + first];
            v[(size_t)r * ow + x] = chroma[(size_t)r * j->source.chroma_stride + 2 * (x / 2) + 1 - first];
        }
#else
    scale_plane(chroma + first, (int32_t)(w + 1) / 2, (int32_t)h, 2, j->source.chroma_stride, u, ow, oh, hstep,
                fir_coef(hstep >> 1), vstep << 1, fir_coef(vstep));
    scale_plane(chroma + 1 - first, (int32_t)(w + 1) / 2, (int32_t)h, 2, j->source.chroma_stride, v, ow, oh, hstep,
                fir_coef(hstep >> 1), vstep << 1, fir_coef(vstep));
    if (ow == w && oh == h)
        for (uint32_t r = 0; r < oh; ++r) {
            u[(size_t)r * ow + ow - 1] = chroma[(size_t)r * j->source.chroma_stride + 2 * ((w - 1) / 2) + first];
            v[(size_t)r * ow + ow - 1] = chroma[(size_t)r * j->source.chroma_stride + 2 * ((w - 1) / 2) + 1 - first];
        }
#endif
#ifdef FLOOR
    const int floor16 = 1;
#else
    const int floor16 = 0;
#endif
    for (uint32_t r = 0; r < oh; ++r)
        for (uint32_t x = 0; x < ow; ++x) {
            const size_t i = (size_t)r * ow + x;
            uint32_t word = UINT32_C(0xff) << 24;
            const int32_t luma_value = floor16 && y[i] < 16 ? 16 : y[i];
            for (unsigned c = 0; c < 3; ++c) {
                int32_t value = (t[4 * c] * luma_value + t[4 * c + 1] * u[i] + t[4 * c + 2] * v[i] + t[4 * c + 3]) >> 10;
#ifndef NO_LOW_CLAMP
                if (value < 0) value = 0;
#endif
#ifndef NO_HIGH_CLAMP
                if (value > 255) value = 255;
#endif
                word |= ((uint32_t)value & 255u) << (16 - 8 * c);
            }
#ifdef MISS
            if (run == 0 && r == 7 && x == 9) word ^= 0x100;
#endif
            memcpy(surface + j->output.offset + (size_t)r * j->output.stride + x * 4, &word, 4);
        }
    free(y); free(u); free(v);
    done->irq_count = ++interrupts;
    done->launch_ticks = 1000; done->irq_ticks = 1000 + 24 * 1500;
    return 1;
}
int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    path = argv[1];
    const struct ss_grant grant = {.main_core = 6};
    g2d_convert_main(&grant);
    return 5;
}
