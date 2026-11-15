/* Host driver of checks/g2d-rotate-words.py: one job per input line, "source output source_stride output_stride sx sy
 * dx dy w h format degrees flip" in decimal, and one output line per job: the word count, then offset=value pairs in
 * hex. */
#include "a733_g2d_rotate.h"
#include <inttypes.h>
#include <stdio.h>

int main(void)
{
    struct a733_g2d_rotate r;
    while (scanf("%" SCNu64 " %" SCNu64 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32
                 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32,
                 &r.source, &r.output, &r.source_stride, &r.output_stride, &r.sx, &r.sy, &r.dx, &r.dy, &r.w, &r.h,
                 &r.format, &r.degrees, &r.flip) == 13) {
        struct a733_g2d_rot_word out[A733_G2D_ROT_WORDS] = {{0, 0}};
        const unsigned n = a733_g2d_rotate_words(&r, out);
        printf("%u", n);
        for (unsigned i = 0; i < n; ++i) printf(" %" PRIx32 "=%" PRIx32, out[i].offset, out[i].value);
        printf("\n");
    }
    return 0;
}
