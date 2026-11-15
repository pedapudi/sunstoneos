/* g2d-check: the G2D board check on the SunstoneOS interface (include/sunstone_g2d.h, docs/g2d-os.md). Stage 1 opens the
 * engine, which powers, clocks and resets it and reads its identity, closes it again, and returns the open record, the
 * close record and any fault. apps/g2d-check/host.py decodes the result. */
#include "g2d_check-app.h"
#include "sunstone_g2d.h"

#define G2D_CHECK_MAGIC UINT32_C(0x4b433247) /* "G2CK" */
enum {G2D_CHECK_OPENED = 1, G2D_CHECK_REFUSED = 2, G2D_CHECK_CLOSE_FAILED = 3};
struct g2d_check_result {
    uint32_t magic, stage, status, open_result, close_result, fault_step, fault_value, reserved;
    struct ss_g2d_info info;
    struct ss_g2d_closed closed;
};
_Static_assert(sizeof(struct g2d_check_result) <= 512, "the result fits the manifest's output_limit");

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

void g2d_check_main(const struct ss_grant *grant)
{
    (void)grant;
    static struct g2d_check_result result;
    result.magic = G2D_CHECK_MAGIC;
    result.stage = 1;
    result.open_result = (uint32_t)finish_call(open_call, &result.info);
    if ((int)result.open_result == 1) {
        result.close_result = (uint32_t)finish_call(close_call, &result.closed);
        result.status = (int)result.close_result == 1 ? G2D_CHECK_OPENED : G2D_CHECK_CLOSE_FAILED;
    } else {
        result.status = G2D_CHECK_REFUSED;
    }
    (void)ss_g2d_fault(&result.fault_step, &result.fault_value);
    ss_finish(&result, sizeof(result));
}
