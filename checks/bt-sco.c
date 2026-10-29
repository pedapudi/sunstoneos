/* SPDX-License-Identifier: Apache-2.0 */
#include "rfcomm-server-fixture.h"
#include "bt-config-fixture.h"
#include "bt_sco.h"
#include "sco-ref.h"
static void audio_identify(struct fixture *f,unsigned supported)
{
    init(f);config_service_current=1;
    const unsigned op[]={0x0c03,0x1001,0x1002,0x1003,0x1005,0x1009};
    const unsigned lengths[]={1,9,65,9,8,7};
    for(unsigned i=0;i<6;++i){send(f,op[i]);uint8_t p[68]={1,op[i],op[i]>>8,0};
        if(i==1)p[4]=13;
        if(i==2){uint8_t *c=p+4;c[0]=0x30;c[1]=3;c[5]=0x40;c[6]=1;c[7]=0xcc;c[8]=3;c[63]=0x70;
            if(supported){c[2]|=0x20;c[10]|=0x18;c[16]|=0x20;c[18]|=0x0c;c[29]|=0x18;}}
        if(i==3){p[5]=0x28;p[6]=1;p[7]=0x80;p[9]=0x20;}
        if(i==4){const uint8_t b[]={32,0,60,2,0,3,0};memcpy(p+4,b,7);}
        if(i==5){const uint8_t b[]={0x31,0x32,0x33,0x34,0x35,0x36};memcpy(p+4,b,6);}
        event(f,HCI_EV_CMD_COMPLETE,p,3+lengths[i]);
        for(unsigned j=0;j<4 && f->hci.stage==i;++j)step(f,64);
        assert(f->hci.stage==i+1);
    }
}
static void audio_config_ready(struct fixture *f,struct bt_acl *a)
{
    audio_identify(f,1);struct bt_hci_incoming_config c=configuration(f);c.synchronous_audio=1;
    assert(bt_hci_configure_incoming(&f->hci,&c,9000)==1);
    const unsigned op[]={0x0c01,0x0c05,0x0c16,0x0c15,0x0c1c,0x0c1b,0x0c2f,0x0c2e,0x0c5b,0x0c5a,0x0c1a,0x0c19};
    const unsigned length[]={8,1,2,0,4,0,1,0,1,0,1,0};
    for(unsigned i=0;i<12;++i){
        if(i<6){config_send(f,i);config_reply(f,i,0);step(f,64);continue;}
        if(f->hci.command==BT_HCI_IDLE)step(f,64);
        size_t start=f->tn;step(f,64);assert(f->hci.command==BT_HCI_WAITING);
        uint8_t expected[]={1,op[i],op[i]>>8,length[i],i==10 ? 2 : 1};
        assert(f->tn==start+4+length[i] && !memcmp(f->tx+start,expected,4+length[i]));
        uint8_t reply[]={1,op[i],op[i]>>8,0,i==11 ? 2 : 1};
        event(f,HCI_EV_CMD_COMPLETE,reply,length[i] ? 4 : 5);step(f,64);
        assert(f->hci.configuration_stage==i+1 && f->hci.synchronous_ready==(i==11));
    }
    assert(f->hci.command_tag==18 && bt_hci_configuration_complete(&f->hci));
    memset(a,0,sizeof(*a));assert(bt_acl_init(a,&f->hci)==1);
}
#include "hfp-audio-fixture.h"
struct audio_fixture {struct gateway g;struct bt_sco q;unsigned authority,secure;uint64_t link;};
static int audio_current(void *ctx,const struct bt_h4_binding *b,uint64_t generation,uint64_t latency)
{struct audio_fixture *f=ctx;return f->authority && b->uart==&f->g.rf.d.lower.f.uart && generation==27 && latency==1000;}
static uint64_t security_generation=31;
static int audio_security(void *ctx,const struct bt_hfp_ag *ag,struct bt_sco_security *security)
{struct audio_fixture *f=ctx;if(!f->authority || ag!=&f->g.ag)return 0;*security=(struct bt_sco_security){security_generation,f->secure};return 1;}
static void audio_start(struct audio_fixture *f,unsigned listen,unsigned secure)
{
    memset(f,0,sizeof(*f));establish(&f->g);f->authority=1;f->secure=secure;
    struct bt_sco_binding b={27,1000,2000,audio_current,audio_security,f};
    assert(bt_sco_init(&f->q,&f->g.rf.d.lower.a,&b)==1);
    assert(bt_sco_open(&f->q,&f->g.ag,listen,100000,&f->link)==1 && f->link==1);
    assert(!f->g.ag.indicators[0] && !f->g.ag.indicators[1]);
}
static int audio_poll(struct audio_fixture *f)
{struct fixture *h=&f->g.rf.d.lower.f;h->room=1024;++h->now;return bt_sco_step(&f->q,64,1000000);}
static size_t command(struct audio_fixture *f,unsigned opcode)
{
    struct fixture *h=&f->g.rf.d.lower.f;size_t before=h->tn;
    for(unsigned i=0;i<10 && (h->hci.opcode!=opcode || h->hci.command!=BT_HCI_WAITING);++i)assert(audio_poll(f)==1);
    assert(h->hci.opcode==opcode && h->hci.command==BT_HCI_WAITING);
    assert(h->tx[before]==1 && h->tx[before+1]==(opcode&255) && h->tx[before+2]==(opcode>>8));return before;
}
static void status(struct audio_fixture *f,unsigned opcode,unsigned code)
{cs(&f->g.rf.d.lower.f,opcode,1,code);assert(audio_poll(f)==1);}
static void remote(struct audio_fixture *f,unsigned esco)
{
    size_t start=command(f,0x041b);struct fixture *h=&f->g.rf.d.lower.f;
    const uint8_t expected[]={1,0x1b,4,2,0x23,0};assert(h->tn-start==6 && !memcmp(h->tx+start,expected,6));
    status(f,0x041b,0);assert(f->q.links[0].phase==BT_SCO_FEATURE_WAIT);
    uint8_t p[]={0,0x23,0,0,0x28,1,esco ? 0x80 : 0,0,0x20,0,0};
    event(h,0x0b,p,sizeof(p));assert(audio_poll(f)==1);
}
static void enhanced(struct audio_fixture *f,unsigned incoming,unsigned profile)
{
    unsigned op=incoming ? 0x043e : 0x043d;
    struct fixture *h=&f->g.rf.d.lower.f;size_t start;
    if(h->hci.opcode==op && h->hci.command==BT_HCI_SENDING)start=h->tn;
    else start=h->tn;
    command(f,op);
    struct hci_cp_enhanced_setup_sync_conn ref={0};
    ref.handle=0x23;ref.tx_bandwidth=ref.rx_bandwidth=8000;
    ref.tx_coding_format.id=ref.rx_coding_format.id=2;
    ref.tx_codec_frame_size=ref.rx_codec_frame_size=60;
    ref.in_bandwidth=ref.out_bandwidth=16000;ref.in_coding_format.id=ref.out_coding_format.id=4;
    ref.in_coded_data_size=ref.out_coded_data_size=16;ref.in_pcm_data_format=ref.out_pcm_data_format=2;
    const unsigned masks[]={0,0x3c8,0x380,0x3c4,0x3c1},latencies[]={0,7,12,5,4};
    ref.pkt_type=(uint16_t)masks[profile];ref.max_latency=(uint16_t)latencies[profile];ref.retrans_effort=profile==1 ? 1 : profile==2 ? 2 : 0;
    unsigned n=incoming ? 63 : 59;assert(h->tn==start+4+n && h->tx[start+3]==n);
    if(!incoming)assert(!memcmp(h->tx+start+4,&ref,sizeof(ref)));
    else {const uint8_t address[]={1,2,3,4,5,6};assert(!memcmp(h->tx+start+4,address,6));assert(!memcmp(h->tx+start+10,(uint8_t *)&ref+2,sizeof(ref)-2));}
    status(f,op,0);assert(f->q.links[0].phase==BT_SCO_SETUP_WAIT);
}
static void complete(struct audio_fixture *f,unsigned status_code,unsigned handle,unsigned type)
{
    uint8_t p[]={status_code,handle,handle>>8,1,2,3,4,5,6,type,type ? 6 : 0,type ? 2 : 0,type ? 30 : 0,0,type ? 30 : 0,0,2};
    event(&f->g.rf.d.lower.f,0x2c,p,sizeof(p));assert(audio_poll(f)==1);
}
static void opened(struct audio_fixture *f)
{audio_start(f,0,0);remote(f,1);enhanced(f,0,1);complete(f,0,0,2);assert(f->q.links[0].phase==BT_SCO_OPEN && f->q.links[0].handle_valid && !f->q.links[0].handle);}
static void sco_input(struct audio_fixture *f,unsigned handle,unsigned status_code,const uint8_t *p,unsigned n)
{
    uint8_t header[]={3,handle,(handle>>8)|(status_code<<4),n};struct fixture *h=&f->g.rf.d.lower.f;
    bytes(h,header,4);bytes(h,p,n);
}
static uint64_t transmit(struct audio_fixture *f)
{
    const uint8_t pcm[]={1,0,0xff,0x7f,0,0x80};uint64_t serial=0;
    assert(bt_sco_send(&f->q,f->link,pcm,sizeof(pcm),100000,&serial)==1);
    size_t before=f->g.rf.d.lower.f.tn;
    for(unsigned i=0;i<8 && f->q.tx[0].state!=BT_SCO_SENT;++i)assert(audio_poll(f)==1);
    const uint8_t expected[]={3,0,0,6,1,0,0xff,0x7f,0,0x80};
    assert(f->g.rf.d.lower.f.tn==before+sizeof(expected) && !memcmp(f->g.rf.d.lower.f.tx+before,expected,sizeof(expected)));
    assert(f->q.credits==2 && f->q.tx[0].state==BT_SCO_SENT);return serial;
}
static void credits_and_audio(void)
{
    struct audio_fixture f;audio_start(&f,0,0);struct simulation *s=&f.g.rf.d.lower;
    uint64_t refused=begin_connect(&s->f,&s->a,9);cs(&s->f,HCI_OP_CREATE_CONN,1,0x0e);astep(&s->a);
    assert(s->a.links[refused-1].state==BT_ACL_CLOSED && !s->a.links[refused-1].handle_valid);
    remote(&f,1);enhanced(&f,0,1);complete(&f,0,0,2);
    assert(f.q.links[0].phase==BT_SCO_OPEN && !f.q.links[0].handle);
    uint64_t audio=transmit(&f);
    const uint8_t acl[]={1,2,3,4};uint64_t data;assert(bt_acl_submit(&s->a,s->link,2,acl,sizeof(acl),100000,&data)==1);
    assert(audio_poll(&f)==1);assert(audio_poll(&f)==1);assert(audio_poll(&f)==1);assert(s->a.credits==1);
    const uint8_t mixed[]={2,0x23,0,1,0,0,0,1,0};event(&s->f,0x13,mixed,sizeof(mixed));assert(audio_poll(&f)==1);
    assert(s->a.credits==2 && f.q.credits==3 && bt_acl_result(&s->a)->serial==data && bt_sco_result(&f.q)->serial==audio);
    assert(bt_acl_release(&s->a,data)==1 && bt_sco_release_result(&f.q,audio)==1);
    const uint8_t pcm[]={1,2,3};for(unsigned i=0;i<4;++i){sco_input(&f,0,i,pcm,sizeof(pcm));assert(audio_poll(&f)==1);}
    for(unsigned i=0;i<4;++i){const struct bt_sco_packet *r=bt_sco_receive(&f.q);
        assert(r && r->status==i && r->length==3 && !memcmp(r->bytes,pcm,3));assert(bt_sco_release_receive(&f.q,r->serial)==1);}
    /* Pool exhaustion retains the exact lower record, then delivers in order. */
    for(unsigned i=0;i<9;++i){uint8_t x=(uint8_t)i;sco_input(&f,0,0,&x,1);assert(audio_poll(&f)==1);}
    assert(f.q.held_rx[0] || f.q.held_rx[1] || f.q.held_rx[2]);
    uint64_t first=bt_sco_receive(&f.q)->serial;assert(bt_sco_release_receive(&f.q,first)==1);assert(audio_poll(&f)==1);
    for(unsigned i=1;i<9;++i){const struct bt_sco_packet *r=bt_sco_receive(&f.q);assert(r && r->bytes[0]==i);assert(bt_sco_release_receive(&f.q,r->serial)==1);}
    audio=transmit(&f);assert(bt_sco_close(&f.q,f.link,100000)==1);
    command(&f,0x0406);status(&f,0x0406,0);const uint8_t done[]={0,0,0,0x13};event(&s->f,5,done,4);assert(audio_poll(&f)==1);
    assert(f.q.links[0].phase==BT_SCO_CLOSED && f.q.credits==3 && bt_sco_result(&f.q)->serial==audio && bt_sco_result(&f.q)->state==BT_SCO_FLUSHED);
    assert(bt_sco_open(&f.q,&f.g.ag,0,100000,&audio)==-1);
    puts("Synchronous PCM bytes, mixed typed credits, loss metadata, retained RX and confirmed disconnect passed");
}
static void mixed_refusal(void)
{
    struct audio_fixture f;opened(&f);transmit(&f);struct simulation *s=&f.g.rf.d.lower;
    const uint8_t data[]={1};uint64_t serial;assert(bt_acl_submit(&s->a,1,2,data,1,100000,&serial)==1);assert(audio_poll(&f)==1);assert(audio_poll(&f)==1);assert(audio_poll(&f)==1);
    const uint8_t bad[]={2,0x23,0,1,0,0,0,2,0};event(&s->f,0x13,bad,sizeof(bad));assert(audio_poll(&f)==-1);
    assert(s->a.credits==1 && f.q.credits==2 && f.q.tx[0].state==BT_SCO_SENT);
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i)if(s->a.packets[i].serial==serial)assert(s->a.packets[i].state==BT_ACL_SENT);
}
static void incoming_and_fallback(void)
{
    struct audio_fixture f;audio_start(&f,1,0);
    /* An actual incoming request may precede remote-feature query completion. */
    const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,2};event(&f.g.rf.d.lower.f,4,request,10);
    remote(&f,1);assert(f.q.links[0].requested);enhanced(&f,1,1);complete(&f,0,1,2);assert(f.q.links[0].phase==BT_SCO_OPEN);
    /* Secure Connections selects safe S4 and forbids an incoming SCO link. */
    audio_start(&f,1,1);remote(&f,1);uint8_t sco_request[]={1,2,3,4,5,6,4,4,0x24,0};
    event(&f.g.rf.d.lower.f,4,sco_request,10);assert(audio_poll(&f)==1);
    size_t at=command(&f,0x042a);const uint8_t reject[]={1,0x2a,4,7,1,2,3,4,5,6,0x0e};
    assert(!memcmp(f.g.rf.d.lower.f.tx+at,reject,sizeof(reject)));status(&f,0x042a,0);complete(&f,0x0e,0,0);assert(f.q.links[0].phase==BT_SCO_CLOSED);
    audio_start(&f,0,1);remote(&f,1);enhanced(&f,0,2);complete(&f,0x1a,0,0);assert(f.q.links[0].phase==BT_SCO_CLOSED);
    /* A clean S1 failure proceeds through the HFP SCO safe settings only. */
    audio_start(&f,0,0);remote(&f,1);enhanced(&f,0,1);complete(&f,0x1a,0,0);
    enhanced(&f,0,3);complete(&f,0x1a,0,0);enhanced(&f,0,4);complete(&f,0,2,0);assert(f.q.links[0].phase==BT_SCO_OPEN);
}
static void refusals_and_partial(void)
{
    struct fixture raw;audio_identify(&raw,0);struct bt_hci before=raw.hci;struct bt_hci_incoming_config c=configuration(&raw);c.synchronous_audio=1;
    assert(bt_hci_configure_incoming(&raw.hci,&c,9000)==-1 && !memcmp(&before,&raw.hci,sizeof(before)));
    struct audio_fixture f;opened(&f);uint64_t serial;const uint8_t sample[]={0,0};
    assert(bt_sco_send(&f.q,1,sample,1,100000,&serial)==-1);
    assert(bt_sco_send(&f.q,1,sample,2,100000,&serial)==1);assert(audio_poll(&f)==1);
    f.g.rf.d.lower.f.room=2;++f.g.rf.d.lower.f.now;assert(bt_sco_step(&f.q,64,100000)==1);
    assert(f.q.tx[0].state==BT_SCO_PARTIAL && f.g.rf.d.lower.f.hci.transport.tx_offset==2);
    const uint8_t disconnect[]={0,0,0,0x13};event(&f.g.rf.d.lower.f,5,disconnect,4);
    f.g.rf.d.lower.f.room=0;assert(bt_sco_step(&f.q,64,100000)==-1);
    assert(f.q.credits==2 && f.q.tx[0].state==BT_SCO_PARTIAL && f.q.acl->state==BT_ACL_FAULT);
    opened(&f);unsigned accesses=f.g.rf.d.lower.f.accesses;f.authority=0;
    assert(audio_poll(&f)==-1 && f.g.rf.d.lower.f.accesses==accesses && f.q.error==BT_SCO_IDENTITY);
    opened(&f);
    const uint8_t duplicate[]={0,0,0,1,2,3,4,5,6,2,6,2,30,0,30,0,2};
    event(&f.g.rf.d.lower.f,0x2c,duplicate,sizeof(duplicate));assert(audio_poll(&f)==-1 && f.q.error==BT_SCO_IDENTITY);
    audio_start(&f,0,0);command(&f,0x041b);status(&f,0x041b,0);f.g.rf.d.lower.f.now=f.q.links[0].deadline;
    assert(audio_poll(&f)==-1 && f.q.links[0].phase==BT_SCO_RETAINED);
}
static unsigned owned_events(const struct fixture *h)
{unsigned n=0;for(unsigned i=0;i<h->hci.transport.count;++i)n+=h->hci.transport.records[i].owner==BT_H4_OWNED;return n;}
/* A request that no link awaits is refused once, without a link identity;
 * a second request while that refusal is outstanding is only released. */
