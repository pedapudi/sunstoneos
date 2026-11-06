/* headset-fixture: an application that declares the headset device, so that its image links the headset service, the
 * Bluetooth stack and the mSBC codec (cmake/sunstone-image.cmake). The gate builds it to check that library; the
 * profile's mode chooses CVSD (2) or mSBC (3) at run time, so one image covers both codecs. It captures one segment and
 * finishes with its length. */
#include "headset_fixture-app.h"

struct headset_fixture_result {
    uint64_t bytes;
    uint32_t frames, reserved;
};
_Static_assert(sizeof(struct headset_fixture_result) <= 48, "headset-fixture result fits the manifest's output_limit");

void headset_fixture_main(const struct ss_grant *grant)
{
    void *pcm;
    size_t bytes = 0;
    int r;
    while ((r = ss_capture(1, &pcm, &bytes)) == 0) {
        if (ss_live() < 0) ss_fail(1);
        ss_progress();
        ss_usb_wait(1000);
    }
    if (r < 0) ss_fail(2);
    const struct headset_fixture_result result = {bytes, grant->capture_frames, 0};
    ss_finish(&result, sizeof(result));
}
