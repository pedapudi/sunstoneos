/* Owner ticks for transport setup and bootstrap identity, first with one worker
 * call per tick, then with calls repeated while the worker reports progress. */
#define AIC8800_TRANSPORT_FIXTURE_MAIN transport_fixture_main
#include "aic8800-sdio-transport.c"

static unsigned batched, waiting_calls, calls;
static void tick(void)
{
    ++m.now;
    for (unsigned n = 0;; ++n) {
        /* The fixture model completes bus work instantly; the cap would only
         * bind if a waiting call were miscounted as progress. */
        assert(n < 64);
        uint64_t before = transport.progress; unsigned words = m.words;
        aic8800_transport_poll(&transport); ++calls;
        assert(m.words - words <= SUNXI_SDIO_FIFO_BUDGET); /* Per-call bound unchanged. */
        if (transport.progress == before) { ++waiting_calls; break; }
        if (!batched) break;
    }
}
static unsigned run(unsigned mode)
{
    batched = mode; calls = waiting_calls = 0; begin(AIC8800_PAD_WRITE_80); ready_delay = 1;
    unsigned ticks = 0;
    for (; ticks < 3000 && transport.state == AIC8800_TRANSPORT_SETUP; ++ticks) tick();
    assert(transport.state == AIC8800_TRANSPORT_READY && m.card[1][7] == 1);
    identity(5000);
    for (; ticks < 6000 && transport.state == AIC8800_TRANSPORT_IDENTITY; ++ticks) tick();
    assert(transport.state == AIC8800_TRANSPORT_COMPLETE && transmissions == 3 && receive_commands == 3);
    assert(transport.identity.revision == 3 && transport.identity.is_h && pad_writes == 3);
    /* Batching yields only on a waiting call: at most one per tick. */
    if (batched) assert(waiting_calls <= ticks);
    return ticks;
}
int main(void)
{
    unsigned single = run(0), repeated = run(1);
    printf("radio poll batching: %u ticks with one call per tick, %u ticks repeating while progress advances\n",
           single, repeated);
    assert(repeated * 2 <= single);
    /* A finished worker reports no progress and performs no access. */
    unsigned accesses = m.accesses; uint64_t progress = transport.progress;
    aic8800_transport_poll(&transport);
    assert(m.accesses == accesses && transport.progress == progress);
    return 0;
}
