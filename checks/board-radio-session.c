/* Only changed session admission and external retention; no protocol replay. */
#include "../drivers/net/a733_radio_session.c"
#include <assert.h>
#include <stdio.h>
static struct a733_radio_session subject;
static struct a733_radio_platform platform;
static struct a733_clock_leases clocks;
static struct a733_pins pins;
static struct a733_rail_leases rails;
static struct a733_pmic pmic;
static struct a733_gpu_resource_manager gpu;
static uint8_t blobs[4][1];
static struct aic8800_supplicant_config profile;
static TaskHandle_t task=(TaskHandle_t)(uintptr_t)1;
static unsigned boundary,initializations,attachments,stops;
TaskHandle_t xTaskGetCurrentTaskHandle(void){return task;}
static int current(void *p){(void)p;return 1;}
static int provider_outside(void *p,const void *o,size_t n){(void)p;(void)o;(void)n;return 1;}
enum a733_radio_platform_result a733_radio_platform_live(struct a733_radio_platform *p,const struct a733_radio_borrow *b,uint64_t d)
{assert(p==&platform&&b==&subject.borrow&&d==900);return A733_RADIO_READY;}
enum a733_radio_platform_result a733_radio_platform_poll(struct a733_radio_platform *p)
{assert(p==&platform);return A733_RADIO_RETAINED;}
int aic8800_transport_attachment_ready(const struct aic8800_sdio_transport *t)
{assert(t==&subject.transport);return boundary;}
int aic8800_supplicant_init(struct aic8800_supplicant *s,const struct aic8800_supplicant_config *c)
{assert(s==&subject.supplicant&&c==&profile);++initializations;s->initialized=1;return 1;}
int aic8800_transport_start_security(struct aic8800_sdio_transport *t,struct aic8800_security *s,
    struct aic8800_supplicant *u,uint64_t d)
{assert(t==&subject.transport&&s==&subject.security&&u==&subject.supplicant&&d==900);++attachments;return 1;}
void sunxi_sdio_stop(struct sunxi_sdio *s,enum sunxi_sdio_error e){(void)e;assert(s==&platform.host);++stops;}
void aic8800_transport_fault_receive(struct aic8800_sdio_transport *t,const void *o){(void)t;(void)o;}
void aic8800_transport_poll(struct aic8800_sdio_transport *t){(void)t;}
void aic8800_network_fault(struct aic8800_network *n){assert(n==&subject.sockets.network);atomic_store(&n->failed,1);}
int sunstone_net_poll_owned(struct sunstone_net *s){assert(s==&subject.sockets);return 1;}
int main(void)
{
    platform.clocks=&clocks;platform.pins=&pins;platform.rails=&rails;platform.pmic=&pmic;
    platform.ops.current=current;pmic.ops.outside=provider_outside;rails.binding.gpu=&gpu;
    subject.self=&subject;subject.platform=&platform;subject.stage=MANAGEMENT;subject.deadline=900;
    subject.config.owner_task=task;atomic_init(&subject.busy,0);
    for(unsigned i=0;i<4;++i){subject.config.firmware.blobs[i].data=blobs[i];subject.config.firmware.blobs[i].bytes=1;}
    assert(!a733_radio_session_start_security(&subject,&profile,900));
    assert(!initializations&&!attachments&&!subject.supplicant.initialized&&!subject.fault);
    boundary=1;assert(a733_radio_session_start_security(&subject,&profile,900));
    assert(initializations==1&&attachments==1&&!subject.fault);
    task=(TaskHandle_t)(uintptr_t)2;
    assert(a733_radio_session_retain(&subject)==A733_SESSION_INVALID&&!stops);
    task=subject.config.owner_task;
    assert(a733_radio_session_retain(&subject)==A733_SESSION_RETAINED&&subject.fault&&stops==1);
    puts("PASS board radio session: busy leaves keys fresh; ready attaches; owner-only failure retains before network");
    return 0;
}
