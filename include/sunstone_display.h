#ifndef SUNSTONE_DISPLAY_H
#define SUNSTONE_DISPLAY_H
#include <stddef.h>
#include <stdint.h>

/* Pixel timing is independent of the cable, serializer and framebuffer format.
 * Rates are exact Hz; porch counts are pixels/lines, excluding sync widths. */
#define SUNSTONE_MODE_HSYNC_POSITIVE 1u
#define SUNSTONE_MODE_VSYNC_POSITIVE 2u
#define SUNSTONE_MODE_INTERLACED 4u
#define SUNSTONE_MODE_PREFERRED 8u
struct sunstone_display_mode {
    uint32_t pixel_hz;
    uint16_t width, height, hfront, hsync, hback, vfront, vsync, vback;
    uint32_t flags;
};
static inline int sunstone_display_mode_valid(const struct sunstone_display_mode *m)
{
    return m && m->pixel_hz && m->width && m->height && m->hsync && m->vsync &&
        !(m->flags & ~15u) && (uint32_t)m->width+m->hfront+m->hsync+m->hback <= 65535 &&
        (uint32_t)m->height+m->vfront+m->vsync+m->vback <= 65535;
}
static inline int sunstone_display_mode_equal(const struct sunstone_display_mode *a,
                                               const struct sunstone_display_mode *b)
{
    return a && b && a->pixel_hz==b->pixel_hz && a->width==b->width && a->height==b->height &&
        a->hfront==b->hfront && a->hsync==b->hsync && a->hback==b->hback &&
        a->vfront==b->vfront && a->vsync==b->vsync && a->vback==b->vback &&
        !((a->flags^b->flags)&~SUNSTONE_MODE_PREFERRED);
}
/* Refresh is the exact rational numerator/denominator in frames per second;
 * interlaced mode geometry is reported in its encoded field convention. */
static inline int sunstone_display_refresh(const struct sunstone_display_mode *m,
    uint32_t *numerator,uint32_t *denominator)
{
    if(!sunstone_display_mode_valid(m)||!numerator||!denominator)return 0;
    *numerator=m->pixel_hz;
    *denominator=((uint32_t)m->width+m->hfront+m->hsync+m->hback)*
        ((uint32_t)m->height+m->vfront+m->vsync+m->vback);return 1;
}
#define SUNSTONE_DISPLAY_MAX_EDID 1024u
#define SUNSTONE_DISPLAY_MAX_MODES 64u
struct sunstone_display_port;
struct sunstone_display_modes {
    const struct sunstone_display_port *source_port;
    uint64_t generation;
    struct sunstone_display_mode modes[SUNSTONE_DISPLAY_MAX_MODES];
    size_t count;
    unsigned hdmi, rgb_range_selectable;
    uint32_t max_tmds_hz;
    /* Unsupported timing encodings and capacity loss are visible to callers. */
    unsigned skipped_timings, unknown_extensions, truncated;
};
enum sunstone_edid_result { SUNSTONE_EDID_OK=1, SUNSTONE_EDID_ARGUMENT=-1,
    SUNSTONE_EDID_HEADER=-2, SUNSTONE_EDID_LENGTH=-3, SUNSTONE_EDID_CHECKSUM=-4,
    SUNSTONE_EDID_BLOCK=-5, SUNSTONE_EDID_TIMING=-6 };
/* Complete EDID only: advertised block count and every checksum must match.
 * Parses base/CTA detailed timings, common CTA VICs and DisplayID I/VII.
 * Unknown encodings are counted. Failure leaves output unchanged. */
int sunstone_display_edid_parse(const uint8_t *,size_t,struct sunstone_display_modes *);

enum sunstone_display_connector { SUNSTONE_DISPLAY_HDMI, SUNSTONE_DISPLAY_DP, SUNSTONE_DISPLAY_DSI, SUNSTONE_DISPLAY_PARALLEL };
enum sunstone_display_mode_status { SUNSTONE_MODE_OK, SUNSTONE_MODE_INVALID,
    SUNSTONE_MODE_INTERLACE, SUNSTONE_MODE_GEOMETRY, SUNSTONE_MODE_CLOCK,
    SUNSTONE_MODE_LANES, SUNSTONE_MODE_FORMAT, SUNSTONE_MODE_LINK,
    SUNSTONE_MODE_NO_PANEL, SUNSTONE_MODE_UNSUPPORTED_PROFILE };
