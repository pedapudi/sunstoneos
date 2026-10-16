#define AIC8800_TRANSPORT_FIXTURE_MAIN runtime_transport_fixture_unused_main
#include "aic8800-sdio-transport.c"
#undef AIC8800_TRANSPORT_FIXTURE_MAIN
#include "aic8800_firmware.h"
#include "aic8800_runtime.h"
#include <stdlib.h>

static struct aic8800_runtime runtime;
static struct aic8800_firmware firmware;
static struct aic8800_sdio_transport base_transport;
static struct sunxi_sdio base_host;
static struct sdio_card base_card;
static struct model base_model;
static uint8_t challenge_bytes[16], reply_bytes[16];
static unsigned platform_calls, binding_transmissions;
static int platform_return=1;
static int platform_failure, callback_late, binding_reply, binding_status, partial_write;
static unsigned message(uint8_t *p,uint16_t id,const uint8_t *params,unsigned n)
{
    p[0]=(uint8_t)(12+n);p[2]=0x11;p[4]=(uint8_t)id;p[5]=(uint8_t)(id>>8);p[10]=(uint8_t)n;
    if(n)memcpy(p+16,params,n);
    return (16+n+3)&~3u;
}
static int platform_id(void *ctx,uint8_t id[16])
{
    assert(ctx==&m);++platform_calls;
    for(unsigned i=0;i<16;++i)id[i]=(uint8_t)(i+32);
    if(callback_late)m.now=runtime.binding_deadline;
    return platform_failure?0:platform_return;
}
static int runtime_write(void *ctx,uint32_t off,uint32_t value)
{
    if(partial_write&&off==0x200&&m.write&&m.position==4){++m.accesses;return 0;}
    int result=write_cb(ctx,off,value);
    if(off==0x200&&m.write&&m.position==m.bytes) {
        assert(m.bytes==512&&!memcmp(m.transmitted,runtime.request,512));
        assert(m.transmitted[8]==0x15&&m.transmitted[9]==4&&m.transmitted[14]==16);
        assert(!memcmp(m.transmitted+16,reply_bytes,16));
        for(unsigned i=32;i<512;++i)assert(!m.transmitted[i]);
        ++binding_transmissions;memset(receive,0,sizeof(receive));
        if(binding_reply) {
            /* Unknown control record before the CFM must survive backpressure. */
            receive[0]=3;receive[2]=0x13;receive[4]=7;receive[5]=8;receive[6]=9;
            uint8_t status[2]={(uint8_t)binding_status,0};message(receive+8,0x0416,status,binding_reply==2?2:1);
            m.card[1][4]=1;
        }
    }
    return result;
}
static void fresh(int early)
{
    /* Restore a simulated completed bootstrap boundary. Firmware execution is
     * supplied as a checkpoint assumption; the previous upload model is reused. */
    m=base_model;host=base_host;card=base_card;transport=base_transport;
    memset(&firmware,0,sizeof(firmware));memset(&runtime,0,sizeof(runtime));
    memset(receive,0,sizeof(receive));m.card[1][4]=0;m.card[1][3]=1;
    firmware.initialized=1;firmware.state=AIC8800_FW_START_CONFIRMED;firmware.owner=&transport;
    firmware.binding_required=(uint8_t)early;memcpy(firmware.binding_challenge,challenge_bytes,16);
    transport.firmware=&firmware;transport.state=AIC8800_TRANSPORT_START_CONFIRMED;
    platform_calls=binding_transmissions=0;platform_failure=callback_late=partial_write=0;
    binding_reply=1;binding_status=0;
    host.config.ops.write32=runtime_write;
    struct aic8800_runtime_config c={.read_platform_id=platform_id,.context=&m,.binding_timeout=2000};
    assert(aic8800_runtime_init(&runtime,&c));assert(!aic8800_runtime_init(&runtime,&c));
    assert(aic8800_transport_start_runtime(&transport,&runtime,m.now+10000));
    assert(!aic8800_transport_start_runtime(&transport,&runtime,m.now+10000));
}
static void until_held(void)
{
    for(unsigned i=0;i<1000&&!transport.held&&transport.state!=AIC8800_TRANSPORT_FAULT;++i)step();
    assert(transport.held);
}
static void until_confirmed(void)
{
    for(unsigned i=0;i<1000&&!runtime.binding_confirmed&&transport.state!=AIC8800_TRANSPORT_FAULT;++i) {
        step();
        if(transport.held) {
            struct aic8800_sdio_response e;assert(aic8800_transport_peek(&transport,&e));
            assert(e.transport_type==0x13);assert(aic8800_transport_consume(&transport));
        }
    }
    assert(runtime.binding_confirmed&&runtime.binding_status==0&&platform_calls==1&&binding_transmissions==1);
}
static void idle(void)
{
    for(unsigned i=0;i<1000&&(transport.pending||transport.cursor!=transport.rx_bytes);++i)step();
    assert(!transport.pending&&transport.cursor==transport.rx_bytes);
}
static void retained_fault(void)
{
    assert(transport.state==AIC8800_TRANSPORT_FAULT&&runtime.state==AIC8800_RUNTIME_FAULT);
    unsigned accesses=m.accesses;step();assert(m.accesses==accesses);
    assert(!aic8800_transport_renew_runtime(&transport,m.now+10000));
    assert(!aic8800_transport_consume(&transport));
}
static void algorithm_vectors(const char *path)
{
    FILE *f=fopen(path,"rb");assert(f);uint8_t row[32];unsigned rows=0;
    while(fread(row,1,32,f)==32) {
        struct aic8800_runtime r={0};struct aic8800_runtime_config c={.read_platform_id=platform_id,.context=&m,.binding_timeout=100};
        assert(aic8800_runtime_init(&r,&c));assert(aic8800_runtime_claim(&r,&r,1,row));
        for(unsigned i=0;i<5;++i)aic8800_runtime_step(&r,&r,2+i);
        assert(r.state==AIC8800_RUNTIME_REQUEST&&!memcmp(r.reply,row+16,16));
        if(!rows){memcpy(challenge_bytes,row,16);memcpy(reply_bytes,row+16,16);}++rows;
    }
    assert(feof(f)&&rows==8);assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"--invalid-platform")) {
        const int invalid[]={-1,2};
        for(unsigned i=0;i<2;++i) {
            struct aic8800_runtime r={0};
            struct aic8800_runtime_config c={.read_platform_id=platform_id,.context=&m,.binding_timeout=100};
            assert(aic8800_runtime_init(&r,&c));assert(aic8800_runtime_claim(&r,&r,1,challenge_bytes));
            platform_return=invalid[i];aic8800_runtime_step(&r,&r,2);
            assert(r.state==AIC8800_RUNTIME_FAULT&&r.error==AIC8800_RUNTIME_PLATFORM_ERROR&&r.stage==0);
        }
        puts("AIC runtime: invalid platform callback returns -1 and 2 retain failure");return 0;
    }
    algorithm_vectors(argv[1]);
    begin(AIC8800_PAD_KEEP);setup();identity(2000);
    while(transport.state==AIC8800_TRANSPORT_IDENTITY)step();
    assert(transport.state==AIC8800_TRANSPORT_COMPLETE);
    base_transport=transport;base_host=host;base_card=card;base_model=m;

    fresh(0);
    /* Opaque data uses length+60, not the control length+4 framing. */
    receive[0]=17;receive[2]=0x80;receive[60]=0xa5;
    message(receive+80,0x0417,challenge_bytes,16);
    uint8_t stale_status=0;message(receive+112,0x0416,&stale_status,1);m.card[1][4]=1;
    until_held();struct aic8800_sdio_response e;assert(aic8800_transport_peek(&transport,&e));
    assert(!e.has_message&&e.record_bytes==77&&e.consumed==80&&e.record[60]==0xa5);
    unsigned before=m.command_count;step();assert(m.command_count==before&&!runtime.binding_seen);
    assert(!aic8800_transport_renew_runtime(&transport,m.now+20000));
    assert(!aic8800_transport_peek(&transport,(void *)runtime.request));
    assert(aic8800_transport_consume(&transport));until_held();
    assert(aic8800_transport_peek(&transport,&e)&&e.message_id==0x0416);
    assert(!runtime.binding_confirmed&&!binding_transmissions);
    assert(aic8800_transport_consume(&transport));until_confirmed();idle();
    assert(aic8800_transport_renew_runtime(&transport,transport.deadline+1000));
    /* Continue receiving after binding, retaining TX confirmation and duplicate. */
    memset(receive,0,sizeof(receive));receive[0]=4;receive[2]=0x12;put32(receive+4,0x12345678);
    message(receive+8,0x0417,challenge_bytes,16);m.card[1][4]=1;
    until_held();assert(aic8800_transport_peek(&transport,&e)&&e.transport_type==0x12);
    assert(aic8800_transport_consume(&transport));until_held();
    assert(aic8800_transport_peek(&transport,&e)&&e.message_id==0x0417);
    assert(platform_calls==1&&binding_transmissions==1);
    m.now=transport.deadline;step();assert(aic8800_transport_peek(&transport,&e));retained_fault();

    fresh(1);until_confirmed();idle();
    /* Renewal observes the current clock, refusing a stale idle lease. */
    m.now=transport.deadline;assert(!aic8800_transport_renew_runtime(&transport,m.now+1000));retained_fault();

    fresh(1);platform_failure=1;
    step();assert(runtime.error==AIC8800_RUNTIME_PLATFORM_ERROR&&!binding_transmissions);retained_fault();
    fresh(1);callback_late=1;
    step();assert(runtime.error==AIC8800_RUNTIME_TIME_ERROR&&!binding_transmissions);retained_fault();
    fresh(1);runtime.challenge[0]^=1;
    for(unsigned i=0;i<10&&transport.state!=AIC8800_TRANSPORT_FAULT;++i)step();
    assert(runtime.error==AIC8800_RUNTIME_CHALLENGE_ERROR&&!binding_transmissions);retained_fault();
    fresh(1);binding_status=7;
    for(unsigned i=0;i<1000&&transport.state!=AIC8800_TRANSPORT_FAULT;++i) {
        step();if(transport.held)assert(aic8800_transport_consume(&transport));
    }
    assert(runtime.binding_status==7&&runtime.error==AIC8800_RUNTIME_BINDING_REJECTED);retained_fault();
    fresh(1);binding_reply=2;
    for(unsigned i=0;i<1000&&transport.state!=AIC8800_TRANSPORT_FAULT;++i) {
        step();if(transport.held)assert(aic8800_transport_consume(&transport));
    }
    assert(transport.error==AIC8800_TRANSPORT_MESSAGE_ERROR&&!runtime.binding_confirmed);retained_fault();
    fresh(1);binding_reply=0;
    for(unsigned i=0;i<500&&runtime.state!=AIC8800_RUNTIME_WAITING;++i)step();
    assert(binding_transmissions==1);m.now=runtime.binding_deadline;step();
    assert(runtime.error==AIC8800_RUNTIME_TIME_ERROR&&runtime.request[8]==0x15);retained_fault();
    fresh(1);partial_write=1;
    for(unsigned i=0;i<500&&transport.state!=AIC8800_TRANSPORT_FAULT;++i)step();
    assert(m.position==4&&host.request.data==transport.tx&&!binding_transmissions);retained_fault();
    fresh(0);memset(receive,0,sizeof(receive));message(receive,0x0417,challenge_bytes,15);m.card[1][4]=1;
    for(unsigned i=0;i<500&&transport.state!=AIC8800_TRANSPORT_FAULT;++i)step();
    assert(transport.error==AIC8800_TRANSPORT_MESSAGE_ERROR&&transport.cursor==0);retained_fault();
    fresh(0);receive[0]=255;receive[1]=15;receive[2]=0;m.card[1][4]=1;
    for(unsigned i=0;i<500&&transport.state!=AIC8800_TRANSPORT_FAULT;++i)step();
    assert(transport.codec_result==AIC8800_CODEC_TRUNCATED&&transport.cursor==0);retained_fault();

    /* Firmware handoff retains the first challenge; subsequent events remain raw. */
    struct aic8800_firmware f={.initialized=1,.state=AIC8800_FW_START_CONFIRMED,.deadline=100};f.owner=&f;
    uint8_t record[32]={0};message(record,0x0417,challenge_bytes,16);
    assert(aic8800_firmware_response(&f,&f,record,32,1,&e)==AIC8800_CODEC_UNRELATED);
    record[16]^=1;
    assert(aic8800_firmware_response(&f,&f,record,32,2,&e)==AIC8800_CODEC_UNRELATED);
    assert(!memcmp(f.binding_challenge,challenge_bytes,16));

    puts("AIC runtime: binding vectors, continuous raw receive, backpressure, lease and retained faults passed");
    return 0;
}
