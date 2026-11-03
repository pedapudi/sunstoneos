/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_radio_result.h"
#include <string.h>
static unsigned platform_stage(const struct a733_radio_platform *p)
{
    if(p->stage>=RSTAGE_READY&&p->card.state==SDIO_CARD_READY)return A733_RADIO_STAGE_ENUMERATED;
    if(p->stage>=RSTAGE_SUPPLY_WAIT)return A733_RADIO_STAGE_POWERED;
    if(p->first_run_guard&&p->guard_done&&p->stage>=RSTAGE_PINS)return A733_RADIO_STAGE_GUARD_PASSED;
    if(p->stage>RSTAGE_RAILS||(p->stage==RSTAGE_RAILS&&p->rail_count&&p->rail_index==p->rail_count))
        return A733_RADIO_STAGE_RAILS_OBSERVED;
    return A733_RADIO_STAGE_NONE;
}
int a733_radio_report_init(struct a733_radio_report *r,const struct a733_radio_allocator *m)
{
    if(!r||!m||!m->allocate||!m->release)return 0;
    memset(r,0,sizeof(*r));r->memory=*m;
    r->head=(struct a733_radio_result_head){.magic=A733_RADIO_RESULT_MAGIC,.version=A733_RADIO_RESULT_VERSION,
        .head_bytes=sizeof(r->head),.ap_entry_bytes=sizeof(struct a733_radio_result_ap)};
    return 1;
}
void a733_radio_report_release(struct a733_radio_report *r)
{
    if(r->aps)r->memory.release(r->memory.context,r->aps);
    r->aps=NULL;r->ap_count=r->ap_capacity=0;
}
void a733_radio_report_platform(struct a733_radio_report *r,const struct a733_radio_platform *p)
{
    struct a733_radio_result_head *h=&r->head;
    unsigned later=h->stage>A733_RADIO_STAGE_ENUMERATED;
    unsigned stage=platform_stage(p);
    if(!later)h->stage=stage;
    h->failed=p->fault!=0;
    if(h->failed&&!h->failure_stage)h->failure_stage=h->stage+1; /* The stage that did not complete. */
    h->flags=(p->first_run_guard?1u:0)|(p->guard_done?2u:0)|(p->power_on?4u:0)|(p->stage==RSTAGE_DOWN?8u:0)|(p->already_on?16u:0)|
        (p->abort_done?32u:0)|((p->abort_flags&16u)?64u:0)|(p->refused?128u:0);
    memcpy(h->pad_power,p->pad_power,sizeof(h->pad_power));
    h->platform_stage=p->stage;
    h->fail_site=p->fail_line?(p->fail_line<<16)|(p->fail_stage&0xffffu):0;
    h->rail_count=0;
    for(unsigned i=0;i<p->rail_count&&i<A733_RADIO_RESULT_RAILS;++i) {
        /* A snapshot is meaningful once its request has been reaped. */
        if(p->stage<=RSTAGE_RAILS&&i>=p->rail_index)break;
        const struct a733_pmic_snapshot *s=&p->snapshots[i];
        h->rails[i]=(struct a733_radio_result_rail){(uint8_t)s->rail,s->enable,s->selector,
            (uint8_t)((s->enabled?1u:0)|(s->decoded?2u:0)),s->programmed_uv};
        h->rail_count=i+1;
    }
    const struct sdio_card *c=&p->card;
    if(c->state==SDIO_CARD_READY) {
        h->ocr=c->ocr;h->selected_ocr=c->selected_ocr;h->rca=c->rca;h->function_count=c->function_count;
        h->cccr_revision=c->cccr_revision;h->sdio_revision=c->sdio_revision;h->sd_revision=c->sd_revision;
        h->capabilities=c->capabilities;h->wide=p->host.wide!=0;h->card_hz=p->host.card_hz;
        for(unsigned i=0;i<A733_RADIO_RESULT_FUNCTIONS&&i<SDIO_CARD_FUNCTIONS;++i) {
            const struct sdio_function_identity *f=&c->functions[i];
            h->functions[i]=(struct a733_radio_result_function){f->manufacturer,f->device,f->class_code,
                (uint8_t)(f->has_manufacturer?1:0),{0,0}};
        }
    }
}
void a733_radio_report_session_failure(struct a733_radio_report *r,unsigned line,unsigned stage)
{if(line&&!r->head.fail_site)r->head.fail_site=(line<<16)|0x100u|(stage&0xffu);}
void a733_radio_report_firmware(struct a733_radio_report *r)
{if(r->head.stage<A733_RADIO_STAGE_FIRMWARE)r->head.stage=A733_RADIO_STAGE_FIRMWARE;}
void a733_radio_report_identity(struct a733_radio_report *r,uint32_t chip,uint32_t sub)
{
    r->head.chip_word=chip;r->head.subrevision_word=sub;
    if(r->head.stage<A733_RADIO_STAGE_IDENTIFIED)r->head.stage=A733_RADIO_STAGE_IDENTIFIED;
}
static unsigned channel(uint16_t mhz)
{
    if(mhz==2484)return 14;
    if(mhz>=2412&&mhz<=2472)return (mhz-2407)/5;
    if(mhz>=5000&&mhz<=5895)return (mhz-5000)/5;
    return 0;
}
static int grow(struct a733_radio_report *r)
{
    if(r->ap_count<r->ap_capacity)return 1;
    size_t capacity=r->ap_capacity?r->ap_capacity*2:16;
    if(capacity<r->ap_capacity||capacity>SIZE_MAX/sizeof(*r->aps))return 0;
    struct a733_radio_result_ap *aps=r->memory.allocate(r->memory.context,capacity*sizeof(*aps));
    if(!aps)return 0;
    if(r->ap_count)memcpy(aps,r->aps,r->ap_count*sizeof(*aps));
    if(r->aps)r->memory.release(r->memory.context,r->aps);
    r->aps=aps;r->ap_capacity=capacity;return 1;
}
void a733_radio_report_ap(struct a733_radio_report *r,const uint8_t bssid[6],const uint8_t *ssid,unsigned ssid_bytes,
    uint16_t frequency,int8_t rssi_dbm)
{
    if(r->head.aps_reported!=UINT32_MAX)++r->head.aps_reported;
    size_t slot=r->ap_count;
    for(size_t i=0;i<r->ap_count;++i)
        if(!memcmp(r->aps[i].bssid,bssid,6)){slot=i;break;} /* One entry per BSSID: the latest sighting. */
    if(slot==r->ap_count) {
        if(!grow(r)){if(r->head.aps_dropped!=UINT32_MAX)++r->head.aps_dropped;return;}
        ++r->ap_count;
    }
    struct a733_radio_result_ap *a=&r->aps[slot];
    memset(a,0,sizeof(*a));
    memcpy(a->bssid,bssid,6);
    a->ssid_bytes=(uint8_t)(ssid_bytes>32?32:ssid_bytes);
    if(ssid&&a->ssid_bytes)memcpy(a->ssid,ssid,a->ssid_bytes);
    a->rssi_dbm=rssi_dbm;a->frequency_mhz=frequency;a->channel=(uint8_t)channel(frequency);
}
void a733_radio_report_scan_done(struct a733_radio_report *r,unsigned reported)
{
    if(reported>r->head.aps_reported)r->head.aps_reported=reported;
    if(r->head.stage<A733_RADIO_STAGE_SCANNED)r->head.stage=A733_RADIO_STAGE_SCANNED;
}
size_t a733_radio_report_bytes(const struct a733_radio_report *r)
{return sizeof(r->head)+r->ap_count*sizeof(struct a733_radio_result_ap);}
size_t a733_radio_report_encode(const struct a733_radio_report *r,void *out,size_t capacity)
{
    size_t bytes=a733_radio_report_bytes(r);
    if(!out||capacity<bytes)return 0;
    struct a733_radio_result_head head=r->head;
    head.bytes=(uint32_t)bytes;head.ap_count=(uint32_t)r->ap_count;
    memcpy(out,&head,sizeof(head));
    if(r->ap_count)memcpy((uint8_t *)out+sizeof(head),r->aps,r->ap_count*sizeof(*r->aps));
    return bytes;
}
