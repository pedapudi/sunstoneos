/* A host SDK for apps/g2d-paths/main.c (checks/g2d-paths-host.py). The large regions are host buffers, and ss_g2d_run
 * performs each unconverted copy on the CPU with the outcome the stage-5 hypothesis predicts for its path: the vendor
 * path writes a source pixel with alpha 0 as 0x00000000, the MARKERS path writes it as pipe 0's fill marker, and the
 * other three copy it exactly. Built with -DALL_ZEROED, every path writes it as 0. Built with -DFAIL_PATH=n, the copy
 * on path n fails at step 62 (a timeout) and writes nothing. ss_finish writes the result to the file named by the first
 * argument. */
#include "g2d_paths-app.h"
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
static uint32_t transparent(uint32_t p)
{
#ifdef ALL_ZEROED
    (void)p;
    return 0;
#else
    return p == SS_G2D_PATH_VENDOR ? 0 : p == SS_G2D_PATH_MARKERS ? UINT32_C(0x00a1b2c3) : UINT32_MAX;
#endif
}
int ss_g2d_run(const struct ss_g2d_job *j, struct ss_g2d_done *done)
{
    if(j->op != SS_G2D_BLIT || j->source.region > 1 || j->output.region > 1 || j->source.stride != j->output.stride ||
       j->source.format || j->output.format || j->path > SS_G2D_PATH_NO_FILL)
        return -1;
    memset(done, 0, sizeof(*done));
    done->magic = SS_G2D_DONE_MAGIC; done->bytes = sizeof(*done);
#ifdef FAIL_PATH
    if(j->path == FAIL_PATH) { fault_step = 62; fault_value = 0x4; return -1; }
#endif
    const uint8_t *src = regions[j->source.region] + j->source.offset;
    uint8_t *out = regions[j->output.region] + j->output.offset;
    for(uint32_t y = 0; y < j->h; ++y)
        for(uint32_t x = 0; x < j->w; ++x) {
            uint32_t w;
            memcpy(&w, src + (size_t)(j->sy + y) * j->source.stride + (size_t)(j->sx + x) * 4, 4);
            if(!(w >> 24) && transparent(j->path) != UINT32_MAX) w = transparent(j->path);
            memcpy(out + (size_t)(j->dy + y) * j->output.stride + (size_t)(j->dx + x) * 4, &w, 4);
        }
    done->irq_count = ++interrupts; done->rcq_status = 0x105; done->status_after = 4;
    return 1;
}
int main(int argc, char **argv)
{
    if(argc != 2) return 1;
    path = argv[1];
    g2d_paths_main(NULL);
    return 5;
}