static void stray_refusal(void)
{
    struct audio_fixture f;opened(&f);struct fixture *h=&f.g.rf.d.lower.f;
    const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,2};event(h,4,request,10);assert(audio_poll(&f)==1);
    size_t at=command(&f,0x042a);const uint8_t reject[]={1,0x2a,4,7,1,2,3,4,5,6,0x0d};
    assert(!memcmp(h->tx+at,reject,sizeof(reject)) && f.q.refuse==2);
    status(&f,0x042a,0);assert(f.q.refuse==3);size_t before=h->tn;
    event(h,4,request,10);assert(audio_poll(&f)==1 && audio_poll(&f)==1 && h->tn==before && f.q.refuse==3);
    complete(&f,0x0d,0,0);assert(audio_poll(&f)==1);
    assert(!f.q.refuse && f.q.links[0].phase==BT_SCO_OPEN && f.q.next_link==2 && !owned_events(h));
    /* The later unanswered request ends with a failed completion no owner claims. */
    complete(&f,0x10,0,0);assert(audio_poll(&f)==1 && f.q.links[0].phase==BT_SCO_OPEN && !owned_events(h));
    /* A refused reject means the controller already ended the request. */
    event(h,4,request,10);assert(audio_poll(&f)==1);command(&f,0x042a);status(&f,0x042a,0x02);
    assert(!f.q.refuse && f.q.links[0].phase==BT_SCO_OPEN && !owned_events(h));
    puts("Unexpected synchronous requests are refused or released without a link identity");
}
/* An accepted refusal whose failed completion never arrives ends at its
 * response bound; a later failed completion is not taken as the refusal's
 * and the next stray request is refused again. */
