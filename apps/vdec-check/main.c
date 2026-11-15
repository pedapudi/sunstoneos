/* vdec-check: the video decoder board check on the SunstoneOS interface (include/sunstone_video_decode.h,
 * docs/video-decode-os.md) and its board record (apps/vdec-check/record.h). Stage 2 opens the decoder, which powers
 * PCK600 domain 2, cold-starts it with PLL_VE0 at 624 MHz and reads its identity; decodes the 48-by-32 nonconstant
 * JPEG (apps/vdec-check/fixture.h) from the large region "jpeg" into the large region "pixels"; closes the decoder
 * (also after a failed open or decode); and returns the record, the decode's answer and the 2,304 decoded bytes.
 * apps/vdec-check/host.py decodes the result. */
#include "vdec_check-app.h"
#include "sunstone_large.h"
#include "sunstone_video_decode.h"
#include "fixture.h"
#include "record.h"
#include <string.h>

#define VD_CHECK_MAGIC UINT32_C(0x4b434456) /* "VDCK" */
/* The fixture's active YU12 bytes: the Y plane, then Cb and Cr at half the width and height. */
#define VD_CHECK_PIXELS (VDEC_CHECK_FIXTURE_WIDTH * VDEC_CHECK_FIXTURE_HEIGHT + \
    2u * ((VDEC_CHECK_FIXTURE_WIDTH + 1u) / 2u) * ((VDEC_CHECK_FIXTURE_HEIGHT + 1u) / 2u))
/* The output region's bytes before the decode, so that a byte the service did not write shows. */
#define VD_CHECK_UNWRITTEN 0x5au
enum {VD_CHECK_DECODED = 1, VD_CHECK_REFUSED = 2, VD_CHECK_CLOSE_FAILED = 3, VD_CHECK_DECODE_FAILED = 4,
      VD_CHECK_NO_REGION = 5};
struct video_decode_check_result {
    uint32_t magic, stage, status, open_result, close_result, fault_step, fault_value, decode_result;
    struct ss_video_decode_done done;
    struct vdec_check_record info;
    uint8_t pixels[VD_CHECK_PIXELS];
};
_Static_assert(sizeof(struct video_decode_check_result) <= 12288, "the result fits the manifest's output_limit");

static struct ss_video_decode_jpeg job;
static int decode(void) { return ss_video_decode_jpeg(&job); }
static int finish_call(int (*call)(void))
{
    for (;;) {
        const int r = call();
        if (r) return r;
        if (ss_live() < 0) ss_fail(1);
        ss_progress();
        ss_video_decode_wait(100000);
    }
}
static uint8_t *region(unsigned index, size_t bytes)
{
    int ready;
    do { ss_progress(); ready = ss_large_wait(index, 100000); } while (ready == 0 && ss_live() >= 0);
    size_t capacity = 0;
    uint8_t *base = ready == 1 ? ss_large_base(index, &capacity) : NULL;
    return base && capacity >= bytes ? base : NULL;
}

void vdec_check_main(const struct ss_grant *grant)
{
    (void)grant;
    static struct video_decode_check_result result;
    result.magic = VD_CHECK_MAGIC;
    result.stage = 2;
    result.open_result = (uint32_t)finish_call(ss_video_decode_open);
    if ((int)result.open_result == 1) {
        /* The regions have no source: the loader marks them ready after the application starts. */
        uint8_t *input = region(VDEC_CHECK_LARGE_JPEG, VDEC_CHECK_FIXTURE_BYTES);
        uint8_t *output = region(VDEC_CHECK_LARGE_PIXELS, VD_CHECK_PIXELS);
        if (input && output) {
            memcpy(input, vdec_check_fixture, VDEC_CHECK_FIXTURE_BYTES);
            memset(output, VD_CHECK_UNWRITTEN, VD_CHECK_PIXELS);
            job = (struct ss_video_decode_jpeg){VDEC_CHECK_LARGE_JPEG, VDEC_CHECK_LARGE_PIXELS, 0, 0,
                VDEC_CHECK_FIXTURE_BYTES, VD_CHECK_PIXELS};
            result.decode_result = (uint32_t)finish_call(decode);
            (void)ss_video_decode_done(&result.done);
            memcpy(result.pixels, output, VD_CHECK_PIXELS);
        }
        result.close_result = (uint32_t)finish_call(ss_video_decode_close);
        result.status = !input || !output ? VD_CHECK_NO_REGION :
            (int)result.decode_result != 1 ? VD_CHECK_DECODE_FAILED :
            (int)result.close_result == 1 ? VD_CHECK_DECODED : VD_CHECK_CLOSE_FAILED;
    } else {
        /* After a fault the close runs the service's one fault close, which keeps the fault and lets the session
         * end normally; after a refusal with no write it returns 1 at once. */
        result.close_result = (uint32_t)finish_call(ss_video_decode_close);
        result.status = VD_CHECK_REFUSED;
    }
    (void)ss_video_decode_fault(&result.fault_step, &result.fault_value);
    (void)vdec_check_record(&result.info);
    ss_finish(&result, sizeof(result));
}
