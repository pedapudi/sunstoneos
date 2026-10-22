/* SNTP reply validation, RTC conversion and the client sequence against
 * scripted copied socket replies. The actual stack exchange is covered by the
 * sntp case of checks/radio-network-dhcp-dns.c. */
#include "sunstone_net.h"
#include "a733_rtc_service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct sunstone_net net;
static struct sunstone_net_request submitted[32];
static struct sunstone_net_reply script[32];
static unsigned submits,scripted,delivered,refuse_submit,random_calls,random_fail;
static uint64_t next_ticket=100;
int sunstone_net_submit(struct sunstone_net *s,const struct sunstone_net_request *r,uint64_t *ticket)
{
    assert(s==&net&&submits<32&&r->timeout==50);
    if(refuse_submit){--refuse_submit;return 0;}
    submitted[submits++]=*r;*ticket=++next_ticket;return 1;
}
int sunstone_net_receive(struct sunstone_net *s,struct sunstone_net_reply *r,TickType_t wait)
{
    assert(s==&net&&!wait);if(delivered==scripted)return 0;
    *r=script[delivered++];r->ticket=next_ticket;return 1;
}
static int fixture_random(void *context,uint8_t *bytes,size_t count)
{
    assert(context==&net&&count==8);++random_calls;if(random_fail)return 0;
    for(size_t i=0;i<count;++i)bytes[i]=(uint8_t)(16*random_calls+i);return 1;
}
static void put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
/* A server reply to nonce: received at seconds+.25, sent at seconds+.5. */
static void server(uint8_t p[48],const uint8_t nonce[8],uint32_t seconds)
{
    memset(p,0,48);p[0]=0x24;p[1]=2;memcpy(p+24,nonce,8);
    put32(p+32,seconds);put32(p+36,0x40000000u);put32(p+40,seconds);put32(p+44,0x80000000u);
}
static const uint8_t time_ip[4]={10,0,0,7};
static struct sunstone_net_reply *expect(enum sunstone_net_result result)
{struct sunstone_net_reply *r=&script[scripted++];memset(r,0,sizeof(*r));r->result=result;return r;}
static struct sunstone_net_reply *datagram(const uint8_t nonce[8],uint16_t port,const uint8_t *address)
{
    struct sunstone_net_reply *r=expect(SUNSTONE_NET_OK);r->bytes=48;r->port=port;memcpy(r->address,address,4);
    server(r->payload,nonce,0xee832ff0u);return r;
}
static struct sunstone_sntp_config config(void)
{
    return (struct sunstone_sntp_config){.random=fixture_random,.random_context=&net,.deadline_us=10000000,
        .max_round_trip_us=1000000,.timeout=50,.attempts=3,.name_bytes=9,.name="time.test"};
}
static void reset(void){submits=scripted=delivered=refuse_submit=random_calls=random_fail=0;}
/* Poll until the client submits or finishes; returns the poll result. */
static int run(struct sunstone_sntp_client *c,uint64_t now)
{
    int result=0;unsigned before=submits;
    for(unsigned i=0;i<8&&!result&&submits==before;++i)result=sunstone_sntp_poll(c,now);
    return result;
}
static void replies(void)
{
    uint8_t nonce[8]={1,2,3,4,5,6,7,8},p[48];struct sunstone_sntp_time t;
    server(p,nonce,0xee832ff0u);
    /* 2026-10-21 12:34:56.5 UTC; 30 ms local round trip less 0.25 s processing is refused. */
    assert(sunstone_sntp_reply(p,48,nonce,1000000,1030000,1000000,&t)==-1);
    put32(p+36,0x7f7ced92u); /* received at .498 s: 2 ms processing, 28 ms round trip */
    assert(sunstone_sntp_reply(p,48,nonce,1000000,1030000,1000000,&t)==1);
    assert(t.local_us==1030000&&t.round_trip_us==28000&&t.unix_us==UINT64_C(1792586096500000)+14000&&t.stratum==2&&!t.leap);
    assert(sunstone_sntp_reply(p,48,nonce,1000000,1030000,27999,&t)==-1);
    assert(sunstone_sntp_reply(p,47,nonce,1000000,1030000,1000000,&t)==0);
    uint8_t other[8]={1,2,3,4,5,6,7,9};assert(sunstone_sntp_reply(p,48,other,1000000,1030000,1000000,&t)==0);
    assert(sunstone_sntp_reply(p,48,nonce,1030001,1030000,1000000,&t)==-1);
    /* Era 1: 0x0754fd00 is 2040-01-01 00:00:00 UTC. */
    server(p,nonce,0x0754fd00u);put32(p+36,0x80000000u);
    assert(sunstone_sntp_reply(p,68,nonce,0,10,1000,&t)==1&&t.unix_us==UINT64_C(2208988800)*1000000u+500000u+5);
    struct rtc_request rtc;assert(sunstone_sntp_rtc_request(&t,10,&rtc)&&rtc.day==25568&&!rtc.hms&&rtc.kind==RTC_REQUEST_SET);
    const struct { unsigned at; uint8_t value; } refused[]={{0,0xe4},{0,0x14},{0,0x23},{0,0x25},{1,0},{1,16},{32,0},{40,0x80}};
    for(unsigned i=0;i<sizeof(refused)/sizeof(refused[0]);++i){
        server(p,nonce,0xee832ff0u);put32(p+36,0x80000000u);
        if(refused[i].at==32)memset(p+32,0,8);       /* unknown receive time */
        else if(refused[i].at==40)put32(p+40,0x80000000u); /* before 1970 */
        else p[refused[i].at]=refused[i].value;      /* leap 3, version 2, client mode, broadcast, KoD, stratum 16 */
        assert(sunstone_sntp_reply(p,48,nonce,0,10,1000,&t)==-1);
    }
    server(p,nonce,0xee832ff0u);put32(p+44,0x20000000u); /* sent before received */
    assert(sunstone_sntp_reply(p,48,nonce,0,1000000,1000000,&t)==-1);
    t=(struct sunstone_sntp_time){.unix_us=UINT64_C(1792586096500000),.local_us=50};
    assert(sunstone_sntp_rtc_request(&t,50+3600000000u,&rtc)&&rtc.day==20748&&rtc.hms==(13u<<16|34u<<8|56u));
    assert(!sunstone_sntp_rtc_request(&t,49,&rtc));
    t.unix_us=UINT64_C(65535)*86400u*1000000u;assert(!sunstone_sntp_rtc_request(&t,50,&rtc));
    t.unix_us-=1;assert(sunstone_sntp_rtc_request(&t,50,&rtc)&&rtc.day==65535&&rtc.hms==(23u<<16|59u<<8|59u));
}
static void nominal(void)
{
    struct sunstone_sntp_client c;struct sunstone_sntp_config k=config();reset();
    k.attempts=0;assert(!sunstone_sntp_start(&c,&net,&k));k=config();
    memcpy(k.server,time_ip,4);assert(!sunstone_sntp_start(&c,&net,&k));k.name_bytes=0;assert(sunstone_sntp_start(&c,&net,&k));
    k=config();k.timeout=0;assert(!sunstone_sntp_start(&c,&net,&k));k=config();
    assert(sunstone_sntp_start(&c,&net,&k));
    refuse_submit=1;assert(!sunstone_sntp_poll(&c,100)&&!submits); /* Queue busy or link lost. */
    assert(!run(&c,100)&&submitted[0].operation==SUNSTONE_NET_RESOLVE&&submitted[0].bytes==9&&!memcmp(submitted[0].payload,"time.test",9));
    expect(SUNSTONE_NET_TIMEOUT);assert(!run(&c,200)&&submitted[1].operation==SUNSTONE_NET_RESOLVE);
    expect(SUNSTONE_NET_BUSY);assert(!run(&c,300)&&submitted[2].operation==SUNSTONE_NET_RESOLVE);
    memcpy(expect(SUNSTONE_NET_OK)->address,time_ip,4);assert(!run(&c,400)&&submitted[3].operation==SUNSTONE_NET_OPEN_UDP);
    expect(SUNSTONE_NET_OK)->handle=9;assert(!run(&c,500)&&submitted[4].operation==SUNSTONE_NET_SEND);
    const struct sunstone_net_request *send=&submitted[4];
    assert(send->handle==9&&send->port==123&&send->bytes==48&&!memcmp(send->address,time_ip,4)&&send->payload[0]==0x23);
    uint8_t first[8];memcpy(first,send->payload+40,8);assert(first[0]==16&&random_calls==1);
    expect(SUNSTONE_NET_OK)->stack_result=48;assert(!run(&c,600)&&submitted[5].operation==SUNSTONE_NET_RECEIVE&&submitted[5].handle==9);
    /* Another port, another address and another nonce are ignored. */
    datagram(first,124,time_ip);assert(!run(&c,700)&&submitted[6].operation==SUNSTONE_NET_RECEIVE);
    datagram(first,123,(const uint8_t[4]){10,0,0,1});assert(!run(&c,800)&&submitted[7].operation==SUNSTONE_NET_RECEIVE);
    uint8_t stale[8]={9,9,9,9,9,9,9,9};datagram(stale,123,time_ip);assert(!run(&c,900)&&submitted[8].operation==SUNSTONE_NET_RECEIVE);
    /* Unanswered: resend with a fresh nonce; the first nonce is then stale. */
    expect(SUNSTONE_NET_TIMEOUT);assert(!run(&c,1000)&&submitted[9].operation==SUNSTONE_NET_SEND&&random_calls==2);
    uint8_t second[8];memcpy(second,submitted[9].payload+40,8);assert(memcmp(first,second,8));
    expect(SUNSTONE_NET_OK)->stack_result=48;assert(!run(&c,2000)&&submitted[10].operation==SUNSTONE_NET_RECEIVE);
    datagram(first,123,time_ip);assert(!run(&c,2100)&&submitted[11].operation==SUNSTONE_NET_RECEIVE);
    struct sunstone_net_reply *late=datagram(second,123,time_ip);late->result=SUNSTONE_NET_TIMEOUT; /* Late, still valid. */
    put32(late->payload+36,0x80000000u);
    assert(!run(&c,32000)&&submitted[12].operation==SUNSTONE_NET_CLOSE&&submitted[12].handle==9);
    expect(SUNSTONE_NET_OK);assert(run(&c,33000)==1&&sunstone_sntp_poll(&c,40000)==1&&submits==13);
    assert(c.time.round_trip_us==31000&&c.time.unix_us==UINT64_C(1792586096500000)+15500&&c.time.local_us==32000);
}
/* Each failure closes the open socket before reporting -1. */
static int failed_after(enum sunstone_net_result open_result,int kod,int exhaust,int deadline,int close_ok)
{
    struct sunstone_sntp_client c;struct sunstone_sntp_config k=config();reset();
    memcpy(k.server,time_ip,4);k.name_bytes=0;k.attempts=deadline?2:1;assert(sunstone_sntp_start(&c,&net,&k));
    assert(!run(&c,100)&&submitted[0].operation==SUNSTONE_NET_OPEN_UDP);
    struct sunstone_net_reply *open=expect(open_result);open->handle=open_result==SUNSTONE_NET_BUSY?0:4;
    if(open_result!=SUNSTONE_NET_OK)goto closing;
    assert(!run(&c,200)&&submitted[1].operation==SUNSTONE_NET_SEND);
    expect(SUNSTONE_NET_OK)->stack_result=48;assert(!run(&c,300)&&submitted[2].operation==SUNSTONE_NET_RECEIVE);
    if(kod){datagram(submitted[1].payload+40,123,time_ip)->payload[1]=0;}
    else if(exhaust)expect(SUNSTONE_NET_TIMEOUT);
    else if(deadline){expect(SUNSTONE_NET_TIMEOUT);assert(!run(&c,k.deadline_us)&&submitted[3].operation==SUNSTONE_NET_CLOSE);goto closed;}
closing:
    if(open_result==SUNSTONE_NET_BUSY){assert(run(&c,400)==-1&&submits==1);return 1;}
    assert(!run(&c,400)&&submitted[submits-1].operation==SUNSTONE_NET_CLOSE&&submitted[submits-1].handle==4);
closed:
    expect(close_ok?SUNSTONE_NET_OK:SUNSTONE_NET_TIMEOUT);
    assert(run(&c,500)==-1&&sunstone_sntp_poll(&c,600)==-1);
    assert(close_ok?!c.handle:c.handle==4);return 1;
}
int main(void)
{
    replies();nominal();
    assert(failed_after(SUNSTONE_NET_TIMEOUT,0,0,0,1)); /* Timed-out OPEN still names its socket. */
    assert(failed_after(SUNSTONE_NET_BUSY,0,0,0,1));
    assert(failed_after(SUNSTONE_NET_OK,1,0,0,1));     /* Kiss-o'-death. */
    assert(failed_after(SUNSTONE_NET_OK,0,1,0,1));     /* Attempts exhausted. */
    assert(failed_after(SUNSTONE_NET_OK,0,0,1,1));     /* Deadline: no new SEND. */
    assert(failed_after(SUNSTONE_NET_OK,1,0,0,0));     /* A failed CLOSE retains the socket. */
    /* Entropy failure before the request closes the socket. */
    struct sunstone_sntp_client c;struct sunstone_sntp_config k=config();reset();
    memcpy(k.server,time_ip,4);k.name_bytes=0;assert(sunstone_sntp_start(&c,&net,&k));
    assert(!run(&c,100));expect(SUNSTONE_NET_OK)->handle=4;random_fail=1;
    assert(!run(&c,200)&&submitted[1].operation==SUNSTONE_NET_CLOSE);expect(SUNSTONE_NET_OK);assert(run(&c,300)==-1);
    puts("PASS SNTP: reply validation and eras, RTC requests, ignored datagrams, resend, late reply, retained close and failures");
    return 0;
}
