/* A host SDK for apps/g2d-rotate/main.c (checks/g2d-rotate-host.py). The large regions are host buffers, and
 * ss_g2d_run performs each rotate on the CPU with the outcome the hypothesis predicts: every pixel copied exactly,
 * 0 and 180 degrees as the identity and a half turn, 90 degrees clockwise and 270 counter-clockwise. Built with
 * -DZEROED, a source pixel with alpha 0 is written as 0x00000000. Built with -DSWAP_TURN, 90 degrees turns
 * counter-clockwise and 270 clockwise. Built with -DFAIL_DEGREES=n, the rotate by n degrees fails at step 62 (a
 * timeout) and writes nothing. Built with -DONE_PIXEL, one pixel of every rectangle has a bit changed, so no model
 * matches. ss_finish writes the result to the file named by the first argument. */
#include "g2d_rotate-app.h"
#include "sunstone_g2d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REGION_BYTES 2097152u
static uint8_t regions[2][REGION_BYTES];
static const char *path;
static unsigned interrupts;
static uint32_t fault_step, fault_value;

void *ss_large_base(unsigned index, size_t *bytes) { if(index > 1) return NULL; *bytes = REGION_BYTES; return regions[index]; }
int ss_large_wait(unsigned index, uint64_t timeout_us) { (void)timeout_us; return index < 2 ? 1 : -1; }
int ss_live(void) { return 0; }
void ss_progress(void) {}
void ss_g2d_wait(uint32_t wait_us) { (void)wait_us; }
void ss_finish(const void *result, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    if(!f || fwrite(result, 1, bytes, f) != bytes || fclose(f)) exit(2);
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
int ss_g2d_run(const struct ss_g2d_job *j, struct ss_g2d_done *done)
{
    if(j->op != SS_G2D_ROTATE || j->source.region > 1 || j->output.region > 1 || j->source.format ||
       j->output.format || j->flip || (j->degrees != 0 && j->degrees != 90 && j->degrees != 180 && j->degrees != 270))
        return -1;
    memset(done, 0, sizeof(*done));
    done->magic = SS_G2D_DONE_MAGIC; done->bytes = sizeof(*done);
#ifdef FAIL_DEGREES
    if(j->degrees == FAIL_DEGREES) { fault_step = 62; fault_value = 0x10000; return -1; }
#endif
    unsigned degrees = j->degrees;
#ifdef SWAP_TURN
    degrees = degrees == 90 ? 270 : degrees == 270 ? 90 : degrees;
#endif
    const int swap = degrees == 90 || degrees == 270;
    const uint32_t w = j->w, h = j->h, ow = swap ? h : w, oh = swap ? w : h;
    const uint8_t *src = regions[j->source.region] + j->source.offset;
    uint8_t *out = regions[j->output.region] + j->output.offset;
    for(uint32_t r = 0; r < oh; ++r)
        for(uint32_t c = 0; c < ow; ++c) {
            uint32_t y, x;
            switch(degrees) {
            case 0: y = r; x = c; break;
            case 180: y = h - 1 - r; x = w - 1 - c; break;
            case 90: y = h - 1 - c; x = r; break;                   /* clockwise */
            default: y = c; x = w - 1 - r; break;                   /* counter-clockwise */
            }
            uint32_t v;
            memcpy(&v, src + (size_t)(j->sy + y) * j->source.stride + (size_t)(j->sx + x) * 4, 4);
#ifdef ZEROED
            if(!(v >> 24)) v = 0;
#endif
#ifdef ONE_PIXEL
            if(r == 1 && c == 2) v ^= 0x00010000;                 /* one pixel that no model gives */
#endif
            memcpy(out + (size_t)(j->dy + r) * j->output.stride + (size_t)(j->dx + c) * 4, &v, 4);
        }
    done->irq_count = ++interrupts; done->rot_int = 0x10001; done->rot_int_after = 0x10000;
    return 1;
}
int main(int argc, char **argv)
{
    if(argc != 2) return 1;
    path = argv[1];
    g2d_rotate_main(NULL);
    return 5;
}