static void stray_refusal_expiry(void)
{
    struct audio_fixture f;opened(&f);struct fixture *h=&f.g.rf.d.lower.f;
    const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,2};event(h,4,request,10);assert(audio_poll(&f)==1);
    command(&f,0x042a);status(&f,0x042a,0);assert(f.q.refuse==3 && f.q.refuse_deadline>h->now);
    h->now=f.q.refuse_deadline-2;assert(audio_poll(&f)==1 && f.q.refuse==3);
    assert(audio_poll(&f)==1 && !f.q.refuse);
    complete(&f,0x0d,0,0);assert(!f.q.refuse && f.q.links[0].phase==BT_SCO_OPEN && !owned_events(h));
    event(h,4,request,10);assert(audio_poll(&f)==1);
    size_t at=command(&f,0x042a);const uint8_t reject[]={1,0x2a,4,7,1,2,3,4,5,6,0x0d};
    assert(!memcmp(h->tx+at,reject,sizeof(reject)) && f.q.refuse==2);
    status(&f,0x042a,0);assert(f.q.refuse==3);complete(&f,0x0d,0,0);
    assert(!f.q.refuse && f.q.links[0].phase==BT_SCO_OPEN && !owned_events(h));
    puts("An unanswered synchronous refusal expires at its response bound");
}
static void acknowledgement_backlog(void)
{
    struct audio_fixture f;audio_start(&f,0,0);command(&f,0x041b);
    struct fixture *h=&f.g.rf.d.lower.f;
    /* A serialized lower advancer classifies HCI before the link owner sees
     * the earlier retained event. No audio TX is pending in this setup. */
    event(h,0x22,0,0);cs(h,0x041b,1,0);
    step(h,64);step(h,64);assert(h->hci.command==BT_HCI_IDLE);
    struct bt_acl_peer peer={{9,2,3,4,5,6},1,0,0};uint64_t generation;
    assert(bt_acl_connect(&f.g.rf.d.lower.a,&peer,100000,&generation)==1);
    assert(audio_poll(&f)==1 && f.q.links[0].phase==BT_SCO_FEATURE_STATUS);
    assert(h->hci.command==BT_HCI_IDLE && f.g.rf.d.lower.a.links[generation-1].state==BT_ACL_CONNECT_QUEUED);
    assert(audio_poll(&f)==1 && f.q.links[0].phase==BT_SCO_FEATURE_WAIT);
    assert(h->hci.opcode==HCI_OP_CREATE_CONN && h->hci.command==BT_HCI_SENDING);
}
static void closed_held_receive(void)
{
    struct audio_fixture f;opened(&f);
    for(unsigned i=0;i<9;++i){uint8_t x=(uint8_t)i;sco_input(&f,0,0,&x,1);assert(audio_poll(&f)==1);}
    assert(bt_sco_close(&f.q,1,100000)==1);command(&f,0x0406);status(&f,0x0406,0);
    const uint8_t done[]={0,0,0,0x13};event(&f.g.rf.d.lower.f,5,done,4);assert(audio_poll(&f)==1);
    assert(f.q.links[0].phase==BT_SCO_CLOSED && f.q.links[0].closed_after);
    for(unsigned i=0;i<9;++i){const struct bt_sco_packet *r=bt_sco_receive(&f.q);
        assert(r && r->bytes[0]==i && r->acquired<f.q.links[0].closed_after);
        assert(bt_sco_release_receive(&f.q,r->serial)==1);assert(audio_poll(&f)==1);}
    assert(!bt_sco_receive(&f.q));uint8_t late=9;sco_input(&f,0,0,&late,1);
    assert(audio_poll(&f)==-1 && f.q.error==BT_SCO_PROTOCOL);
    puts("Known audio RX retained before disconnect drains after confirmed close; newly acquired late RX remains refused.");
}
static void close_local(struct audio_fixture *f,unsigned status_code)
{
    assert(bt_sco_close(&f->q,f->link,100000)==1);command(f,0x0406);status(f,0x0406,0);
    uint8_t done[]={status_code,0,0,0x13};event(&f->g.rf.d.lower.f,5,done,4);assert(audio_poll(f)==1);
}
static void reopen_refusal(struct audio_fixture *f,int expected)
{
    struct bt_sco_link old=f->q.links[0];uint64_t id=0xfeed;
    assert(bt_sco_reopen(&f->q,f->link,100000,&id)==expected&&id==0xfeed);
    assert(!memcmp(&old,&f->q.links[0],sizeof(old)));
}
static void reopen_cases(void)
{
    struct audio_fixture f;opened(&f);reopen_refusal(&f,-1);
    /* The close receipt follows actual local status/completion, and old copied
     * RX plus one classified held H4 record and FLUSHED TX still block rearm. */
    for(unsigned i=0;i<9;++i){uint8_t x=(uint8_t)i;sco_input(&f,0,0,&x,1);assert(audio_poll(&f)==1);}
    uint64_t sent=transmit(&f);close_local(&f,0);
    assert(f.q.links[0].local_closed&&f.q.links[0].closed_after>f.q.links[0].acknowledgement);
    reopen_refusal(&f,0);
    for(unsigned i=0;i<9;++i){const struct bt_sco_packet *r=bt_sco_receive(&f.q);
        assert(r&&r->link==f.link&&r->bytes[0]==i);
        assert(bt_sco_release_receive(&f.q,r->serial)==1&&audio_poll(&f)==1);}
    reopen_refusal(&f,0);assert(bt_sco_release_result(&f.q,sent)==1);
    struct bt_h4 *h=&f.g.rf.d.lower.f.hci.transport;
    unsigned assembling=h->assembling;h->assembling=0;reopen_refusal(&f,0);h->assembling=assembling;
    size_t staged=h->staged,offset=h->stage_offset;h->staged=1;h->stage_offset=0;
    reopen_refusal(&f,0);h->staged=staged;h->stage_offset=offset;
    h->records[0].owner=BT_H4_OWNED;reopen_refusal(&f,0);h->records[0].owner=BT_H4_FREE;
    size_t length=h->tx_length,txoffset=h->tx_offset;h->tx_length=1;h->tx_offset=0;
    reopen_refusal(&f,0);h->tx_length=length;h->tx_offset=txoffset;
    f.q.command_link=0;reopen_refusal(&f,0);f.q.command_link=BT_SCO_LINKS;
    security_generation=32;reopen_refusal(&f,-1);security_generation=31;
    f.secure=1;reopen_refusal(&f,-1);f.secure=0;
    ++f.g.ag.link;reopen_refusal(&f,-1);--f.g.ag.link;
    ++f.g.rf.d.lower.a.links[0].generation;reopen_refusal(&f,-1);--f.g.rf.d.lower.a.links[0].generation;
    unsigned ready=f.g.rf.d.lower.f.hci.synchronous_ready;f.g.rf.d.lower.f.hci.synchronous_ready=0;
    reopen_refusal(&f,-1);f.g.rf.d.lower.f.hci.synchronous_ready=ready;
    uint64_t next=f.q.next_link;f.q.next_link=UINT64_MAX;reopen_refusal(&f,-1);
    f.q.next_link=0;reopen_refusal(&f,-1);f.q.next_link=next;
    /* More than four reopenings reuse the exact slot, including controller
     * handle zero, and invalidate old caller generations. */
    for(unsigned i=0;i<5;++i){
        uint64_t prior=f.link,new_id=0;
        assert(bt_sco_reopen(&f.q,prior,100000,&new_id)==1&&new_id==prior+1);
        assert(f.q.links[0].generation==new_id&&!f.q.links[0].handle_valid&&!f.q.links[0].local_closed);
        for(unsigned j=1;j<BT_SCO_LINKS;++j)assert(f.q.links[j].phase==BT_SCO_UNUSED);
        assert(bt_sco_close(&f.q,prior,100000)==-1);
        f.link=new_id;remote(&f,1);enhanced(&f,0,1);complete(&f,0,0,2);close_local(&f,0);
    }
    /* Ordinary open still quarantines the peer. Remote close and a failed
     * local close do not create a reopen receipt. */
    uint64_t id=0xfeed;assert(bt_sco_open(&f.q,&f.g.ag,0,100000,&id)==-1&&id==0xfeed);
    opened(&f);const uint8_t done[]={0,0,0,0x13};event(&f.g.rf.d.lower.f,5,done,4);assert(audio_poll(&f)==1);
    assert(!f.q.links[0].local_closed);reopen_refusal(&f,-1);
    opened(&f);close_local(&f,0x0c);assert(!f.q.links[0].local_closed);reopen_refusal(&f,-1);
    /* A newly acquired late packet is rejected under the closed generation. */
    opened(&f);close_local(&f,0);uint8_t late=9;sco_input(&f,0,0,&late,1);
    assert(audio_poll(&f)==-1&&f.q.error==BT_SCO_PROTOCOL);reopen_refusal(&f,-1);
    puts("PASS local-close receipt, copied/H4 drain, unchanged security, generation refusal and five same-slot/handle reopenings");
}
int main(int argc,char **argv)
{(void)config_ready;if(argc==2&&!strcmp(argv[1],"--reopen")){reopen_cases();return 0;}if(argc==2 && !strcmp(argv[1],"--close-held")){closed_held_receive();return 0;}if(argc!=2 || strcmp(argv[1],"--remaining"))acknowledgement_backlog();credits_and_audio();mixed_refusal();incoming_and_fallback();refusals_and_partial();stray_refusal();stray_refusal_expiry();puts("New synchronous audio composition passed; prior Bluetooth scenario suites were not executed.");return 0;}
