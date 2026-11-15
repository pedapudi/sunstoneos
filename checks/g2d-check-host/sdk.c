/* A host SDK for apps/g2d-check/main.c (checks/g2d-check-host.py). The large regions are host buffers, and ss_g2d_run
 * performs the copy on the CPU: ARGB8888 to ARGB8888 or BGRA8888, a byte reverse of each 32-bit word. Built with
 * -DNO_CONVERT, the copy ignores the output format, as an engine that converts nothing would. ss_finish writes the
 * result to the file named by the first argument. */
#include "g2d_check-app.h"
#include "sunstone_g2d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REGION_BYTES 2097152u
static uint8_t regions[2][REGION_BYTES];
static const char *path;
static unsigned interrupts;

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
int ss_g2d_fault(uint32_t *step, uint32_t *value) { *step = 0; *value = 0; return 1; }
static uint32_t convert(uint32_t w, uint32_t from, uint32_t to)
{
#ifdef NO_CONVERT
    (void)from; (void)to;
    return w;
#else
    if(from != 0 || (to != 0 && to != 3)) exit(4);
    return to == 3 ? __builtin_bswap32(w) : w;
#endif
}
int ss_g2d_run(const struct ss_g2d_job *j, struct ss_g2d_done *done)
{
    if(j->op != SS_G2D_BLIT || j->source.region > 1 || j->output.region > 1 || j->source.stride != j->output.stride)
        return -1;
    const uint8_t *src = regions[j->source.region] + j->source.offset;
    uint8_t *out = regions[j->output.region] + j->output.offset;
    for(uint32_t y = 0; y < j->h; ++y)
        for(uint32_t x = 0; x < j->w; ++x) {
            uint32_t w;
            memcpy(&w, src + (size_t)(j->sy + y) * j->source.stride + (size_t)(j->sx + x) * 4, 4);
            w = convert(w, j->source.format, j->output.format);
            memcpy(out + (size_t)(j->dy + y) * j->output.stride + (size_t)(j->dx + x) * 4, &w, 4);
        }
    memset(done, 0, sizeof(*done));
    done->magic = SS_G2D_DONE_MAGIC; done->bytes = sizeof(*done);
    done->irq_count = ++interrupts; done->rcq_status = 0x105; done->status_after = 4;
    return 1;
}
int main(int argc, char **argv)
{
    if(argc != 2) return 1;
    path = argv[1];
    g2d_check_main(NULL);
    return 5;
}
