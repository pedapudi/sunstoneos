#include "playback.h"
#include "bluetooth.h"
#include "bt_sco.h"
#include "msbc.h"
#include "a733_storage_digest.h"
#include <string.h>
#include "bt_memory.h"
/* state0: USB producer owns bytes;1: Bluetooth owns immutable PCM;2: USB owns
 * immutable result. Release/acquire transfers ownership; neither task writes
 * fields belonging to the other state. Provider associations are owner-only. */
static struct {
    struct bluetooth_application *app;
    TaskHandle_t producer;
    struct {atomic_uint state;uint8_t pcm[BLUETOOTH_PLAYBACK_BYTES];struct bluetooth_playback_result result;} slots[2];
    atomic_ullong accepted,acknowledged,silence,rx_packets,rx_bytes,rx_loss;
    atomic_ullong silence_uart,silence_released,silence_flushed,silence_not_sent;
    atomic_uint sealed,cancelled,closed;
    uint64_t next_owner,next_frame;
    unsigned upload_bytes,pcm_offset,frame_offset,ready,started;
    uint8_t frame[60];
    struct {uint64_t serial,ticket;unsigned sent;} packets[BT_SCO_PACKETS];
} playback;
int bluetooth_playback_mode(const struct bluetooth_application *a)
{return a&&a->self==a&&a->profile&&((a->profile->version==2&&a->profile->headset.mode==4)||(a->profile->version==3&&a->profile->headset.mode==5));}
static int live(struct bluetooth_application *a)
{return bluetooth_playback_mode(a)&&!atomic_load_explicit(&a->failed,memory_order_acquire)&&
    !(atomic_load_explicit(a->recovery_state,memory_order_acquire)&4);}
static int external(struct bluetooth_application *a,const void *p,size_t n)
{return bt_range_disjoint(p,n,&playback,sizeof(playback))&&bt_range_disjoint(p,n,a,sizeof(*a))&&
    bt_range_disjoint(p,n,a->profile,sizeof(*a->profile))&&bt_range_disjoint(p,n,a->shared,sizeof(*a->shared));}
