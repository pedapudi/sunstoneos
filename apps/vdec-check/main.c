/* vdec-check: the video decoder board check on the SunstoneOS interface (include/sunstone_video_decode.h,
 * docs/video-decode-os.md). Stage 1 opens the decoder, which powers PCK600 domain 2, cold-starts it with PLL_VE0 at
 * 624 MHz and reads its identity, closes it again, and returns the record and any fault. apps/vdec-check/host.py
 * decodes the result. */
#include "vdec_check-app.h"
#include "sunstone_video_decode.h"

#define VD_CHECK_MAGIC UINT32_C(0x4b434456) /* "VDCK" */
enum {VD_CHECK_OPENED = 1, VD_CHECK_REFUSED = 2, VD_CHECK_CLOSE_FAILED = 3};
struct video_decode_check_result {
    uint32_t magic, stage, status, open_result, close_result, fault_step, fault_value, reserved;
    struct ss_video_decode_info info;
};
_Static_assert(sizeof(struct video_decode_check_result) <= 1024, "the result fits the manifest's output_limit");

static int finish_call(int (*call)(struct ss_video_decode_info *), struct ss_video_decode_info *info)
{
    for (;;) {
        const int r = call(info);
        if (r) return r;
        if (ss_live() < 0) ss_fail(1);
        ss_progress();
        ss_video_decode_wait(100000);
    }
}

void vdec_check_main(const struct ss_grant *grant)
{
    (void)grant;
    static struct video_decode_check_result result;
    result.magic = VD_CHECK_MAGIC;
    result.stage = 1;
    result.open_result = (uint32_t)finish_call(ss_video_decode_open, &result.info);
    if ((int)result.open_result == 1) {
        result.close_result = (uint32_t)finish_call(ss_video_decode_close, &result.info);
        result.status = (int)result.close_result == 1 ? VD_CHECK_OPENED : VD_CHECK_CLOSE_FAILED;
    } else {
        result.status = VD_CHECK_REFUSED;
    }
    (void)ss_video_decode_fault(&result.fault_step, &result.fault_value);
    ss_finish(&result, sizeof(result));
}
