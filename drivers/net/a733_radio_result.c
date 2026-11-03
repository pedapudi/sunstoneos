/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_radio_result.h"
#include <string.h>
/* The furthest stage the platform's own state proves. The rails are observed
 * once RAILS completes; a passed guard, the power-on and the enumeration each
 * have their own milestone. A faulted platform keeps the stage it reached. */
static unsigned platform_stage(const struct a733_radio_platform *p)
{
    if(p->stage>=RSTAGE_READY&&p->card.state==SDIO_CARD_READY)return A733_RADIO_STAGE_ENUMERATED;
    if(p->stage>=RSTAGE_SUPPLY_WAIT)return A733_RADIO_STAGE_POWERED;
    if(p->first_run_guard&&p->guard_done&&p->stage>=RSTAGE_PINS)return A733_RADIO_STAGE_GUARD_PASSED;
    if(p->stage>RSTAGE_RAILS||(p->stage==RSTAGE_RAILS&&p->rail_count&&p->rail_index==p->rail_count))
        return A733_RADIO_STAGE_RAILS_OBSERVED;
    return A733_RADIO_STAGE_NONE;
}
void a733_radio_result_platform(struct a733_radio_result *r,const struct a733_radio_platform *p)
{
    unsigned later=r->stage>A733_RADIO_STAGE_ENUMERATED;
    uint32_t keep_stage=r->stage;
    if(!r->magic){r->magic=A733_RADIO_RESULT_MAGIC;r->version=A733_RADIO_RESULT_VERSION;r->bytes=sizeof(*r);}
    unsigned stage=platform_stage(p);
    r->stage=later?keep_stage:stage;
    r->failed=p->fault!=0;
    if(r->failed&&!r->failure_stage)r->failure_stage=r->stage+1; /* The stage that did not complete. */
    r->flags=(p->first_run_guard?1u:0)|(p->guard_done?2u:0)|(p->power_on?4u:0);
    memcpy(r->pad_power,p->pad_power,sizeof(r->pad_power));
    r->platform_stage=p->stage;
    r->rail_count=0;
    for(unsigned i=0;i<p->rail_count&&i<A733_RADIO_RESULT_RAILS;++i) {
        /* A snapshot is meaningful once its request has been reaped. */
        if(p->stage<=RSTAGE_RAILS&&i>=p->rail_index)break;
        const struct a733_pmic_snapshot *s=&p->snapshots[i];
        r->rails[i]=(struct a733_radio_result_rail){(uint8_t)s->rail,s->enable,s->selector,
            (uint8_t)((s->enabled?1u:0)|(s->decoded?2u:0)),s->programmed_uv};
        r->rail_count=i+1;
    }
    const struct sdio_card *c=&p->card;
    if(c->state==SDIO_CARD_READY) {
        r->ocr=c->ocr;r->selected_ocr=c->selected_ocr;r->rca=c->rca;r->function_count=c->function_count;
        r->cccr_revision=c->cccr_revision;r->sdio_revision=c->sdio_revision;r->sd_revision=c->sd_revision;
        r->capabilities=c->capabilities;r->wide=p->host.wide!=0;r->card_hz=p->host.card_hz;
        for(unsigned i=0;i<A733_RADIO_RESULT_FUNCTIONS&&i<SDIO_CARD_FUNCTIONS;++i) {
            const struct sdio_function_identity *f=&c->functions[i];
            r->functions[i]=(struct a733_radio_result_function){f->manufacturer,f->device,f->class_code,
                (uint8_t)(f->has_manufacturer?1:0),{0,0}};
        }
    }
}
void a733_radio_result_firmware(struct a733_radio_result *r)
{if(r->stage<A733_RADIO_STAGE_FIRMWARE)r->stage=A733_RADIO_STAGE_FIRMWARE;}
void a733_radio_result_identity(struct a733_radio_result *r,uint32_t chip,uint32_t sub)
{
    r->chip_word=chip;r->subrevision_word=sub;
    if(r->stage<A733_RADIO_STAGE_IDENTIFIED)r->stage=A733_RADIO_STAGE_IDENTIFIED;
}
static unsigned channel(uint16_t mhz)
{
    if(mhz==2484)return 14;
    if(mhz>=2412&&mhz<=2472)return (mhz-2407)/5;
    if(mhz>=5000&&mhz<=5895)return (mhz-5000)/5;
    return 0;
}
void a733_radio_result_ap(struct a733_radio_result *r,const uint8_t bssid[6],const uint8_t *ssid,unsigned ssid_bytes,
    uint16_t frequency,int8_t rssi_dbm)
{
    if(r->aps_reported!=UINT32_MAX)++r->aps_reported;
    unsigned slot=r->ap_count;
    for(unsigned i=0;i<r->ap_count;++i)
        if(!memcmp(r->aps[i].bssid,bssid,6)){slot=i;break;} /* One entry per BSSID: the latest sighting. */
    if(slot==r->ap_count) {
        if(r->ap_count<A733_RADIO_RESULT_APS)++r->ap_count;
        else { /* Full: replace the weakest if this one is stronger. */
            unsigned weakest=0;
            for(unsigned i=1;i<r->ap_count;++i)if((int8_t)r->aps[i].rssi_dbm<(int8_t)r->aps[weakest].rssi_dbm)weakest=i;
            if(rssi_dbm<=(int8_t)r->aps[weakest].rssi_dbm)return;
            slot=weakest;
        }
    }
    struct a733_radio_result_ap *a=&r->aps[slot];
    memset(a,0,sizeof(*a));
    memcpy(a->bssid,bssid,6);
    a->ssid_bytes=(uint8_t)(ssid_bytes>32?32:ssid_bytes);
    if(ssid&&a->ssid_bytes)memcpy(a->ssid,ssid,a->ssid_bytes);
    a->rssi_dbm=(uint8_t)rssi_dbm;a->frequency=frequency;a->channel=(uint8_t)channel(frequency);
}
void a733_radio_result_scan_done(struct a733_radio_result *r,unsigned reported)
{
    if(reported>r->aps_reported)r->aps_reported=reported;
    if(r->stage<A733_RADIO_STAGE_SCANNED)r->stage=A733_RADIO_STAGE_SCANNED;
}
