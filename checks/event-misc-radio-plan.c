/* Production board wait planning; source ownership and device MMIO have their
 * independent full-controller check in event-misc-sdio.c. */
#include "host_rtos.h"
#include "radio.h"
#include "sunstone_deadline.h"
#include <assert.h>
#include <stdio.h>
enum { STARTING,CONNECTING,SECURING,ONLINE,SCANNING,SCANNED,SHUTTING_DOWN,DOWN };
static struct board_radio radio;
static struct a733_native_usb shared;
static unsigned ready,certificates;
int os_radio_owner_blocked_until(uint64_t until) __attribute__((weak));
int os_radio_owner_blocked_until(uint64_t until){assert(until>host_time_us());++certificates;return 1;}
int a733_radio_events_begin(struct a733_radio_events *e){return e==&radio.events;}
int a733_radio_events_prepare(struct a733_radio_events *e,uint64_t *until)
{assert(e==&radio.events);(void)until;return 0;}
int aic8800_transport_event_work(const struct aic8800_sdio_transport *t,uint64_t now)
{(void)t;(void)now;return ready;}
#include "radio-protocol-deadline.inc"
#include "radio-plan-events.inc"
static void owner(void *unused)
{
    (void)unused;radio.constructed=1;radio.config.shared=&shared;
    radio.platform.config.lifetime_deadline_us=5000000;radio.config.startup_deadline_us=2000000;
    assert(!event_work(&radio)&&radio.event_until==2000000&&certificates==1);
    atomic_store(&shared.pmic_ready,1);radio.stage=ONLINE;radio.platform.stage=RSTAGE_READY;
    radio.platform.host.state=SUNXI_SDIO_READY;radio.session.transport.initialized=1;
    radio.session.deadline=4000000;radio.session.config.renew_margin_us=500000;radio.session.transport.deadline=3700000;
    assert(!event_work(&radio)&&radio.event_until==3500000); /* Renewal wins; startup bound is retired. */
    radio.event_caller_limit=3000000;assert(!event_work(&radio)&&radio.event_until==3000000);
    ready=1;unsigned claims=certificates;assert(event_work(&radio)==1&&certificates==claims);ready=0;
    host_advance_us(123);radio.event_caller_limit=120;
    assert(event_work(&radio)==1&&event_work(&radio)==-1); /* One pass for matured companion deadline. */
    radio.event_caller_limit=200;assert(!event_work(&radio)&&radio.event_until==200);
    host_advance_us(77);assert(event_work(&radio)==1);
    radio.event_caller_limit=0;radio.session.deadline=5000000;radio.session.config.renew_margin_us=0;
    radio.session.transport.deadline=199;assert(event_work(&radio)==1); /* Provider deadline matured after the service pass. */
    radio.session.transport.deadline=4000000;radio.session.transport.supplicant=&radio.session.supplicant;
    radio.session.supplicant.state=AIC8800_SUPPLICANT_NEW;radio.session.supplicant.sae=(void *)1;
    radio.session.supplicant.sae_accepted=1;radio.session.supplicant.sae_deadline=199;
    assert(!event_work(&radio)&&radio.event_until==4000000); /* Completed SAE has no active timer. */
    radio.session.supplicant.sae_accepted=0;radio.session.supplicant.sae_deadline=300;
    assert(!event_work(&radio)&&radio.event_until==300);
    radio.session.supplicant.sae=NULL;assert(!event_work(&radio)&&radio.event_until==4000000);
    radio.session.transport.initialized=0;radio.platform.host.state=SUNXI_SDIO_UNPREPARED;
    radio.platform.stage=RSTAGE_SUPPLY_WAIT;radio.platform.wait_since=0;
    assert(!event_work(&radio)&&radio.event_until==10001);
    radio.platform.stage=RSTAGE_MODULE_SETTLE;assert(!event_work(&radio)&&radio.event_until==50001);
    puts("PASS radio planner keeps ready work live and combines exact source deadlines");
}
int main(void){host_rtos_run(owner,NULL,4,6000000);}
