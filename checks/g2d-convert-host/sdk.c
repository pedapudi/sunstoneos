/* A host SDK for apps/g2d-convert/main.c (checks/g2d-convert-host.py). The large regions are host buffers, the clock
 * is ready with the approved points, and ss_g2d_run performs each convert of a packed source on the CPU with the
 * engine's model: the bytes of each 4-byte pair read in the order of the format code (0x20 YUYV, 0x21 UYVY, 0x22 YVYU,
 * 0x23 VYUY), then the scaler model (the job's steps and banks, as drivers/g2d/a733_g2d_convert.c programs them: the
 * Lanczos horizontal banks, and the linear vertical bank or, with the Lanczos vertical variant in the job's reserved
 * word, the Lanczos one; each position rounded to the nearest of 32 phases, taps at positions -1 to +2, the edge
 * samples repeated, the horizontal pass and then the vertical, each rounded to nearest and clamped; an unscaled row's
 * last pixel takes its own chroma pair), then the programmed table (tools/g2d-csc-tables.py) with luma kept, clamped
 * to 0..255. Mutants:
 * - -DORDER_SWAP reads format 0x20 as UYVY and 0x21 as YUYV;
 * - -DNEAREST_CHROMA gives odd pixels the chroma of their own pair (no interpolation);
 * - -DTRUNC_PHASE truncates each scaler position to its phase instead of rounding it;
 * - -DV_THEN_H runs the vertical pass before the horizontal one;
 * - -DWIDE keeps the horizontal pass's sum unrounded and unclamped into the vertical pass;
 * - -DIGNORE_VARIANT keeps the linear vertical bank when the job asks for the Lanczos one;
 * - -DMISS changes one even pixel of the first conversion's exact region;
 * - -DFAIL_OP=n fails operation n (counted from 0) at step 62 (a timeout) with nothing written.
 * ss_finish writes the result to the file named by the first argument. */
#include "g2d_convert-app.h"
#include "sunstone_g2d.h"
#include "a733_g2d_convert.c"
#include <stdio.h>
#include <stdlib.h>

static uint8_t surface[0x400000], source[0x400000], reference[0x200000];
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
/* One pass of the scaler over n samples spaced by stride, to m outputs spaced by out_stride: each output's 4-tap sum
 * at positions i - 1 to i + 2, the edges repeated. With round set the sum is rounded to nearest, divided by 64 and
 * clamped; otherwise it is kept (the WIDE mutant's first pass) and the second pass divides by 4096. */
static void pass(const int32_t *in, int32_t n, size_t stride, int32_t *out, uint32_t m, size_t out_stride,
                 uint32_t step_reg, const uint32_t *bank, int shift)
{
    for (uint32_t x = 0; x < m; ++x) {
        const uint64_t q = phase_of((uint64_t)x * step_reg);
        const int32_t i = (int32_t)(q >> 5);
        const uint32_t word = bank[q & 31];
        int64_t acc = 0;
        for (int k = 0; k < 4; ++k) {
            int32_t at = i - 1 + k;
            at = at < 0 ? 0 : at >= n ? n - 1 : at;
            acc += (int8_t)(word >> (8 * k)) * (int64_t)in[(size_t)at * stride];
        }
        out[(size_t)x * out_stride] = shift ? clip((int32_t)((acc + (INT64_C(1) << (shift - 1))) >> shift)) : (int32_t)acc;
    }
}
/* The scaler model for one component: w by h samples to ow by oh, with the horizontal register step (20 fraction bits)
 * and bank, the vertical register step and bank. */
