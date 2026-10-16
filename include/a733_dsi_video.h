#ifndef SUNSTONE_A733_DSI_VIDEO_H
#define SUNSTONE_A733_DSI_VIDEO_H
#include <stdint.h>

enum a733_dsi_video_format { A733_DSI_RGB888, A733_DSI_RGB666_LOOSE };
enum a733_dsi_video_mode { A733_DSI_SYNC_PULSE, A733_DSI_BURST };
struct a733_dsi_video_timing {
    uint32_t pixel_hz, hs_hz, ls_hz;
    uint16_t width, height, hsync, hback, hfront, vsync, vback, vfront;
    unsigned lanes;
    enum a733_dsi_video_format format;
    enum a733_dsi_video_mode mode;
};
struct a733_dsi_video_word { uint32_t offset, mask, value; };
#define A733_DSI_VIDEO_WORDS 48u
struct a733_dsi_video_plan {
    struct a733_dsi_video_word words[A733_DSI_VIDEO_WORDS];
    uint32_t count, byte_hz, htotal, vtotal;
};
/* Pure, all-or-nothing plan. Four lanes, single link, virtual channel zero,
 * continuous clock, RGB888/loose RGB666, burst or nonburst sync pulses.
 * Pixel rate is the nominal mode rate. LS = pixel rate (nonburst) or pixel
 * rate * 10/9 (burst), HS = LS * 6, byte clock = HS/8. All are exact integers;
 * the existing PLL planner must admit HS/LS. No rate is electrically qualified.
 * Refuses field overflow and packet-count underflow. Storage must be disjoint.
 */
int a733_dsi_video_plan(const struct a733_dsi_video_timing *,
                         struct a733_dsi_video_plan *);

enum a733_dsi_video_action { A733_DSI_VIDEO_PREPARE, A733_DSI_VIDEO_START,
    A733_DSI_VIDEO_STOP, A733_DSI_VIDEO_OBSERVE };
enum a733_dsi_video_state { A733_DSI_VIDEO_PENDING, A733_DSI_VIDEO_READY,
    A733_DSI_VIDEO_ACTIVE, A733_DSI_VIDEO_STOPPED, A733_DSI_VIDEO_RETAINED };
enum a733_dsi_video_error { A733_DSI_VIDEO_OK, A733_DSI_VIDEO_TIME,
    A733_DSI_VIDEO_IO, A733_DSI_VIDEO_AUTHORITY, A733_DSI_VIDEO_INITIAL_STATE,
    A733_DSI_VIDEO_VERIFY, A733_DSI_VIDEO_ENGINE, A733_DSI_VIDEO_REENTRY };
struct a733_dsi_video_config;
struct a733_dsi_video_ops {
    int (*admit)(void *, const struct a733_dsi_video_config *, enum a733_dsi_video_action);
    int (*read32)(void *, uintptr_t, uint32_t *);
    int (*write32)(void *, uintptr_t, uint32_t);
    int (*barrier)(void *);
    int (*now_us)(void *, uint64_t *);
};
struct a733_dsi_video_config {
    struct a733_dsi_video_ops ops;
    void *context;
    uint64_t lease;
    unsigned controller;
    struct a733_dsi_video_timing timing;
};
struct a733_dsi_video {
    struct a733_dsi_video_config config;
    struct a733_dsi_video_plan plan;
    uint64_t deadline, last_now;
    uint32_t expected, control, debug_status;
    enum a733_dsi_video_state state;
    enum a733_dsi_video_error error;
    enum a733_dsi_video_action action;
    unsigned initialized, entered, step, index, substep, writes_attempted;
};
/* Fresh zero storage, no reinitialization, privileged serialized owner.
 * Copies config/plan. Callback/context storage must remain live, private and
 * disjoint. Callbacks return exactly 1, are bounded and non-reentrant; MMIO is
 * ordered Device memory and barrier completes writes. admit authenticates the
 * combined host/PHY/PLL/pin/power lease, exact clocks and exclusive engine use.
 * PREPARE requires completed PHY preparation, idle LP engine and no pixel feed.
 * START requires an armed, correctly routed TCON/DE producer and prepared panel.
 * STOP must preserve the feed until STOPPED; OBSERVE must retain exclusivity.
 * These guarantees remain valid for each entire operation and ACTIVE interval.
 * Production authority/producer binding and watchdog recovery remain required.
 */
int a733_dsi_video_init(struct a733_dsi_video *, const struct a733_dsi_video_config *,
                         uint64_t deadline);
/* READY -> START; ACTIVE -> STOP or OBSERVE. No restart/reconfigure/release.
 * Refusal is pure. Deadline is an absolute monotonic microsecond timestamp. */
int a733_dsi_video_request(struct a733_dsi_video *, enum a733_dsi_video_action,
                            uint64_t deadline);
/* At most one register/barrier/admission operation per poll; pre/post deadlines.
 * READY proves setup, ACTIVE proves only the local start bit remained asserted,
 * STOPPED proves observed local engine idle after its conditional exit request.
 * OBSERVE checks the local engine/error flags; it does not prove pixel delivery.
 * Every fault retains all resources. Terminal states perform no I/O. ACTIVE
 * requires explicitly scheduled OBSERVE operations; no implicit background work.
 * No clocks, reset, panel commands, IRQ acknowledgement or resource release. */
enum a733_dsi_video_state a733_dsi_video_poll(struct a733_dsi_video *);
#endif
