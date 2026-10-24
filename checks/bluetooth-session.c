/* Execute only new bootstrap construction/readiness. Earlier board session
 * test bodies remain unexecuted. Default Wi-Fi source behavior is reused. */
#define main bluetooth_archived_session_main
#include "board-radio-session.c"
#undef main
static unsigned wifi_initializations;
int aic8800_firmware_init(struct aic8800_firmware *f,const struct aic8800_firmware_config *c)
{assert(f==&subject.firmware);f->config=*c;f->initialized=1;return 1;}
int aic8800_runtime_init(struct aic8800_runtime *r,const struct aic8800_runtime_config *c)
{assert(r==&subject.runtime);r->config=*c;r->initialized=1;return 1;}
int aic8800_wifi_init(struct aic8800_wifi *w,const struct aic8800_wifi_config *c)
{(void)w;(void)c;++wifi_initializations;return 1;}
int aic8800_data_init(struct aic8800_data *d){(void)d;return 1;}
int aic8800_security_init(struct aic8800_security *s){(void)s;return 1;}
int main(void)
{
    platform.initialized=1;platform.clocks=&clocks;platform.pins=&pins;platform.rails=&rails;platform.pmic=&pmic;
    platform.ops.current=current;pmic.pmic_bus_ops.outside=provider_outside;rails.binding.gpu=&gpu;
    platform.config.deadline_us=100;platform.config.lifetime_deadline_us=10000;
    struct a733_radio_session_config c={.owner_task=task,.owner=22,.bootstrap_deadline=900,
        .runtime_lease_us=1000,.renew_margin_us=200,.transport={.ticks_per_ms=1000}};
    for(unsigned i=0;i<4;++i)c.firmware.blobs[i]=(struct aic8800_firmware_blob){blobs[i],1};
    assert(a733_radio_session_init_bootstrap(&subject,&platform,&c)&&subject.bootstrap_only&&!wifi_initializations);
    uint64_t generation=0;
    assert(a733_radio_session_bootstrap(&subject,&generation)==0&&!generation);
    /* Reuse existing authenticated firmware/runtime attachment proof. These
     * explicit lower-worker checkpoints do not claim another upload execution. */
    subject.stage=BOOTSTRAP;subject.borrow.generation=4;subject.firmware.state=AIC8800_FW_START_CONFIRMED;
    subject.firmware.bt_patch_applied=1;subject.firmware.binding_required=1;subject.runtime.state=AIC8800_RUNTIME_RECEIVING;
    subject.transport.state=AIC8800_TRANSPORT_RUNTIME;subject.transport.card=&platform.card;
    subject.transport.firmware=&subject.firmware;subject.firmware.owner=&subject.transport;
    subject.transport.runtime=&subject.runtime;subject.runtime.owner=&subject.transport;
    subject.transport.receive_owner=&subject.dispatcher;subject.dispatcher.initialized=1;subject.dispatcher.transport=&subject.transport;
    assert(a733_radio_session_bootstrap(&subject,&generation)==0&&!generation);
    subject.runtime.binding_confirmed=1;
    assert(a733_radio_session_bootstrap(&subject,&generation)==1&&generation==4);
    subject.dispatcher.faulted=1;generation=0;
    assert(a733_radio_session_bootstrap(&subject,&generation)==-1&&!generation&&stops==1);
    assert(!admission(&subject,900)&&!wifi_initializations);
    assert(a733_radio_session_retain(&subject)==A733_SESSION_RETAINED&&stops==2);
    puts("PASS Bluetooth session: bootstrap omits Wi-Fi, binding and exact borrowed graph gate firmware generation, failure retains");
    return 0;
}