static void scale_plane(const int32_t *in, int32_t w, int32_t h, int32_t *out, uint32_t ow, uint32_t oh,
                        uint32_t hreg, const uint32_t *hbank, uint32_t vreg, const uint32_t *vbank)
{
#if defined(WIDE)
    const int first = 0, second = 12;
#else
    const int first = 6, second = 6;
#endif
#ifdef V_THEN_H
    int32_t *mid = malloc(sizeof(int32_t) * (size_t)w * oh);
    for (int32_t x = 0; x < w; ++x) pass(in + x, h, (size_t)w, mid + x, oh, (size_t)w, vreg, vbank, first);
    for (uint32_t y = 0; y < oh; ++y) pass(mid + (size_t)y * w, w, 1, out + (size_t)y * ow, ow, 1, hreg, hbank, second);
#else
    int32_t *mid = malloc(sizeof(int32_t) * ow * (size_t)h);
    for (int32_t y = 0; y < h; ++y) pass(in + (size_t)y * w, w, 1, mid + (size_t)y * ow, ow, 1, hreg, hbank, first);
    for (uint32_t x = 0; x < ow; ++x) pass(mid + x, h, ow, out + x, oh, ow, vreg, vbank, second);
#endif
    free(mid);
}
/* The byte positions of Y0, U, Y1 and V in a pair for each packed order (main.c's order_bytes). */
static const uint8_t positions[4][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {0, 3, 2, 1}, {1, 2, 3, 0}};
_Static_assert(A733_G2D_CONVERT_LANCZOS_VERTICAL == 1u, "main.c passes 1 for the Lanczos vertical variant");
int ss_g2d_run(const struct ss_g2d_job *j, struct ss_g2d_done *done)
{
    if (j->op != SS_G2D_CONVERT || j->source.region != 1 || j->output.region != 0 ||
        j->source.format - SS_G2D_FORMAT_YUYV > 3u || j->output.format || j->sx || j->sy || j->dx || j->dy ||
        j->reserved > 1u)
        return -1;
    memset(done, 0, sizeof(*done));
    done->magic = SS_G2D_DONE_MAGIC; done->bytes = sizeof(*done);
    const unsigned run = runs++;
    (void)run;
#ifdef FAIL_OP
    if (run == FAIL_OP) { fault_step = 62; fault_value = 0x4; return -1; }
#endif
    unsigned order = j->source.format - SS_G2D_FORMAT_YUYV;
#ifdef ORDER_SWAP
    if (order < 2) order ^= 1;
#endif
    const uint8_t *b = positions[order];
    const int32_t *t = a733_g2d_csc_table(j->csc);
    const uint32_t w = j->w, h = j->h, ow = j->ow, oh = j->oh, cw = w / 2;
    const uint32_t hstep = (w << 19) / ow, vstep = (h << 19) / oh;
    int lanczos = j->reserved == A733_G2D_CONVERT_LANCZOS_VERTICAL;
#ifdef IGNORE_VARIANT
    lanczos = 0;
#endif
    const uint32_t *vbank = lanczos ? lan2coefftab32_full + fir_coef(vstep) : linearcoefftab32;
    int32_t *luma = malloc(sizeof(int32_t) * w * h), *cu = malloc(sizeof(int32_t) * cw * h);
    int32_t *cv = malloc(sizeof(int32_t) * cw * h);
    int32_t *y = malloc(sizeof(int32_t) * ow * oh), *u = malloc(sizeof(int32_t) * ow * oh);
    int32_t *v = malloc(sizeof(int32_t) * ow * oh);
    for (uint32_t r = 0; r < h; ++r)
        for (uint32_t k = 0; k < cw; ++k) {
            const uint8_t *p = source + j->source.offset + (size_t)r * j->source.stride + 4 * k;
            luma[(size_t)r * w + 2 * k] = p[b[0]]; luma[(size_t)r * w + 2 * k + 1] = p[b[2]];
            cu[(size_t)r * cw + k] = p[b[1]]; cv[(size_t)r * cw + k] = p[b[3]];
        }
    scale_plane(luma, (int32_t)w, (int32_t)h, y, ow, oh, hstep << 1, lan2coefftab32_full + fir_coef(hstep),
                vstep << 1, vbank);
#ifdef NEAREST_CHROMA
    for (uint32_t r = 0; r < oh; ++r)
        for (uint32_t x = 0; x < ow; ++x) {
            u[(size_t)r * ow + x] = cu[(size_t)r * cw + x / 2];
            v[(size_t)r * ow + x] = cv[(size_t)r * cw + x / 2];
        }
#else
    scale_plane(cu, (int32_t)cw, (int32_t)h, u, ow, oh, hstep, lan2coefftab32_full + fir_coef(hstep >> 1), vstep << 1,
                vbank);
    scale_plane(cv, (int32_t)cw, (int32_t)h, v, ow, oh, hstep, lan2coefftab32_full + fir_coef(hstep >> 1), vstep << 1,
                vbank);
    if (ow == w && oh == h)
        for (uint32_t r = 0; r < oh; ++r) {
            u[(size_t)r * ow + ow - 1] = cu[(size_t)r * cw + cw - 1];
            v[(size_t)r * ow + ow - 1] = cv[(size_t)r * cw + cw - 1];
        }
#endif
    for (uint32_t r = 0; r < oh; ++r)
        for (uint32_t x = 0; x < ow; ++x) {
            const size_t i = (size_t)r * ow + x;
            uint32_t word = UINT32_C(0xff) << 24;
            for (unsigned c = 0; c < 3; ++c)
                word |= (uint32_t)clip((t[4 * c] * y[i] + t[4 * c + 1] * u[i] + t[4 * c + 2] * v[i] + t[4 * c + 3]) >> 10)
                        << (16 - 8 * c);
#ifdef MISS
            if (run == 0 && r == 7 && x == 10) word ^= 0x100;
#endif
            memcpy(surface + j->output.offset + (size_t)r * j->output.stride + x * 4, &word, 4);
        }
    free(luma); free(cu); free(cv); free(y); free(u); free(v);
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
