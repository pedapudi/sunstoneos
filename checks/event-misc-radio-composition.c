/* Actual board orchestration, including its full existing source-backed
 * coordinator model; scripted lower-session progress isolates wait admission. */
#define main board_reference_main
#define EVENT_MISC_RADIO_COMPOSITION
#include "board-radio.c"
#undef main
static unsigned blocks;
int a733_radio_events_init(struct a733_radio_events *e,struct a733_radio_platform *p,TaskHandle_t owner)
{e->platform=p;e->owner=owner;return 1;}
int a733_radio_events_begin(struct a733_radio_events *e){(void)e;return 1;}
void a733_radio_events_irq(struct a733_radio_events *e){(void)e;assert(0);}
int a733_radio_events_prepare(struct a733_radio_events *e,uint64_t *until)
{(void)e;(void)until;return 0;}
int aic8800_transport_event_work(const struct aic8800_sdio_transport *t,uint64_t now)
{(void)t;(void)now;return 0;}
uint64_t aic8800_transport_event_deadline(const struct aic8800_sdio_transport *t,uint64_t now)
{(void)t;(void)now;return 7000;}
uint64_t sunstone_deadline_now_us(void){return time_us;}
int sunstone_deadline_register(void){return 1;}
int sunstone_deadline_wait_until(uint64_t until){assert(until>time_us);time_us=until;++blocks;return 1;}
int sunstone_net_wait_owned(struct sunstone_net *s){(void)s;assert(0);return 0;}
BaseType_t xTaskGenericNotify(TaskHandle_t t,UBaseType_t i,uint32_t v,eNotifyAction a,uint32_t *old)
{(void)t;(void)i;(void)v;(void)a;(void)old;return pdPASS;}
UBaseType_t uxQueueMessagesWaiting(const QueueHandle_t q){(void)q;return 0;}
int main(void)
{
    assert(board_reference_main()==0);
    memset(&app,0,sizeof(app));atomic_store(&recovery_state,1);atomic_store(&shared.pmic_ready,1);
    time_us=100;app.self=&app;app.constructed=1;app.config.shared=&shared;app.config.recovery_state=&recovery_state;
    app.config.session.owner_task=task;app.config.scan_only=app.config.associate=1;
    app.config.startup_deadline_us=8000;app.platform.config.lifetime_deadline_us=10000;
    app.platform.stage=RSTAGE_READY;app.platform.host.state=SUNXI_SDIO_READY;
    app.session.deadline=9000;app.session.transport.initialized=1;
    app.stage=SCANNING;app.operation_deadline=8000;app.entropy_begun=1;
    entropy_is_ready=1;entropy_is_failed=0;scan_finished=1;session_result=A733_SESSION_MANAGEMENT;
    assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==STARTING&&app.event_ready);
    assert(board_radio_owner_wait(&app,0)==1&&!blocks&&!app.event_ready&&time_us==100);
    /* A bounded session pass advances to the pure-CPU START_RUNTIME transition. */
    app.config.associate=0;app.scan_started=1;app.session.transport.state=AIC8800_TRANSPORT_START_CONFIRMED;
    event_clock_cost=600;event_session_step=1;
    assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.event_ready);
    event_clock_cost=event_session_step=0;
    uint64_t before=time_us;assert(board_radio_owner_wait(&app,0)==1&&!blocks&&time_us==before);
    /* Former transport/session deadlines cannot pace controller retirement. */
    app.stage=SHUTTING_DOWN;app.platform.stop_requested=1;app.platform.stop_deadline=app.platform.deadline=9000;
    app.platform.host.state=SUNXI_SDIO_QUIESCED;app.platform.stage=RSTAGE_SHUTDOWN_CLOCKS;app.session.deadline=1;
    assert(board_radio_owner_wait(&app,0)==1&&blocks==1&&time_us==before+50);
    app.stage=ONLINE;app.platform.stop_requested=0;app.platform.aborting=1;atomic_store(&app.failed,1);
    app.session.sockets.initialized=app.session.sockets.event_bound=1;app.session.sockets.requests=(QueueHandle_t)(uintptr_t)1;
    before=time_us;assert(board_radio_owner_wait(&app,0)==1&&blocks==2&&time_us==before+50);
    puts("PASS actual board poll preserves scan/budget ready work and retires inactive protocol timers");
}