static int producer(struct bluetooth_application *a)
{
    if(!live(a))return 0;
    if(!playback.app){playback.app=a;playback.producer=xTaskGetCurrentTaskHandle();}
    return playback.app==a&&playback.producer==xTaskGetCurrentTaskHandle()&&playback.producer!=a->owner;
}
int bluetooth_playback_write(struct bluetooth_application *a,uint64_t ticket,uint32_t offset,const uint8_t *p,size_t n)
{
    if(!producer(a)||!external(a,p,n)||!n||n>1024||!ticket||atomic_load_explicit(&playback.sealed,memory_order_acquire))return -1;
    uint64_t head=atomic_load_explicit(&playback.accepted,memory_order_relaxed);
    if(head>=UINT64_MAX/1200||ticket!=head+1||offset!=playback.upload_bytes||offset>2400||n>2400-offset)return -1;
    if(atomic_load_explicit(&playback.slots[head%2].state,memory_order_acquire))return 0;
    memcpy(playback.slots[head%2].pcm+offset,p,n);playback.upload_bytes+=(unsigned)n;return live(a)?1:-1;
}
int bluetooth_playback_commit(struct bluetooth_application *a,uint64_t ticket,const uint8_t hash[32])
{
    if(!producer(a)||!external(a,hash,32)||atomic_load_explicit(&playback.sealed,memory_order_acquire))return -1;
    uint64_t head=atomic_load_explicit(&playback.accepted,memory_order_relaxed);
    if(head>=UINT64_MAX/1200||ticket!=head+1||playback.upload_bytes!=2400||atomic_load_explicit(&playback.slots[head%2].state,memory_order_acquire))return -1;
    uint8_t actual[32];storage_digest_bytes(playback.slots[head%2].pcm,2400,actual);if(memcmp(actual,hash,32))return -1;
    struct bluetooth_playback_result *r=&playback.slots[head%2].result;memset(r,0,sizeof(*r));r->ticket=ticket;r->codec=2;memcpy(r->sha256,actual,32);
    /* Both native tasks are existing privileged tasks. The kernel critical
     * section serializes publication with owner closure, as in I2S ingestion. */
    taskENTER_CRITICAL();
    int publish=live(a)&&!atomic_load_explicit(&playback.sealed,memory_order_relaxed);
    if(publish){atomic_store_explicit(&playback.slots[head%2].state,1,memory_order_release);
        atomic_store_explicit(&playback.accepted,head+1,memory_order_release);}
    taskEXIT_CRITICAL();
    if(!publish)return -1;playback.upload_bytes=0;return 1;
}
int bluetooth_playback_copy(struct bluetooth_application *a,struct bluetooth_playback_result *r,struct bluetooth_playback_status *s)
{
    if(!producer(a)||!external(a,r,sizeof(*r))||!external(a,s,sizeof(*s))||!bt_range_disjoint(r,sizeof(*r),s,sizeof(*s)))return -1;
    memset(r,0,sizeof(*r));memset(s,0,sizeof(*s));
    s->disconnected=atomic_load_explicit(&playback.closed,memory_order_acquire);
    s->acknowledged=atomic_load_explicit(&playback.acknowledged,memory_order_relaxed);
    s->accepted=atomic_load_explicit(&playback.accepted,memory_order_acquire);
    s->silence_frames=atomic_load_explicit(&playback.silence,memory_order_acquire);
    s->discarded_packets=atomic_load_explicit(&playback.rx_packets,memory_order_acquire);s->discarded_bytes=atomic_load_explicit(&playback.rx_bytes,memory_order_acquire);
    s->loss_packets=atomic_load_explicit(&playback.rx_loss,memory_order_acquire);
    s->silence_uart=atomic_load_explicit(&playback.silence_uart,memory_order_acquire);s->silence_released=atomic_load_explicit(&playback.silence_released,memory_order_acquire);
    s->silence_flushed=atomic_load_explicit(&playback.silence_flushed,memory_order_acquire);s->silence_not_sent=atomic_load_explicit(&playback.silence_not_sent,memory_order_acquire);
    s->sealed=atomic_load_explicit(&playback.sealed,memory_order_acquire);s->cancelled=atomic_load_explicit(&playback.cancelled,memory_order_acquire);
    if(s->accepted<s->acknowledged||s->accepted-s->acknowledged>2)return -1;
    s->free_slots=(uint32_t)(2-(s->accepted-s->acknowledged));
    unsigned slot=(unsigned)(s->acknowledged%2);int result=atomic_load_explicit(&playback.slots[slot].state,memory_order_acquire)==2;
    if(result)*r=playback.slots[slot].result;return live(a)?result:-1;
}
int bluetooth_playback_ack(struct bluetooth_application *a,uint64_t ticket)
{
    if(!producer(a))return -1;uint64_t tail=atomic_load_explicit(&playback.acknowledged,memory_order_relaxed);
    if(tail==UINT64_MAX||ticket!=tail+1||atomic_load_explicit(&playback.slots[tail%2].state,memory_order_acquire)!=2)return -1;
    atomic_store_explicit(&playback.slots[tail%2].state,0,memory_order_release);atomic_store_explicit(&playback.acknowledged,tail+1,memory_order_release);return 1;
}
int bluetooth_playback_end(struct bluetooth_application *a,int cancel)
{
    if(!producer(a)||cancel<0||cancel>1)return -1;
    playback.upload_bytes=0;
    taskENTER_CRITICAL();
    if(cancel)atomic_store_explicit(&playback.cancelled,1,memory_order_release);
    atomic_store_explicit(&playback.sealed,1,memory_order_release);
    taskEXIT_CRITICAL();return 1;
}
static unsigned pending(uint64_t ticket)
{unsigned n=0;for(unsigned i=0;i<BT_SCO_PACKETS;++i)n+=playback.packets[i].serial&&playback.packets[i].ticket==ticket;return n;}
int bluetooth_playback_step(struct bluetooth_application *a,struct bt_sco *q,uint64_t acl,uint64_t audio,uint64_t now,uint64_t deadline)
{
    if(!live(a)||xTaskGetCurrentTaskHandle()!=a->owner||!q||deadline<=now)return -1;
    /* USB may not have uploaded anything yet; it is the sole initializer of its
     * producer binding. Owner fields have static initial values independently. */
    const struct bt_sco_packet *t=bt_sco_result(q);
    if(t){unsigned i=0;while(i<BT_SCO_PACKETS&&playback.packets[i].serial!=t->serial)++i;if(i==BT_SCO_PACKETS)return -1;
        uint64_t ticket=playback.packets[i].ticket;
        if(ticket){struct bluetooth_playback_result *r=&playback.slots[(ticket-1)%2].result;
            if(r->ticket!=ticket||atomic_load_explicit(&playback.slots[(ticket-1)%2].state,memory_order_relaxed)!=1)return -1;
            if(!playback.packets[i].sent&&(t->state==BT_SCO_RETIRED||t->state==BT_SCO_FLUSHED))++r->uart_packets;
            if(t->state==BT_SCO_RETIRED)++r->released_packets;
            else if(t->state==BT_SCO_FLUSHED){++r->flushed_packets;r->flags|=2;}
            else if(t->state==BT_SCO_NOT_SENT){++r->not_sent_packets;r->flags|=2;}else return -1;}
        else {
            if(!playback.packets[i].sent&&(t->state==BT_SCO_RETIRED||t->state==BT_SCO_FLUSHED))atomic_fetch_add_explicit(&playback.silence_uart,1,memory_order_release);
            if(t->state==BT_SCO_RETIRED)atomic_fetch_add_explicit(&playback.silence_released,1,memory_order_release);
            else if(t->state==BT_SCO_FLUSHED)atomic_fetch_add_explicit(&playback.silence_flushed,1,memory_order_release);
            else if(t->state==BT_SCO_NOT_SENT)atomic_fetch_add_explicit(&playback.silence_not_sent,1,memory_order_release);else return -1;
        }
        if(bt_sco_release_result(q,t->serial)!=1)return -1;memset(&playback.packets[i],0,sizeof(playback.packets[i]));
    }
    for(unsigned i=0;i<BT_SCO_PACKETS;++i)if(playback.packets[i].serial&&!playback.packets[i].sent){
        for(unsigned j=0;j<BT_SCO_PACKETS;++j)if(q->tx[j].serial==playback.packets[i].serial&&q->tx[j].state==BT_SCO_SENT){
            playback.packets[i].sent=1;if(playback.packets[i].ticket)++playback.slots[(playback.packets[i].ticket-1)%2].result.uart_packets;else atomic_fetch_add_explicit(&playback.silence_uart,1,memory_order_release);}}
    unsigned slot=(unsigned)(playback.next_owner%2);
    unsigned available=atomic_load_explicit(&playback.slots[slot].state,memory_order_acquire)==1;
    uint64_t ticket=playback.next_owner+1;
    if(available&&playback.slots[slot].result.ticket!=ticket)return -1;
    unsigned cancel=atomic_load_explicit(&playback.cancelled,memory_order_acquire);
    if(cancel&&playback.ready){playback.ready=0;playback.frame_offset=0;}
    if(available&&(cancel||playback.pcm_offset==2400)&&!playback.ready&&!pending(ticket)){
        struct bluetooth_playback_result *r=&playback.slots[slot].result;
        r->firmware=a->firmware_generation;r->uart=a->uart_generation;r->acl=acl;r->audio=audio;
        if(cancel)r->flags|=1;
        atomic_store_explicit(&playback.slots[slot].state,2,memory_order_release);
        ++playback.next_owner;playback.pcm_offset=0;available=0;
    }
    if(cancel||atomic_load_explicit(&playback.closed,memory_order_acquire))return 1;
    unsigned open=0;for(unsigned i=0;i<BT_SCO_LINKS;++i)open|=q->links[i].generation==audio&&q->links[i].phase==BT_SCO_OPEN;
    if(!open)return 1;
    if(!playback.ready&&now>=playback.next_frame){
        const uint8_t *pcm=NULL;uint8_t zero[240]={0};
        if(available&&playback.pcm_offset<2400){pcm=playback.slots[slot].pcm+playback.pcm_offset;playback.started=1;}
        else if(atomic_load_explicit(&playback.sealed,memory_order_acquire))return 1;
        else pcm=zero;
        if(bluetooth_msbc_encode(pcm,playback.frame)!=1)return -1;
        if(pcm!=zero){playback.pcm_offset+=240;playback.slots[slot].result.encoded_samples+=120;}
        else atomic_fetch_add_explicit(&playback.silence,1,memory_order_release);
        playback.ready=1;playback.frame_offset=0;
        /* Each frame remembers its owner even if the period finishes encoding. */
        playback.frame[59]=0;
        playback.started=pcm!=zero?2:1;
    }
    if(playback.ready){unsigned i=0;while(i<BT_SCO_PACKETS&&playback.packets[i].serial)++i;if(i==BT_SCO_PACKETS)return 1;
        unsigned bytes=60-playback.frame_offset;if(bytes>q->mtu)bytes=q->mtu;uint64_t serial=0;
        int r=bt_sco_send(q,audio,playback.frame+playback.frame_offset,bytes,deadline,&serial);if(r<0)return -1;
        if(r==1){uint64_t owner=playback.started==2?ticket:0;playback.packets[i].serial=serial;playback.packets[i].ticket=owner;
            if(owner)playback.slots[slot].result.submitted_bytes+=bytes;playback.frame_offset+=bytes;
            if(playback.frame_offset==60){playback.ready=0;playback.frame_offset=0;playback.next_frame=now>UINT64_MAX-7500?UINT64_MAX:now+7500;}}
    }
    return 1;
}
int bluetooth_playback_done(struct bluetooth_application *a)
{return live(a)&&atomic_load_explicit(&playback.sealed,memory_order_acquire)&&
    playback.next_owner==atomic_load_explicit(&playback.accepted,memory_order_acquire)&&!playback.ready;}
int bluetooth_playback_discard(struct bluetooth_application *a,unsigned bytes,unsigned status)
{
    if(!live(a)||xTaskGetCurrentTaskHandle()!=a->owner)return -1;
    atomic_fetch_add_explicit(&playback.rx_packets,1,memory_order_release);atomic_fetch_add_explicit(&playback.rx_bytes,bytes,memory_order_release);
    if(status)atomic_fetch_add_explicit(&playback.rx_loss,1,memory_order_release);return 1;
}
void bluetooth_playback_closed(struct bluetooth_application *a)
{if(live(a)&&xTaskGetCurrentTaskHandle()==a->owner)atomic_store_explicit(&playback.closed,1,memory_order_release);}

void bluetooth_playback_stop(struct bluetooth_application *a)
{if(live(a)&&xTaskGetCurrentTaskHandle()==a->owner){taskENTER_CRITICAL();atomic_store_explicit(&playback.cancelled,1,memory_order_release);atomic_store_explicit(&playback.sealed,1,memory_order_release);taskEXIT_CRITICAL();}}

int bluetooth_playback_cancelled(struct bluetooth_application *a)
{return live(a)&&atomic_load_explicit(&playback.cancelled,memory_order_acquire);}
