/* SNTP client (RFC 4330, section 5) for one registered socket client, and the
 * conversion that feeds its UTC time to the RTC service's SET request. */
#include "sunstone_net.h"
#include "a733_rtc_service.h"
#include <string.h>
enum { RESOLVE, OPEN, SEND, RECEIVE, CLOSE, DONE, FAILED };
/* Seconds from the NTP epoch (1900) to the Unix epoch (1970). */
#define NTP_UNIX_SECONDS UINT64_C(2208988800)
static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
/* RFC 4330 section 3: a clear top bit means era 1 (2036-2104). Times before
 * 1970 and the all-zero "unknown" timestamp are refused. */
static int unix_us(const uint8_t *p,uint64_t *out)
{
    uint64_t seconds=be32(p),fraction=be32(p+4);
    if(!seconds&&!fraction)return 0;
    if(!(seconds&0x80000000u))seconds+=UINT64_C(1)<<32;
    if(seconds<NTP_UNIX_SECONDS)return 0;
    *out=(seconds-NTP_UNIX_SECONDS)*1000000u+((fraction*1000000u)>>32);return 1;
}
void sunstone_sntp_request(uint8_t packet[SUNSTONE_SNTP_BYTES],const uint8_t nonce[8])
{
    memset(packet,0,SUNSTONE_SNTP_BYTES);packet[0]=4<<3|3; /* LI 0, version 4, client mode */
    memcpy(packet+40,nonce,8);
}
int sunstone_sntp_reply(const uint8_t *p,size_t bytes,const uint8_t nonce[8],uint64_t sent_us,
    uint64_t received_us,uint64_t max_round_trip_us,struct sunstone_sntp_time *out)
{
    if(!p||!nonce||!out||bytes<SUNSTONE_SNTP_BYTES||memcmp(p+24,nonce,8))return 0;
    unsigned leap=p[0]>>6,version=(p[0]>>3)&7,mode=p[0]&7,stratum=p[1];
    uint64_t receive,transmit;
    if(leap==3||version<3||version>4||mode!=4||!stratum||stratum>15||
       !unix_us(p+32,&receive)||!unix_us(p+40,&transmit)||transmit<receive||received_us<sent_us||
       transmit-receive>received_us-sent_us)return -1;
    uint64_t round_trip=(received_us-sent_us)-(transmit-receive);
    if(round_trip>max_round_trip_us)return -1;
    *out=(struct sunstone_sntp_time){.unix_us=transmit+round_trip/2,.local_us=received_us,
        .round_trip_us=round_trip,.stratum=(uint8_t)stratum,.leap=(uint8_t)leap};
    return 1;
}
int sunstone_sntp_rtc_request(const struct sunstone_sntp_time *t,uint64_t now_us,struct rtc_request *out)
{
    if(!t||!out||now_us<t->local_us||now_us-t->local_us>UINT64_MAX-t->unix_us)return 0;
    uint64_t seconds=(t->unix_us+(now_us-t->local_us))/1000000u,day=seconds/86400u+1,second=seconds%86400u;
    if(day>65535u)return 0;
    *out=(struct rtc_request){.kind=RTC_REQUEST_SET,.day=(uint32_t)day,
        .hms=(uint32_t)(second/3600u<<16|second/60u%60u<<8|second%60u)};
    return 1;
}
int sunstone_sntp_start(struct sunstone_sntp_client *c,struct sunstone_net *net,const struct sunstone_sntp_config *config)
{
    if(!c||!net||!config||!config->random||!config->timeout||config->timeout>(TickType_t)(portMAX_DELAY/2)||
       !config->attempts||config->attempts>SUNSTONE_SNTP_ATTEMPTS||!config->max_round_trip_us)return 0;
    unsigned server=config->server[0]|config->server[1]|config->server[2]|config->server[3];
    if(server?config->name_bytes!=0:(!config->name_bytes||config->name_bytes>=SUNSTONE_NET_NAME_BYTES))return 0;
    memset(c,0,sizeof(*c));c->net=net;c->config=*config;memcpy(c->server,config->server,4);
    c->stage=server?OPEN:RESOLVE;return 1;
}
/* Close an open socket before reporting failure; without one, fail now. */
static void abandon(struct sunstone_sntp_client *c)
{c->failed=1;c->stage=c->handle?CLOSE:FAILED;}
/* One copied reply for the current stage. */
static void collect(struct sunstone_sntp_client *c,uint64_t now)
{
    const struct sunstone_net_reply *r=&c->reply;
    if(r->ticket!=c->ticket){c->stage=FAILED;c->failed=1;return;}
    switch(c->stage){
    case RESOLVE:
        /* The pinned DNS sends one query; an ARP miss for the server consumes
         * it, so a timed-out lookup is repeated until the deadline. BUSY means
         * the IP task has not yet released the earlier lookup's slot. */
        if(r->result==SUNSTONE_NET_OK){memcpy(c->server,r->address,4);c->stage=OPEN;}
        else if(r->result!=SUNSTONE_NET_TIMEOUT&&r->result!=SUNSTONE_NET_BUSY)abandon(c);
        break;
    case OPEN:
        /* A timed-out OPEN still names its retained socket for CLOSE. */
        c->handle=r->handle;
        if(r->result==SUNSTONE_NET_OK&&r->handle)c->stage=SEND;else abandon(c);
        break;
    case SEND:
        if(r->result==SUNSTONE_NET_OK&&r->stack_result==SUNSTONE_SNTP_BYTES)c->stage=RECEIVE;else abandon(c);
        break;
    case RECEIVE:{
        /* A late receive still carries its datagram in the TIMEOUT reply. */
        if(r->bytes&&(r->result==SUNSTONE_NET_OK||r->result==SUNSTONE_NET_TIMEOUT)){
            int valid=0;
            if(!memcmp(r->address,c->server,4)&&r->port==SUNSTONE_SNTP_PORT)
                valid=sunstone_sntp_reply(r->payload,r->bytes,c->nonce,c->sent_us,now,c->config.max_round_trip_us,&c->time);
            if(valid>0){c->stage=CLOSE;break;}
            if(valid<0){abandon(c);break;}
            if(r->result==SUNSTONE_NET_OK)break; /* Another datagram: keep receiving. */
        }else if(r->result!=SUNSTONE_NET_TIMEOUT){abandon(c);break;}
        /* Unanswered: an ARP miss may have consumed the request. Send again
         * with a fresh nonce, so a reply to the earlier one is ignored. */
        if(c->sent<c->config.attempts)c->stage=SEND;else abandon(c);
        break;}
    case CLOSE:
        if(r->result==SUNSTONE_NET_OK){c->handle=0;c->stage=c->failed?FAILED:DONE;}
        else{c->failed=1;c->stage=FAILED;}
        break;
    default:break;
    }
}
int sunstone_sntp_poll(struct sunstone_sntp_client *c,uint64_t now)
{
    if(!c||!c->net)return -1;
    if(c->pending){
        if(!sunstone_net_receive(c->net,&c->reply,0))return 0;
        c->pending=0;collect(c,now);
    }
    if(c->stage<CLOSE&&now>=c->config.deadline_us)abandon(c);
    if(c->stage==DONE)return 1;
    if(c->stage==FAILED)return -1;
    struct sunstone_net_request *q=&c->request;
    memset(q,0,sizeof(*q));q->timeout=c->config.timeout;
    switch(c->stage){
    case RESOLVE:q->operation=SUNSTONE_NET_RESOLVE;q->bytes=c->config.name_bytes;memcpy(q->payload,c->config.name,q->bytes);break;
    case OPEN:q->operation=SUNSTONE_NET_OPEN_UDP;break;
    case SEND:
        if(c->config.random(c->config.random_context,c->nonce,sizeof(c->nonce))!=1){abandon(c);return 0;}
        q->operation=SUNSTONE_NET_SEND;q->handle=c->handle;q->port=SUNSTONE_SNTP_PORT;q->bytes=SUNSTONE_SNTP_BYTES;
        memcpy(q->address,c->server,4);sunstone_sntp_request(q->payload,c->nonce);break;
    case RECEIVE:q->operation=SUNSTONE_NET_RECEIVE;q->handle=c->handle;q->bytes=SUNSTONE_NET_BYTES;break;
    default:q->operation=SUNSTONE_NET_CLOSE;q->handle=c->handle;break;
    }
    /* Submission may be refused while the queue is busy or the link is lost;
     * the next poll submits again. The SEND time is taken at submission. */
    if(sunstone_net_submit(c->net,q,&c->ticket)){
        c->pending=1;
        if(c->stage==SEND){c->sent_us=now;++c->sent;}
    }
    return 0;
}