struct sunstone_panel;
struct sunstone_display_port {
    const char *name;
    enum sunstone_display_connector connector;
    unsigned controller, lanes;
    uint8_t lane_map[4];
    uint64_t resource_id; /* Board sharing key only; zero unspecified, never authority. */
    const struct sunstone_panel *panel;
};
struct sunstone_display_connector_ops {
    /* Bounded callbacks; deadline is absolute microseconds. Detect returns an
     * IRQ/event generation, incremented across disconnect/reconnect even if
     * both samples are connected. Poll-only providers must report generation 0. */
    int (*detect)(void *,uint64_t,unsigned *connected,uint64_t *generation);
    int (*read_edid)(void *,uint64_t,uint8_t *,size_t,size_t *);
    enum sunstone_display_mode_status (*mode_valid)(void *,const struct sunstone_display_mode *);
};
struct sunstone_display_connector_state {
    const struct sunstone_display_port *port;
    struct sunstone_display_connector_ops ops;
    void *context;
    unsigned connected, changed;
    uint64_t generation, event_generation;
};
/* Caller owns serialization and resource lifetime. These functions allocate
 * nothing, program no clocks and perform no retries. Hotplug is explicit. */
int sunstone_display_detect(struct sunstone_display_connector_state *,uint64_t);
int sunstone_display_get_modes(struct sunstone_display_connector_state *,uint64_t,
                              uint8_t *scratch,size_t,struct sunstone_display_modes *);
enum sunstone_display_mode_status sunstone_display_choose(struct sunstone_display_connector_state *,
    const struct sunstone_display_modes *,const struct sunstone_display_mode *requested,
    struct sunstone_display_mode *selected);

enum sunstone_panel_format { SUNSTONE_PANEL_RGB888, SUNSTONE_PANEL_RGB666_LOOSE };
#define SUNSTONE_PANEL_SYNC_PULSE 1u
#define SUNSTONE_PANEL_BURST 2u
#define SUNSTONE_PANEL_NONCONTINUOUS_CLOCK 4u
#define SUNSTONE_PANEL_LP_COMMANDS 8u
#define SUNSTONE_PANEL_NO_EOT 16u
/* Board/provider resolves named resources. No voltage or GPIO number is
 * inferred from the panel model. Every callback must retain ownership on error. */
enum sunstone_panel_step_kind { SUNSTONE_PANEL_SUPPLY, SUNSTONE_PANEL_GPIO,
    SUNSTONE_PANEL_DELAY_US, SUNSTONE_PANEL_DCS, SUNSTONE_PANEL_BACKLIGHT, SUNSTONE_PANEL_PACKET };
struct sunstone_panel_step {
    enum sunstone_panel_step_kind kind;
    const char *resource;
    uint32_t value;
    const uint8_t *payload;
    size_t bytes;
};
struct sunstone_panel {
    const char *compatible;
    uint64_t id;
    const struct sunstone_display_mode *modes;
    size_t mode_count;
    unsigned lanes, flags;
    enum sunstone_panel_format format;
    const struct sunstone_panel_step *prepare, *unprepare;
    size_t prepare_count, unprepare_count;
    const char *backlight;
    /* Enable steps run after video starts; backlight belongs here. */
    const struct sunstone_panel_step *enable;
    size_t enable_count;
    /* Zero for serial panels; parallel metadata may specify 16/18/24 bits. */
    unsigned bus_width;
};
struct sunstone_panel_ops {
    /* 1 completed, 0 pending, -1 retained failure. A pending step is submitted
     * once by the provider and completed on subsequent calls; delays yield. */
    int (*step)(void *,const struct sunstone_panel_step *,uint64_t);
};
struct sunstone_panel_sequence {
    struct sunstone_panel_sequence *self;
    const struct sunstone_panel *panel;
    struct sunstone_panel_ops ops;
    void *context;
    size_t next;
    uint64_t deadline;
    unsigned unprepare, failed; /* 0 prepare, 1 unprepare, 2 enable after video. */
};
int sunstone_panel_valid(const struct sunstone_panel *);
int sunstone_panel_sequence_init(struct sunstone_panel_sequence *,const struct sunstone_panel *,
    const struct sunstone_panel_ops *,void *,unsigned unprepare,uint64_t deadline);
int sunstone_panel_poll(struct sunstone_panel_sequence *);
/* Source-derived examples for host validation; neither is fitted by default. */
extern const struct sunstone_panel sunstone_panel_innolux_p079zca;
extern const struct sunstone_panel sunstone_panel_lg_lh500wx1_sd03;
#endif
