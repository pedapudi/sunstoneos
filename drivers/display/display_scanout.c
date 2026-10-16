#include "display_scanout.h"
#include <stddef.h>
#include <string.h>

static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an > UINTPTR_MAX-x || bn > UINTPTR_MAX-y || (x < y+bn && y < x+an);
}
static void fail(struct display_scanout *s, enum display_scanout_error e)
{
    if (s->state != DISPLAY_SCANOUT_FAULT_RETAINED) {
        s->error = e; s->failed_phase = s->phase; s->state = DISPLAY_SCANOUT_FAULT_RETAINED;
    }
}
static int clock_read(void *ctx, uint64_t *value)
{
    struct display_scanout *s = ctx;
    uint64_t n = 0;
    int ok = s->provider.now_us(s->context, &n);
    if (s->state == DISPLAY_SCANOUT_FAULT_RETAINED) return 0;
    if (ok != 1 || n < s->last_now || n >= s->deadline) {
        fail(s, DISPLAY_SCANOUT_TIME); return 0;
    }
    s->last_now = n; *value = n;
    return 1;
}
static int resources_valid(const struct display_scanout *s, const struct display_resource_token *t)
{
    return t->id && t->generation && t->owner == s->request.owner && t->board_id == s->request.board_id &&
        t->panel_id == s->request.panel_id && t->recovery_generation &&
        t->scopes == DISPLAY_REQUIRED_SCOPES && t->reference_hz == 24000000 && t->host_hz == 150000000 &&
        t->hs_hz == s->request.timing.dsi.hs_hz && t->ls_hz == s->request.timing.dsi.ls_hz;
}
static int frame_valid(const struct display_scanout *s, const struct display_frame_token *t)
{
    const struct display_frame *f = &s->request.frame;
    return t->id && t->generation && t->owner == s->request.owner && t->frame_id == f->frame_id &&
        t->mapping_generation && t->visibility_generation && t->cpu_address == f->cpu_address &&
        t->device_address == f->device_address && t->mapped_bytes == f->mapped_bytes && t->pitch == f->pitch &&
        t->cache_state == DISPLAY_CACHE_IMMUTABLE_DEVICE_READ;
}
static int authenticate(struct display_scanout *s)
{
    if (!resources_valid(s, &s->ownership.resources) || !frame_valid(s, &s->ownership.frame)) {
        fail(s, DISPLAY_SCANOUT_IDENTITY); return 0;
    }
    int ok = s->provider.authenticate(s->context, &s->request, &s->ownership, s->phase, s->deadline);
    if (s->state == DISPLAY_SCANOUT_FAULT_RETAINED) return 0;
    if (ok != 1) { fail(s, DISPLAY_SCANOUT_PROVIDER); return 0; }
    return 1;
}
static int read32(void *ctx, uintptr_t address, uint32_t *v)
{
    struct display_scanout *s = ctx;
    return s->provider.read32(s->context, &s->ownership.resources, address, v);
}
static int write32(void *ctx, uintptr_t address, uint32_t v)
{
    struct display_scanout *s = ctx;
    return s->provider.write32(s->context, &s->ownership.resources, address, v);
}
static int barrier(void *ctx)
{
    struct display_scanout *s = ctx;
    return s->provider.barrier(s->context, &s->ownership.resources);
}
static int phy_admit(void *ctx, const struct a733_dsi_phy_config *c)
{
    struct display_scanout *s = ctx;
    return s->phase == DISPLAY_PREPARE_PHY && c == &s->phy.config && c->lease == s->ownership.resources.id &&
        !s->panel_ready && authenticate(s);
}
static int dsi_admit(void *ctx, const struct a733_dsi_video_config *c, enum a733_dsi_video_action a)
{
    struct display_scanout *s = ctx;
    if (c != &s->dsi.config || c->lease != s->ownership.resources.id || !s->panel_ready ||
        !a733_dsi_phy_ready(&s->phy, 1, 4, c->lease)) return 0;
    int valid = (s->phase == DISPLAY_PREPARE_DSI && a == A733_DSI_VIDEO_PREPARE && !s->de.initialized) ||
        (s->phase == DISPLAY_START_DSI && a == A733_DSI_VIDEO_START &&
         s->de.state == A733_DE_ARMED && s->lcd.state == A733_TCON_ACTIVE) ||
        (s->phase == DISPLAY_OBSERVE_DSI && a == A733_DSI_VIDEO_OBSERVE &&
         s->de.state == A733_DE_ARMED && s->lcd.state == A733_TCON_ACTIVE) ||
        (s->phase == DISPLAY_STOP_DSI && a == A733_DSI_VIDEO_STOP &&
         s->de.state == A733_DE_ARMED && s->lcd.state == A733_TCON_ACTIVE);
    return valid && authenticate(s);
}
static int lcd_admit(void *ctx, const struct a733_tcon_lcd_config *c, enum a733_tcon_lcd_action a)
{
    struct display_scanout *s = ctx;
    if (c != &s->lcd.config || c->lease != s->ownership.resources.id) return 0;
    int valid = (s->phase == DISPLAY_PREPARE_LCD && a == A733_TCON_PREPARE &&
                 s->dsi.state == A733_DSI_VIDEO_READY && !s->de.initialized) ||
        (s->phase == DISPLAY_START_LCD && a == A733_TCON_START &&
         s->dsi.state == A733_DSI_VIDEO_READY && s->de.state == A733_DE_ARMED) ||
        (s->phase == DISPLAY_OBSERVE_LCD && a == A733_TCON_OBSERVE &&
         s->dsi.state == A733_DSI_VIDEO_ACTIVE && s->de.state == A733_DE_ARMED) ||
        (s->phase == DISPLAY_STOP_LCD && a == A733_TCON_STOP &&
         s->dsi.state == A733_DSI_VIDEO_STOPPED && s->de.state == A733_DE_DISABLED_RETAINED &&
         s->ownership.de_drain.kind == DISPLAY_PROOF_DE_DRAIN && s->ownership.de_drain.id);
    return valid && authenticate(s);
}
static int de_admit(void *ctx, const struct a733_de_config *c, enum a733_de_action a)
{
    struct display_scanout *s = ctx;
    if (c != &s->de.config || c->lease != s->ownership.resources.id || c->surface.owner != s->ownership.frame.id) return 0;
    int valid = (s->phase == DISPLAY_PREPARE_DE && a == A733_DE_PREPARE &&
                 s->dsi.state == A733_DSI_VIDEO_READY && s->lcd.state == A733_TCON_READY) ||
        (s->phase == DISPLAY_START_DE && a == A733_DE_START &&
         s->dsi.state == A733_DSI_VIDEO_READY && s->lcd.state == A733_TCON_READY) ||
        (s->phase == DISPLAY_OBSERVE_DE && a == A733_DE_OBSERVE &&
         s->dsi.state == A733_DSI_VIDEO_ACTIVE && s->lcd.state == A733_TCON_ACTIVE) ||
        (s->phase == DISPLAY_STOP_DE && a == A733_DE_STOP &&
         s->dsi.state == A733_DSI_VIDEO_STOPPED && s->lcd.state == A733_TCON_ACTIVE);
    return valid && authenticate(s);
}
int display_scanout_init(struct display_scanout *s, const struct display_request *r,
                         const struct display_provider *p, void *ctx)
{
    if (!s || !r || !p || s->initialized || overlap(s,sizeof(*s),r,sizeof(*r)) ||
        overlap(s,sizeof(*s),p,sizeof(*p)) || !r->owner || !r->board_id || !r->panel_id ||
        !r->frame.frame_id || !r->frame.cpu_address || (r->frame.cpu_address & 63) ||
        r->frame.mapped_bytes > UINTPTR_MAX-r->frame.cpu_address ||
        !p->now_us || !p->acquire_resources || !p->acquire_frame || !p->authenticate ||
        !p->read32 || !p->write32 || !p->barrier || !p->prepare_panel || !p->drain_de || !p->quiet_output) return 0;
    struct a733_de_surface surface = {r->frame.device_address,r->frame.mapped_bytes,r->owner,r->frame.pitch};
    struct a733_de_plan plan;
    if (!a733_de_plan(&r->timing, &surface, &plan)) return 0;
    memset(s, 0, sizeof(*s)); s->request = *r; s->provider = *p; s->context = ctx; s->initialized = 1;
    return 1;
}
static void phase(struct display_scanout *s, enum display_phase next)
{
    s->phase = next; s->begun = s->authenticated = 0; s->phase_after = s->last_now;
}
int display_scanout_prepare(struct display_scanout *s, uint64_t deadline)
{
    if (!s || !s->initialized || s->entered || s->state != DISPLAY_SCANOUT_NEW || deadline <= s->last_now) return 0;
    s->deadline = deadline; s->state = DISPLAY_SCANOUT_PENDING; phase(s, DISPLAY_ACQUIRE_RESOURCES);
    return 1;
}
int display_scanout_request(struct display_scanout *s, enum display_scanout_action a, uint64_t deadline)
{
    if (!s || !s->initialized || s->entered || deadline <= s->last_now ||
        !((a == DISPLAY_SCANOUT_START && s->state == DISPLAY_SCANOUT_READY) ||
          ((a == DISPLAY_SCANOUT_OBSERVE || a == DISPLAY_SCANOUT_STOP) && s->state == DISPLAY_SCANOUT_ACTIVE))) return 0;
    s->deadline = deadline; s->state = DISPLAY_SCANOUT_PENDING;
    phase(s, a == DISPLAY_SCANOUT_START ? DISPLAY_START_DE :
             a == DISPLAY_SCANOUT_OBSERVE ? DISPLAY_OBSERVE_DE : DISPLAY_STOP_DSI);
    return 1;
}
static int begin_leaf(struct display_scanout *s)
{
    uint64_t id = s->ownership.resources.id;
    switch (s->phase) {
    case DISPLAY_PREPARE_PHY: {
        struct a733_dsi_phy_config c = {.ops={phy_admit,read32,write32,barrier,clock_read},.context=s,
            .lease=id,.deadline=s->deadline,.hs_hz=s->request.timing.dsi.hs_hz,
            .ls_hz=s->request.timing.dsi.ls_hz,.controller=1,.lanes=4};
        return a733_dsi_phy_init(&s->phy,&c);
    }
    case DISPLAY_PREPARE_DSI: {
        struct a733_dsi_video_config c = {.ops={dsi_admit,read32,write32,barrier,clock_read},.context=s,
            .lease=id,.controller=1,.timing=s->request.timing.dsi};
        return a733_dsi_video_init(&s->dsi,&c,s->deadline);
    }
    case DISPLAY_PREPARE_LCD: {
        struct a733_tcon_lcd_config c = {.ops={lcd_admit,read32,write32,barrier,clock_read},.context=s,
            .lease=id,.timing=s->request.timing};
        return a733_tcon_lcd_init(&s->lcd,&c,s->deadline);
    }
    case DISPLAY_PREPARE_DE: {
        struct a733_de_config c = {.ops={de_admit,read32,write32,barrier,clock_read},.context=s,.lease=id,
            .timing=s->request.timing,.surface={s->ownership.frame.device_address,s->ownership.frame.mapped_bytes,
                                             s->ownership.frame.id,s->ownership.frame.pitch}};
        return a733_de_init(&s->de,&c,s->deadline);
    }
    case DISPLAY_START_DE: return a733_de_request(&s->de,A733_DE_START,s->deadline);
    case DISPLAY_START_LCD: return a733_tcon_lcd_request(&s->lcd,A733_TCON_START,s->deadline);
    case DISPLAY_START_DSI: return a733_dsi_video_request(&s->dsi,A733_DSI_VIDEO_START,s->deadline);
    case DISPLAY_OBSERVE_DE: return a733_de_request(&s->de,A733_DE_OBSERVE,s->deadline);
    case DISPLAY_OBSERVE_LCD: return a733_tcon_lcd_request(&s->lcd,A733_TCON_OBSERVE,s->deadline);
    case DISPLAY_OBSERVE_DSI: return a733_dsi_video_request(&s->dsi,A733_DSI_VIDEO_OBSERVE,s->deadline);
    case DISPLAY_STOP_DSI: return a733_dsi_video_request(&s->dsi,A733_DSI_VIDEO_STOP,s->deadline);
    case DISPLAY_STOP_DE: return a733_de_request(&s->de,A733_DE_STOP,s->deadline);
    case DISPLAY_STOP_LCD: return a733_tcon_lcd_request(&s->lcd,A733_TCON_STOP,s->deadline);
    default: return 0;
    }
}
static int poll_leaf(struct display_scanout *s)
{
    /* Return 0 pending, 1 complete, -1 retained leaf fault. */
    switch (s->phase) {
    case DISPLAY_PREPARE_PHY: {
        enum a733_dsi_phy_state q = a733_dsi_phy_poll(&s->phy);
        return q == A733_DSI_PHY_PENDING ? 0 : q == A733_DSI_PHY_READY ? 1 : -1;
    }
    case DISPLAY_PREPARE_DSI: case DISPLAY_START_DSI: case DISPLAY_OBSERVE_DSI: case DISPLAY_STOP_DSI: {
        enum a733_dsi_video_state q = a733_dsi_video_poll(&s->dsi);
        enum a733_dsi_video_state expected = s->phase == DISPLAY_PREPARE_DSI ? A733_DSI_VIDEO_READY :
            s->phase == DISPLAY_STOP_DSI ? A733_DSI_VIDEO_STOPPED : A733_DSI_VIDEO_ACTIVE;
        return q == A733_DSI_VIDEO_PENDING ? 0 : q == expected ? 1 : -1;
    }
    case DISPLAY_PREPARE_LCD: case DISPLAY_START_LCD: case DISPLAY_OBSERVE_LCD: case DISPLAY_STOP_LCD: {
        enum a733_tcon_lcd_state q = a733_tcon_lcd_poll(&s->lcd);
        enum a733_tcon_lcd_state expected = s->phase == DISPLAY_PREPARE_LCD ? A733_TCON_READY :
            s->phase == DISPLAY_STOP_LCD ? A733_TCON_STOPPED : A733_TCON_ACTIVE;
        return q == A733_TCON_PENDING ? 0 : q == expected ? 1 : -1;
    }
    default: {
        enum a733_de_state q = a733_de_poll(&s->de);
        enum a733_de_state expected = s->phase == DISPLAY_PREPARE_DE ? A733_DE_READY :
            s->phase == DISPLAY_STOP_DE ? A733_DE_DISABLED_RETAINED : A733_DE_ARMED;
        return q == A733_DE_PENDING ? 0 : q == expected ? 1 : -1;
    }
    }
}
static int acquire(struct display_scanout *s)
{
    int ok;
    if (s->phase == DISPLAY_ACQUIRE_RESOURCES) {
        memcpy(&s->observed_resources,&s->ownership.resources,sizeof(s->observed_resources));
        ok = s->provider.acquire_resources(s->context,&s->request,s->deadline,&s->observed_resources);
        if (!s->ownership.resources.id) memcpy(&s->ownership.resources,&s->observed_resources,sizeof(s->observed_resources));
        if (memcmp(&s->ownership.resources,&s->observed_resources,sizeof(s->observed_resources)) ||
            ((ok == 1 || s->observed_resources.id) && !resources_valid(s,&s->observed_resources))) {
            fail(s,DISPLAY_SCANOUT_IDENTITY); return -1;
        }
    } else {
        memcpy(&s->observed_frame,&s->ownership.frame,sizeof(s->observed_frame));
        ok = s->provider.acquire_frame(s->context,&s->request,&s->ownership.resources,s->deadline,&s->observed_frame);
        if (!s->ownership.frame.id) memcpy(&s->ownership.frame,&s->observed_frame,sizeof(s->observed_frame));
        if (memcmp(&s->ownership.frame,&s->observed_frame,sizeof(s->observed_frame)) ||
            ((ok == 1 || s->observed_frame.id) && !frame_valid(s,&s->observed_frame))) {
            fail(s,DISPLAY_SCANOUT_IDENTITY); return -1;
        }
    }
    return ok;
}
static int proof_valid(const struct display_scanout *s, const struct display_proof *p, enum display_proof_kind kind)
{
    return p->id && p->generation && p->kind == kind && p->completed_us >= s->phase_after && p->completed_us <= s->last_now &&
        p->resource_id == s->ownership.resources.id && p->resource_generation == s->ownership.resources.generation &&
        p->frame_id == s->ownership.frame.id && p->frame_generation == s->ownership.frame.generation;
}
static void completed(struct display_scanout *s)
{
    switch (s->phase) {
    case DISPLAY_ACQUIRE_RESOURCES: phase(s,DISPLAY_ACQUIRE_FRAME); break;
    case DISPLAY_ACQUIRE_FRAME: phase(s,DISPLAY_PREPARE_PHY); break;
    case DISPLAY_PREPARE_PHY: phase(s,DISPLAY_PREPARE_PANEL); break;
    case DISPLAY_PREPARE_PANEL: s->panel_ready=1; phase(s,DISPLAY_PREPARE_DSI); break;
    case DISPLAY_PREPARE_DSI: phase(s,DISPLAY_PREPARE_LCD); break;
    case DISPLAY_PREPARE_LCD: phase(s,DISPLAY_PREPARE_DE); break;
    case DISPLAY_PREPARE_DE: s->state=DISPLAY_SCANOUT_READY; break;
    case DISPLAY_START_DE: phase(s,DISPLAY_START_LCD); break;
    case DISPLAY_START_LCD: phase(s,DISPLAY_START_DSI); break;
    case DISPLAY_START_DSI: case DISPLAY_OBSERVE_DSI: s->state=DISPLAY_SCANOUT_ACTIVE; break;
    case DISPLAY_OBSERVE_DE: phase(s,DISPLAY_OBSERVE_LCD); break;
    case DISPLAY_OBSERVE_LCD: phase(s,DISPLAY_OBSERVE_DSI); break;
    case DISPLAY_STOP_DSI: phase(s,DISPLAY_STOP_DE); break;
    case DISPLAY_STOP_DE: phase(s,DISPLAY_DRAIN_DE); break;
    case DISPLAY_DRAIN_DE: phase(s,DISPLAY_STOP_LCD); break;
    case DISPLAY_STOP_LCD: phase(s,DISPLAY_QUIET_OUTPUT); break;
    case DISPLAY_QUIET_OUTPUT: s->state=DISPLAY_SCANOUT_STOPPED_RETAINED; break;
    }
}
enum display_scanout_state display_scanout_poll(struct display_scanout *s)
{
    if (!s || !s->initialized) return DISPLAY_SCANOUT_FAULT_RETAINED;
    if (s->state != DISPLAY_SCANOUT_PENDING) return s->state;
    if (s->entered) { fail(s,DISPLAY_SCANOUT_REENTRY); return s->state; }
    s->entered=1;
    int ok=0;
    uint64_t n=0;
    unsigned external = s->phase == DISPLAY_ACQUIRE_RESOURCES || s->phase == DISPLAY_ACQUIRE_FRAME ||
        s->phase == DISPLAY_PREPARE_PANEL || s->phase == DISPLAY_DRAIN_DE || s->phase == DISPLAY_QUIET_OUTPUT;
    if (external || !s->begun) {
        if (!clock_read(s,&n)) goto out;
    }
    if (!external) {
        if (!s->begun) {
            if (!begin_leaf(s)) fail(s,DISPLAY_SCANOUT_SEQUENCE);
            else s->begun=1;
            goto out;
        }
        ok=poll_leaf(s);
        if (s->state==DISPLAY_SCANOUT_FAULT_RETAINED) goto out;
        if (ok<0) { fail(s,DISPLAY_SCANOUT_LEAF); goto out; }
    } else {
        if (s->phase == DISPLAY_ACQUIRE_RESOURCES || s->phase == DISPLAY_ACQUIRE_FRAME) ok=acquire(s);
        else if (!s->authenticated) {
            ok=authenticate(s);
            if (!clock_read(s,&n)) goto out;
            if (ok==1 && s->state==DISPLAY_SCANOUT_PENDING) s->authenticated=1;
            goto out;
        } else {
            s->authenticated=0; /* Authenticate again before every pending step. */
            if (s->phase==DISPLAY_PREPARE_PANEL)
                ok=s->provider.prepare_panel(s->context,&s->request,&s->ownership,&s->phy,s->deadline);
            else if (s->phase==DISPLAY_DRAIN_DE)
                ok=s->provider.drain_de(s->context,&s->request,&s->ownership,s->deadline,&s->ownership.de_drain);
            else ok=s->provider.quiet_output(s->context,&s->request,&s->ownership,s->deadline,&s->ownership.output_quiet);
        }
        if (s->state==DISPLAY_SCANOUT_FAULT_RETAINED) goto out;
        if (!clock_read(s,&n)) goto out;
        if (ok!=0 && ok!=1) { fail(s,DISPLAY_SCANOUT_PROVIDER); goto out; }
        if (s->phase==DISPLAY_DRAIN_DE || s->phase==DISPLAY_QUIET_OUTPUT) {
            const struct display_proof *p = s->phase==DISPLAY_DRAIN_DE ? &s->ownership.de_drain : &s->ownership.output_quiet;
            enum display_proof_kind kind = s->phase==DISPLAY_DRAIN_DE ? DISPLAY_PROOF_DE_DRAIN : DISPLAY_PROOF_OUTPUT_QUIET;
            if ((ok==1 && !proof_valid(s,p,kind)) || (ok==0 && p->id)) {
                fail(s,DISPLAY_SCANOUT_IDENTITY); goto out;
            }
        }
    }
    if (ok==1) completed(s);
out:
    s->entered=0;
    return s->state;
}
