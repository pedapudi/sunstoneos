#include "sunstone_display.h"
int sunstone_display_detect(struct sunstone_display_connector_state *s,uint64_t d)
{
    unsigned connected;uint64_t generation;
    if(!s||!s->port||!s->ops.detect||!d||
       s->ops.detect(s->context,d,&connected,&generation)!=1||connected>1)return 0;
    unsigned changed=s->connected!=connected||s->event_generation!=generation;
    if(changed&&s->generation==UINT64_MAX)return 0;
    s->changed=changed;if(changed)++s->generation;
    s->connected=connected;s->event_generation=generation;return 1;
}
int sunstone_display_get_modes(struct sunstone_display_connector_state *s,uint64_t d,
    uint8_t *scratch,size_t capacity,struct sunstone_display_modes *out)
{
    if(!s||!out||!scratch||capacity<128||capacity>SUNSTONE_DISPLAY_MAX_EDID||
       !sunstone_display_detect(s,d)||!s->connected)return 0;
    if((s->port->connector==SUNSTONE_DISPLAY_DSI||s->port->connector==SUNSTONE_DISPLAY_PARALLEL)){
        const struct sunstone_panel *p=s->port->panel;
        if(!sunstone_panel_valid(p)||p->mode_count>SUNSTONE_DISPLAY_MAX_MODES)return 0;
        struct sunstone_display_modes m={0};m.count=p->mode_count;
        for(size_t i=0;i<m.count;++i)m.modes[i]=p->modes[i];
        m.source_port=s->port;m.generation=s->generation;*out=m;return 1;
    }
    size_t bytes=0;uint64_t generation=s->generation;
    if(!s->ops.read_edid||s->ops.read_edid(s->context,d,scratch,capacity,&bytes)!=1||
       bytes>capacity)return 0;
    struct sunstone_display_modes m;
    if(sunstone_display_edid_parse(scratch,bytes,&m)!=SUNSTONE_EDID_OK||
       !sunstone_display_detect(s,d)||!s->connected||generation!=s->generation)return 0;
    m.source_port=s->port;m.generation=s->generation;*out=m;return 1;
}
enum sunstone_display_mode_status sunstone_display_choose(struct sunstone_display_connector_state *s,
    const struct sunstone_display_modes *m,const struct sunstone_display_mode *requested,
    struct sunstone_display_mode *selected)
{
    if(!s||!s->port||!m||!selected||m->count>SUNSTONE_DISPLAY_MAX_MODES||!s->ops.mode_valid)
        return SUNSTONE_MODE_INVALID;
    if(!s->connected||m->source_port!=s->port||m->generation!=s->generation)return SUNSTONE_MODE_LINK;
    if((s->port->connector==SUNSTONE_DISPLAY_DSI||s->port->connector==SUNSTONE_DISPLAY_PARALLEL)&&!s->port->panel)return SUNSTONE_MODE_NO_PANEL;
    enum sunstone_display_mode_status last=SUNSTONE_MODE_LINK;
    /* Preferred EDID timing first. An explicit request never silently falls back. */
    for(unsigned preferred=0;preferred<2;++preferred)for(size_t i=0;i<m->count;++i){
        const struct sunstone_display_mode *v=&m->modes[i];
        if(!!(v->flags&SUNSTONE_MODE_PREFERRED)!=(preferred==0))continue;
        if(requested&&!sunstone_display_mode_equal(v,requested))continue;
        last=sunstone_display_mode_valid(v)?s->ops.mode_valid(s->context,v):SUNSTONE_MODE_INVALID;
        if(last==SUNSTONE_MODE_OK){*selected=*v;return last;}
    }
    return last;
}
int sunstone_panel_valid(const struct sunstone_panel *p)
{
    if(!p||!p->compatible||!p->id||!p->modes||!p->mode_count||p->mode_count>SUNSTONE_DISPLAY_MAX_MODES||
       p->lanes>4||(!p->lanes&&(p->bus_width!=16&&p->bus_width!=18&&p->bus_width!=24))||
       (p->lanes&&p->bus_width)||p->flags&~31u||
       (unsigned)p->format>SUNSTONE_PANEL_RGB666_LOOSE||p->prepare_count>256||p->unprepare_count>256||p->enable_count>256||
       (p->prepare_count&&!p->prepare)||(p->unprepare_count&&!p->unprepare)||(p->enable_count&&!p->enable))return 0;
    for(size_t i=0;i<p->mode_count;++i)if(!sunstone_display_mode_valid(&p->modes[i]))return 0;
    for(unsigned reverse=0;reverse<3;++reverse){
        const struct sunstone_panel_step *steps=reverse==2?p->enable:reverse?p->unprepare:p->prepare;
        size_t count=reverse==2?p->enable_count:reverse?p->unprepare_count:p->prepare_count;
        for(size_t i=0;i<count;++i){const struct sunstone_panel_step *s=&steps[i];
            if((unsigned)s->kind>SUNSTONE_PANEL_PACKET||s->bytes>256||
               ((s->kind==SUNSTONE_PANEL_DCS||s->kind==SUNSTONE_PANEL_PACKET)?(!s->payload||!s->bytes):s->bytes!=0)||
               ((s->kind==SUNSTONE_PANEL_SUPPLY||s->kind==SUNSTONE_PANEL_GPIO||s->kind==SUNSTONE_PANEL_BACKLIGHT)&&!s->resource)||
               (s->kind==SUNSTONE_PANEL_DELAY_US&&!s->value))return 0;
        }
    }
    return 1;
}
int sunstone_panel_sequence_init(struct sunstone_panel_sequence *s,const struct sunstone_panel *p,
    const struct sunstone_panel_ops *ops,void *context,unsigned unprepare,uint64_t deadline)
{
    if(!s||s->self||!sunstone_panel_valid(p)||!ops||!ops->step||unprepare>2||!deadline)return 0;
    *s=(struct sunstone_panel_sequence){.self=s,.panel=p,.ops=*ops,.context=context,
        .deadline=deadline,.unprepare=unprepare};return 1;
}
int sunstone_panel_poll(struct sunstone_panel_sequence *s)
{
    if(!s||s->self!=s||s->failed)return -1;
    size_t count=s->unprepare==2?s->panel->enable_count:s->unprepare?s->panel->unprepare_count:s->panel->prepare_count;
    if(s->next>count){s->failed=1;return -1;}
    if(s->next==count)return 1;
    const struct sunstone_panel_step *steps=s->unprepare==2?s->panel->enable:s->unprepare?s->panel->unprepare:s->panel->prepare;
    int r=s->ops.step(s->context,&steps[s->next],s->deadline);
    if(r!=0&&r!=1){s->failed=1;return -1;}
    if(r==1)++s->next;
    return s->next==count?1:0;
}
