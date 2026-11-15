/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_radio_result.h"
#include "aic8800_firmware.h"
#include <string.h>
static unsigned platform_stage(const struct a733_radio_platform *p)
{
    unsigned stage=p->stop_requested&&p->stop_phase==3?p->stop_from:p->stage;
    if(stage>=RSTAGE_READY&&p->card.state==SDIO_CARD_READY)return A733_RADIO_STAGE_ENUMERATED;
    if(stage>=RSTAGE_SUPPLY_WAIT)return A733_RADIO_STAGE_POWERED;
    if(p->first_run_guard&&p->guard_done&&stage>=RSTAGE_PINS)return A733_RADIO_STAGE_GUARD_PASSED;
    if(stage>RSTAGE_RAILS||(stage==RSTAGE_RAILS&&p->rail_count&&p->rail_index==p->rail_count))
        return A733_RADIO_STAGE_RAILS_OBSERVED;
    return A733_RADIO_STAGE_NONE;
}
/* The stage numbers are stored in records and never change, so they do not order the stages: identified is stored
 * as 6 and firmware as 5, though the module is identified first. Order and succession come from these two. */
static unsigned rank(unsigned stage)
{
    switch(stage) {
    case A733_RADIO_STAGE_IDENTIFIED:return A733_RADIO_STAGE_ENUMERATED+1;
    case A733_RADIO_STAGE_FIRMWARE:return A733_RADIO_STAGE_ENUMERATED+2;
    case A733_RADIO_STAGE_SCANNED:case A733_RADIO_STAGE_ASSOCIATED:return stage+1;
    default:return stage;
    }
}
static unsigned next_stage(unsigned stage)
{
    switch(stage) {
    case A733_RADIO_STAGE_ENUMERATED:return A733_RADIO_STAGE_IDENTIFIED;
    case A733_RADIO_STAGE_IDENTIFIED:return A733_RADIO_STAGE_FIRMWARE;
    case A733_RADIO_STAGE_FIRMWARE:return A733_RADIO_STAGE_SCANNED;
    default:return stage+1;
    }
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
    unsigned later=rank(h->stage)>rank(A733_RADIO_STAGE_ENUMERATED);
    unsigned stage=platform_stage(p);
    if(!later)h->stage=stage;
    h->failed=p->fault!=0;
    if(h->failed&&!h->failure_stage)h->failure_stage=next_stage(h->stage); /* The stage that did not complete. */
    h->flags=(p->first_run_guard?1u:0)|(p->guard_done?2u:0)|(p->power_on?4u:0)|(p->stage==RSTAGE_DOWN?8u:0)|(p->already_on?16u:0)|
        (p->abort_done?32u:0)|((p->abort_flags&16u)?64u:0)|(p->refused?128u:0)|(a733_radio_platform_clean(p)?256u:0);
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
{if(line&&!r->head.fail_site)r->head.fail_site=(line<<16)|0x100u|(stage&0xfeffu);}
void a733_radio_report_firmware_progress(struct a733_radio_report *r,const struct aic8800_firmware *f,uint64_t ticks_per_ms)
{
    if(f->state==AIC8800_FW_NEW)return;
    unsigned ms=ticks_per_ms?(unsigned)((f->last_confirm/ticks_per_ms)&0xffffu):0;
    r->head.firmware_flags=((unsigned)f->state&15u)|(((unsigned)f->error&15u)<<4)|((unsigned)f->phase<<8)|(ms<<16);
    r->head.firmware_progress=((unsigned)f->image_offset&0x7ffffu)|((f->pairs_done&0xfffu)<<19);
}
void a733_radio_report_firmware(struct a733_radio_report *r)
{if(rank(r->head.stage)<rank(A733_RADIO_STAGE_FIRMWARE))r->head.stage=A733_RADIO_STAGE_FIRMWARE;}
void a733_radio_report_identity(struct a733_radio_report *r,uint32_t chip,uint32_t sub)
{
    r->head.chip_word=chip;r->head.subrevision_word=sub;
    if(rank(r->head.stage)<rank(A733_RADIO_STAGE_IDENTIFIED))r->head.stage=A733_RADIO_STAGE_IDENTIFIED;
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
void a733_radio_report_ap(struct a733_radio_report *r,const uint8_t bssid[6],uint16_t frequency,int8_t rssi_dbm)
{
    if(r->head.aps_reported!=UINT32_MAX)++r->head.aps_reported;
    if(r->scan.sightings!=UINT32_MAX)++r->scan.sightings;
    r->has_scan=1;
    size_t slot=r->ap_count;
    for(size_t i=0;i<r->ap_count;++i)
        if(!memcmp(r->aps[i].bssid,bssid,6)){slot=i;break;} /* One entry per BSSID: the latest sighting. */
    if(slot<r->ap_count&&r->scan.merges!=UINT32_MAX)++r->scan.merges;
    if(slot==r->ap_count) {
        if(!grow(r)){if(r->head.aps_dropped!=UINT32_MAX)++r->head.aps_dropped;return;}
        ++r->ap_count;
    }
    struct a733_radio_result_ap *a=&r->aps[slot];
    memset(a,0,sizeof(*a));
    memcpy(a->bssid,bssid,6);
    a->rssi_dbm=rssi_dbm;a->frequency_mhz=frequency;a->channel=(uint8_t)channel(frequency);
}
void a733_radio_report_scan_done(struct a733_radio_report *r,unsigned reported)
{
    if(reported>r->head.aps_reported)r->head.aps_reported=reported;
    r->scan.firmware_reported=reported>UINT32_MAX?UINT32_MAX:(uint32_t)reported;r->has_scan=1;
    if(rank(r->head.stage)<rank(A733_RADIO_STAGE_SCANNED))r->head.stage=A733_RADIO_STAGE_SCANNED;
}
void a733_radio_report_set_aside(struct a733_radio_report *r,uint64_t set_aside)
{
    r->scan.set_aside=set_aside>UINT32_MAX?UINT32_MAX:(uint32_t)set_aside;
    if(set_aside)r->has_scan=1;
}
void a733_radio_report_association(struct a733_radio_report *r,const struct a733_radio_assoc *a)
{r->assoc=*a;r->has_assoc=1;}
void a733_radio_report_rsn(struct a733_radio_report *r,const struct a733_radio_rsn *s)
{r->rsn=*s;r->has_rsn=1;}
static unsigned suite(const uint8_t *p,uint8_t *flags)
{
    if(p[0]==0x00&&p[1]==0x0f&&p[2]==0xac)return p[3];
    *flags|=2u;return 256u;
}
void a733_radio_rsn_summarize(struct a733_radio_rsn *s,const uint8_t *e,size_t n)
{
    memset(s,0,sizeof(*s));s->group=A733_RADIO_RSN_SUITE_ABSENT;
    if(!e){s->check=A733_RADIO_RSN_NO_ELEMENT;return;}
    s->flags=1u;
    if(n<4||e[0]!=48||(size_t)e[1]+2u!=n){s->flags|=4u;return;}
    const uint8_t *p=e+2,*end=e+n;
    s->version=(uint16_t)(p[0]|p[1]<<8);p+=2;
    /* The body's fields in order; each is optional from the end (IEEE 802.11-2020 9.4.2.24). */
    if(end-p>=4){s->group=p[0]==0x00&&p[1]==0x0f&&p[2]==0xac?p[3]:A733_RADIO_RSN_SUITE_OTHER;p+=4;}
    else if(end!=p){s->flags|=4u;return;}
    for(unsigned list=0;list<2;++list){
        if(end==p)return;
        if(end-p<2){s->flags|=4u;return;}
        size_t count=(size_t)(p[0]|p[1]<<8);p+=2;
        if(!count||count>(size_t)(end-p)/4u){s->flags|=4u;return;}
        s->flags|=list?16u:8u;
        for(size_t i=0;i<count;++i,p+=4){
            unsigned type=suite(p,&s->flags);
            if(type<32u){if(list)s->akm|=1u<<type;else s->pairwise|=1u<<type;}
        }
    }
    if(end==p)return;
    if(end-p<2){s->flags|=4u;return;}
    s->capabilities=(uint16_t)(p[0]|p[1]<<8);
}
enum a733_radio_rsn_check a733_radio_rsn_classify(const struct a733_radio_rsn *s)
{
    if(!(s->flags&1u))return A733_RADIO_RSN_NO_ELEMENT;
    if(s->flags&4u)return A733_RADIO_RSN_MALFORMED;
    if(s->group!=4u&&s->group!=A733_RADIO_RSN_SUITE_ABSENT)return A733_RADIO_RSN_GROUP_NOT_CCMP; /* 4: CCMP-128 */
    if((s->flags&8u)&&!(s->pairwise&(1u<<4)))return A733_RADIO_RSN_PAIRWISE_NO_CCMP;
    if(s->capabilities&(1u<<6))return A733_RADIO_RSN_MFP_REQUIRED;
    if(!(s->akm&(1u<<2)))return A733_RADIO_RSN_NO_PSK; /* 2: PSK; an absent list means 802.1X */
    return A733_RADIO_RSN_PASSED;
}
size_t a733_radio_report_bytes(const struct a733_radio_report *r)
{return sizeof(r->head)+r->ap_count*sizeof(struct a733_radio_result_ap)+(r->has_scan?sizeof(r->scan):0)+
    (r->has_assoc?sizeof(r->assoc):0)+(r->has_rsn?sizeof(r->rsn):0);}
size_t a733_radio_report_encode(const struct a733_radio_report *r,void *out,size_t capacity)
{
    size_t bytes=a733_radio_report_bytes(r);
    if(!out||capacity<bytes)return 0;
    struct a733_radio_result_head head=r->head;
    head.bytes=(uint32_t)bytes;head.ap_count=(uint32_t)r->ap_count;
    if(r->has_assoc)head.flags|=512u;
    if(r->has_scan)head.flags|=1024u;
    if(r->has_rsn)head.flags|=2048u;
    uint8_t *at=out;
    memcpy(at,&head,sizeof(head));at+=sizeof(head);
    if(r->ap_count){memcpy(at,r->aps,r->ap_count*sizeof(*r->aps));at+=r->ap_count*sizeof(*r->aps);}
    if(r->has_scan){memcpy(at,&r->scan,sizeof(r->scan));at+=sizeof(r->scan);}
    if(r->has_assoc){memcpy(at,&r->assoc,sizeof(r->assoc));at+=sizeof(r->assoc);}
    if(r->has_rsn)memcpy(at,&r->rsn,sizeof(r->rsn));
    return bytes;
}
