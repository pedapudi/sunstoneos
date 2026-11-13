/* Reuse the source-backed card/bootstrap model; the interrupt binding itself
 * is exercised by event-misc-sdio.c with the real scheduler. */
#define AIC8800_TRANSPORT_FIXTURE_MAIN transport_reference_main
#include "aic8800-sdio-transport.c"
int main(void)
{
    begin(AIC8800_PAD_KEEP);setup();
    /* The controller fixture has separately tested event preparation. This
     * case starts at its READY handoff to isolate protocol sleep selection. */
    host.events=1;m.allow_irq_ack=1;drop_setup_confirmation=1;identity(2000);
    for(unsigned i=0;i<1000&&!transport.irq_wait;++i)step();
    assert(transport.irq_wait&&!transport.pending&&host.state==SUNXI_SDIO_READY);
    unsigned commands=m.command_count;
    assert(!aic8800_transport_event_work(&transport,m.now));
    assert(aic8800_transport_event_deadline(&transport,m.now)==transport.deadline);
    for(unsigned i=0;i<100;++i)step();
    assert(m.command_count==commands&&!aic8800_transport_event_work(&transport,m.now));
    m.irq_level=1;sunxi_sdio_poll(&host);assert(host.sdio_pending);
    assert(aic8800_transport_event_work(&transport,m.now));
    m.irq_level=0;m.card[1][4]=1;
    for(unsigned i=0;i<1000&&transport.state==AIC8800_TRANSPORT_IDENTITY;++i)step();
    assert(transport.state==AIC8800_TRANSPORT_COMPLETE&&!aic8800_transport_event_work(&transport,m.now));
    begin(AIC8800_PAD_KEEP);setup();host.events=1;m.allow_irq_ack=1;zero_credit=1;identity(2000);
    for(unsigned i=0;i<100&&!transport.wait_until;++i)step();
    assert(transport.wait_until>m.now&&!transport.pending);
    uint64_t due=transport.wait_until;
    assert(aic8800_transport_event_deadline(&transport,m.now)==due);
    assert(!aic8800_transport_event_work(&transport,due-1)&&aic8800_transport_event_work(&transport,due));
    commands=m.command_count;while(m.now+1<due)step();assert(m.command_count==commands);
    step();assert(m.now==due&&m.command_count==commands); /* admission, no premature command */
    step();assert(m.command_count==commands+1);
    puts("PASS AIC empty receive waits for function IRQ; credit retries keep exact declared cadence");
}
