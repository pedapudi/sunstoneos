/* Actual transport, supplicant observation and eloop dispatch. The RSN action
 * step is an explicit modeled boundary; no credentials or RF are involved. */
#define AIC8800_TRANSPORT_FIXTURE_MAIN transport_reference_main
#include "aic8800-sdio-transport.c"
#include "aic8800_runtime.h"
#include "aic8800_supplicant.h"
#include "transport-phase.inc"
static struct aic8800_supplicant *active;
static struct aic8800_supplicant supplicant;
static struct aic8800_runtime runtime;
static struct aic8800_wifi wifi;
static struct aic8800_data data;
static struct aic8800_security security;
static unsigned fired,stepped;
#include "rsn-timer-events.inc"
void aic8800_supplicant_step(struct aic8800_supplicant *s,uint64_t now)
{
    assert(s==&supplicant&&s->count==1&&!s->action_active&&now==m.now);
    ++stepped;s->action_active=1;data.state=AIC8800_DATA_REQUEST;
}
int aic8800_supplicant_receive(struct aic8800_supplicant *s,const struct aic8800_data_frame *f,uint64_t now)
{(void)s;(void)f;(void)now;assert(0);return 0;}
static void timer(void *context,void *argument)
{
    assert(context==&supplicant&&!argument&&active==&supplicant);
    ++fired;supplicant.count=1;supplicant.actions[0].kind=AIC8800_SUPPLICANT_EAPOL;
}
int main(void)
{
    begin(AIC8800_PAD_KEEP);setup();host.events=1;m.allow_irq_ack=1;
    transport.state=AIC8800_TRANSPORT_RUNTIME;transport.phase=RX_STATUS;transport.irq_wait=1;
    transport.deadline=m.now+2000;transport.runtime=&runtime;transport.wifi=&wifi;
    transport.data=&data;transport.security=&security;transport.supplicant=&supplicant;
    runtime=(struct aic8800_runtime){.owner=&transport,.initialized=1,.state=AIC8800_RUNTIME_RECEIVING};
    wifi=(struct aic8800_wifi){.owner=&transport,.initialized=1,.state=AIC8800_WIFI_READY,
        .link=AIC8800_WIFI_SECURITY_PENDING,.association_generation=1};
    data=(struct aic8800_data){.owner=&transport,.initialized=1,.wifi=&wifi,.deadline=transport.deadline,.state=AIC8800_DATA_IDLE};
    data.result.generation=1;
    security=(struct aic8800_security){.owner=&transport,.initialized=1,.wifi=&wifi,.generation=1,.deadline=transport.deadline,.state=AIC8800_SECURITY_WAITING};
    supplicant=(struct aic8800_supplicant){.owner=&transport,.initialized=1,.wifi=&wifi,.data=&data,
        .security=&security,.generation=1,.state=AIC8800_SUPPLICANT_HANDSHAKE,.deadline=transport.deadline};
    supplicant.timers[0]=(struct aic8800_supplicant_timer){timer,&supplicant,NULL,m.now+10};
    unsigned commands=m.command_count;m.now+=10;
    assert(aic8800_transport_event_deadline(&transport,m.now)==m.now);
    aic8800_transport_poll(&transport);
    if(!(fired==1&&stepped==1&&!supplicant.timers[0].handler&&m.command_count==commands))
        fprintf(stderr,"timer=%u step=%u commands=%u/%u state=%u error=%u supplicant=%u phase=%u\n",fired,stepped,m.command_count,commands,transport.state,transport.error,supplicant.state,transport.phase);
    assert(fired==1&&stepped==1&&!supplicant.timers[0].handler&&m.command_count==commands);
    assert(transport.state==AIC8800_TRANSPORT_RUNTIME&&transport.phase==RX_STATUS);
    assert(!aic8800_transport_event_work(&transport,m.now));
    assert(aic8800_transport_event_deadline(&transport,m.now)==transport.deadline);
    supplicant.count=0;supplicant.action_active=0;supplicant.completed=1;supplicant.port_pending=0;
    supplicant.state=AIC8800_SUPPLICANT_RETIRING;
    supplicant.timers[0]=(struct aic8800_supplicant_timer){timer,&supplicant,NULL,m.now-1};
    assert(!aic8800_transport_event_work(&transport,m.now));aic8800_transport_poll(&transport);
    assert(stepped==1&&fired==1&&m.command_count==commands);
    assert(aic8800_transport_event_deadline(&transport,m.now)==transport.deadline);
    supplicant.state=AIC8800_SUPPLICANT_RETIRED;
    assert(!aic8800_transport_event_work(&transport,m.now));aic8800_transport_poll(&transport);
    assert(stepped==1&&m.command_count==commands);
    supplicant.state=AIC8800_SUPPLICANT_RETIRING;security.state=AIC8800_SECURITY_IDLE;
    assert(aic8800_transport_event_work(&transport,m.now));security.state=AIC8800_SECURITY_DONE;
    assert(aic8800_transport_event_work(&transport,m.now));
    supplicant.state=AIC8800_SUPPLICANT_HANDSHAKE;supplicant.completed=0;supplicant.deferred_bytes=1;data.state=AIC8800_DATA_IDLE;
    assert(aic8800_transport_event_work(&transport,m.now));
    puts("PASS due RSN timer dispatches from idle RX_STATUS without tick or receive IRQ");
}
