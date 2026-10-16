/* Reuse the existing register model without running its independent suite. */
#define main sdio_component_fixture_unused_main
#include "sdio-controller.c"
#undef main
#include "aic8800_sdio_transport.h"

static struct aic8800_sdio_transport transport;
static uint8_t receive[4096];
static unsigned transmissions, receive_commands, credit_reads, enable_reads, pad_writes;
static int add_unrelated, malformed, response_status, ready_delay, zero_credit, fail_setup;
static uint64_t pad_time, byte_mode_time;
static int drop_setup_confirmation, fail_pad_restore, setup_write_applied, fail_tx;
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static void confirmation(uint8_t *p, uint32_t address, uint32_t value)
{
    p[0] = 20; p[2] = 0x11; p[4] = 1; p[5] = 4; p[10] = 8;
    put32(p + 16, address); put32(p + 20, value);
}
static void tx_done(void)
{
    ++transmissions;
    int write_setup = transmissions == 1;
    assert(m.transmitted[0] == (write_setup ? 20 : 16) && m.transmitted[2] == 0x11);
    assert(m.transmitted[3] == (write_setup ? 0x7e : 0xd5));
    assert(m.transmitted[8] == (write_setup ? 2 : 0) && m.transmitted[9] == 4);
    assert(m.transmitted[14] == (write_setup ? 8 : 4));
    for (unsigned i = write_setup ? 24 : 20; i < 512; ++i) assert(m.transmitted[i] == 0);
    uint32_t address = get32(m.transmitted + 16);
    if (write_setup) {
        assert(address == 0x40500058 && get32(m.transmitted + 20) == 0x40);
        setup_write_applied = 1;
    } else {
        assert(address == (transmissions == 2 ? 0x40500000u : 4u));
        assert(transport.setup_confirmed && setup_write_applied);
        if (transport.config.pad == AIC8800_PAD_WRITE_80)
            assert(m.card[0][0xf1] == 0 && m.now - pad_time >= 10);
    }
    memset(receive, 0, sizeof(receive));
    unsigned at = 0;
    if (add_unrelated && transmissions == 1) {
        confirmation(receive, 0x12345678, 0xabcdef12); at = 24;
    }
    if (write_setup) {
        /* The published sender imposes no echo semantics on these CFM fields. */
        confirmation(receive + at, 0xdeadbeef, 0x11223344); receive[at + 4] = 3;
    } else confirmation(receive + at, address, transmissions == 2 ? 0x00c30000u : 0x70u);
    if (malformed) receive[10] = 9; /* Both confirmations declare eight bytes. */
    m.card[1][4] = drop_setup_confirmation && write_setup ? 0 :
        response_status ? (uint8_t)response_status : 1;
    m.card[1][5] = 128;
}
static int transport_read(void *context, uint32_t offset, uint32_t *out)
{
    if (offset != 0x200) return read_cb(context, offset, out);
    ++m.accesses;
    if (m.fail_fifo && m.position == 4) return 0;
    assert(m.active && !m.write && !m.stall_fifo && m.position + 4 <= m.bytes);
    *out = get32(receive + m.position); m.position += 4; ++m.words;
    if (m.position == m.bytes) { m.regs[0x38 / 4] |= 8; m.card[1][4] = 0; }
    return 1;
}
static int transport_write(void *context, uint32_t offset, uint32_t value)
{
    if (offset == 0x18 && !(value & (1u << 21))) {
        uint32_t arg = m.regs[0x1c / 4]; unsigned cmd = value & 63;
        unsigned fn = (arg >> 28) & 7, addr = (arg >> 9) & 0x1ffff;
        if (cmd == 52 && (arg >> 31)) {
            if (fn == 0 && addr == 2 && !ready_delay) m.card[0][3] |= (uint8_t)arg;
            if (fn == 0 && (addr == 0xf1 || addr == 0xf2)) { ++pad_writes; pad_time = m.now; }
            if (fail_pad_restore && fn == 0 && addr == 0xf1 && (uint8_t)arg == 0) {
                assert(write_cb(context, offset, value)); return 0;
            }
            if (fn == 1 && addr == 7) byte_mode_time = m.now;
            if (fail_setup && fn == 0 && addr == 0x111) {
                assert(write_cb(context, offset, value)); return 0; /* Write took effect. */
            }
        } else if (cmd == 52) {
            if (fn == 0 && addr == 3 && ++enable_reads >= 3) m.card[0][3] |= 2;
            if (fn == 1 && addr == 3) { ++credit_reads; m.card[1][3] = zero_credit || credit_reads < 3 ? 0 : 1; }
        } else if (cmd == 53) {
            assert(fn == 1 && !(arg & (1u << 26))); /* Fixed FIFO address. */
            assert(m.card[0][0x110] == 0 && m.card[0][0x111] == 2 && (m.card[0][3] & 2));
            if (arg >> 31) assert(addr == 0x10 && m.regs[0x14 / 4] == 512);
            else { assert(addr == 0x0f); ++receive_commands; }
        }
    }
    if (fail_tx && offset == 0x200 && m.write && m.position == 4) { ++m.accesses; return 0; }
    int result = write_cb(context, offset, value);
    if (offset == 0x200 && m.write && m.position == m.bytes) tx_done();
    return result;
}
static void begin(enum aic8800_transport_pad pad)
{
    /* Each case discards a complete simulated device; no live hardware exists. */
    reset_model();
    struct sunxi_sdio_config cfg = host.config;
    cfg.ops.read32 = transport_read; cfg.ops.write32 = transport_write;
    memset(&host, 0, sizeof(host)); assert(sunxi_sdio_init(&host, &cfg));
    memset(&transport, 0, sizeof(transport)); memset(receive, 0, sizeof(receive));
    transmissions = receive_commands = credit_reads = enable_reads = pad_writes = 0;
    add_unrelated = malformed = response_status = ready_delay = zero_credit = fail_setup = 0;
    pad_time = byte_mode_time = 0;
    drop_setup_confirmation = fail_pad_restore = setup_write_applied = fail_tx = 0;
    prepare(); enumerate(); /* Real controller and card APIs, existing model. */
    struct aic8800_transport_config tc = { .variant = AIC8800_TRANSPORT_D80_BOOTSTRAP,
        .pad = pad, .ticks_per_ms = 10, .command_timeout = 100, .poll_interval = 5 };
    m.card[0][2] = 4; m.card[0][3] = 4; m.card[0][4] = 0x80; m.card[1][0] = 0x40;
    m.card[0][0x110] = 64;
    assert(aic8800_transport_init(&transport, &card, &tc));
    assert(!aic8800_transport_init(&transport, &card, &tc));
    assert(aic8800_transport_setup(&transport, m.now + 2000));
}
static void step(void)
{
    ++m.now; unsigned words = m.words;
    aic8800_transport_poll(&transport);
    assert(m.words - words <= SUNXI_SDIO_FIFO_BUDGET);
}
static void setup(void)
{
    for (unsigned i = 0; i < 300 && transport.state == AIC8800_TRANSPORT_SETUP; ++i) step();
    assert(transport.state == AIC8800_TRANSPORT_READY);
    assert(transport.prior_enable == 4 && m.card[0][2] == 6 && m.card[0][4] == 0x87);
    assert(m.card[1][0] == 0x47 && transport.prior_block_bytes == 64);
    assert(byte_mode_time - pad_time >= 10 && m.card[1][7] == 1);
    assert(m.regs[0x30 / 4] == 0); /* Controller delivery remains disabled. */
}
static void identity(uint64_t duration)
{
    struct aic8800_identity_admission a = { .record = 1, .address = 0x40500058, .value = 0x40,
        .awake = 1, .no_outstanding_debug_operations = 1 };
    a.record = 0; assert(!aic8800_transport_read_identity(&transport, &a, m.now + duration));
    a.record = 1; assert(aic8800_transport_read_identity(&transport, &a, m.now + duration));
}
static void stopped(void)
{
    assert(transport.state == AIC8800_TRANSPORT_FAULT && host.state == SUNXI_SDIO_FAULT);
    unsigned before = m.accesses;
    aic8800_transport_poll(&transport); assert(m.accesses == before);
    assert(!aic8800_transport_setup(&transport, m.now + 1000));
    assert(!aic8800_transport_consume(&transport));
}
static void good(void)
{
    begin(AIC8800_PAD_WRITE_80); ready_delay = 1; setup();
    assert(pad_writes == 2 && m.card[0][0xf1] == 0x80 && enable_reads >= 3);
    add_unrelated = 1; identity(2000);
    for (unsigned i = 0; i < 300 && !transport.held; ++i) step();
    assert(transport.held && transmissions == 1 && receive_commands == 1);
    struct aic8800_sdio_response event;
    assert(aic8800_transport_peek(&transport, &event));
    assert(event.address == 0x12345678 && event.value == 0xabcdef12);
    uint8_t saved[4096]; memcpy(saved, transport.rx, sizeof(saved));
    unsigned commands = m.command_count;
    for (unsigned i = 0; i < 5; ++i) step();
    assert(m.command_count == commands && !memcmp(saved, transport.rx, sizeof(saved)));
    assert(aic8800_transport_consume(&transport));
    for (unsigned i = 0; i < 300 && transport.state == AIC8800_TRANSPORT_IDENTITY; ++i) step();
    assert(transport.state == AIC8800_TRANSPORT_COMPLETE && transmissions == 3 && receive_commands == 3);
    assert(transport.identity.revision == 3 && transport.identity.is_h && transport.identity.subrevision == 7);
    assert(transport.identity.known_revision && credit_reads >= 5);
    assert(transport.setup_confirmation.address == 0xdeadbeef &&
        transport.setup_confirmation.value == 0x11223344 && transport.setup_confirmed);
    assert(pad_writes == 3 && m.card[0][0xf1] == 0);
}
static void errors(void)
{
    begin(AIC8800_PAD_KEEP); setup(); assert(pad_writes == 1 && m.card[0][0xf1] == 0);
    zero_credit = 1; identity(40);
    while (transport.state == AIC8800_TRANSPORT_IDENTITY) step();
    assert(transport.error == AIC8800_TRANSPORT_TIMEOUT && transmissions == 0); stopped();

    begin(AIC8800_PAD_KEEP); fail_setup = 1;
    while (transport.state == AIC8800_TRANSPORT_SETUP) step();
    assert(transport.error == AIC8800_TRANSPORT_HOST_ERROR && m.card[0][0x111] == 2);
    assert(transport.pending && host.request.command == 52); stopped();

    const int statuses[] = { 9, 0x80, 120 };
    for (unsigned i = 0; i < 3; ++i) {
        begin(AIC8800_PAD_KEEP); setup(); response_status = statuses[i]; identity(2000);
        if (i == 2) malformed = 1;
        while (transport.state == AIC8800_TRANSPORT_IDENTITY) step();
        assert(transport.error == (i == 0 ? AIC8800_TRANSPORT_RX_BOUNDS :
            i == 1 ? AIC8800_TRANSPORT_UNSUPPORTED_IRQ : AIC8800_TRANSPORT_MESSAGE_ERROR));
        assert(transport.raw_rx_status == statuses[i]);
        assert(receive_commands == (i == 2 ? 1u : 0u)); stopped();
    }
    begin(AIC8800_PAD_KEEP); setup(); m.fail_fifo = 1; identity(2000);
    while (transport.state == AIC8800_TRANSPORT_IDENTITY) step();
    assert(transport.error == AIC8800_TRANSPORT_HOST_ERROR && transport.last_result.transferred == 4);
    assert(host.request.data == transport.rx && transport.rx[0] == 20 && transport.pending); stopped();

    begin(AIC8800_PAD_KEEP); setup(); add_unrelated = 1; identity(2000);
    while (!transport.held && transport.state == AIC8800_TRANSPORT_IDENTITY) step();
    assert(transport.held); m.now = transport.deadline; step();
    struct aic8800_sdio_response retained;
    assert(aic8800_transport_peek(&transport, &retained) && retained.address == 0x12345678);
    assert(transport.error == AIC8800_TRANSPORT_TIMEOUT); stopped();
}
static void bootstrap_failures(void)
{
    uint8_t bytes[28], original[28]; memset(bytes, 0xa5, sizeof(bytes));
    memcpy(original, bytes, sizeof(bytes));
    assert(aic8800_sdio_encode_bootstrap_setup(bytes, 23) == AIC8800_CODEC_TRUNCATED);
    assert(!memcmp(bytes, original, sizeof(bytes)));
    assert(aic8800_sdio_encode_bootstrap_setup(bytes, sizeof(bytes)) == AIC8800_CODEC_OK);
    assert(!memcmp(bytes + 24, original + 24, 4));
    struct aic8800_sdio_response out, before;
    memset(&out, 0xa5, sizeof(out)); before = out;
    memset(bytes, 0, sizeof(bytes)); confirmation(bytes, 9, 10); bytes[4] = 3;
    bytes[10] = 7;
    assert(aic8800_sdio_decode_bootstrap_setup_response(bytes, 24, &out) == AIC8800_CODEC_MALFORMED);
    assert(!memcmp(&out, &before, sizeof(out)));
    bytes[10] = 8;
    assert(aic8800_sdio_decode_bootstrap_setup_response(bytes, 23, &out) == AIC8800_CODEC_TRUNCATED);
    assert(!memcmp(&out, &before, sizeof(out)));

    begin(AIC8800_PAD_KEEP); setup(); drop_setup_confirmation = 1; identity(150);
    while (transport.state == AIC8800_TRANSPORT_IDENTITY) step();
    assert(transport.error == AIC8800_TRANSPORT_TIMEOUT && setup_write_applied);
    assert(transmissions == 1 && !transport.setup_confirmed && receive_commands == 0); stopped();

    begin(AIC8800_PAD_WRITE_80); setup(); fail_pad_restore = 1; identity(2000);
    while (transport.state == AIC8800_TRANSPORT_IDENTITY) step();
    assert(transport.error == AIC8800_TRANSPORT_HOST_ERROR && setup_write_applied);
    assert(transmissions == 1 && transport.setup_confirmed && m.card[0][0xf1] == 0);
    assert(host.request.command == 52 && transport.pending); stopped();

    begin(AIC8800_PAD_KEEP); setup(); fail_tx = 1; identity(2000);
    while (transport.state == AIC8800_TRANSPORT_IDENTITY) step();
    assert(transport.error == AIC8800_TRANSPORT_HOST_ERROR && !transport.setup_confirmed);
    assert(transport.last_result.transferred == 4 && host.request.data == transport.tx);
    assert(transmissions == 0 && transport.pending); stopped();
}
#ifndef AIC8800_TRANSPORT_FIXTURE_MAIN
#define AIC8800_TRANSPORT_FIXTURE_MAIN main
#endif
int AIC8800_TRANSPORT_FIXTURE_MAIN(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "bootstrap")) {
        good(); bootstrap_failures();
        puts("AIC SDIO bootstrap: fixed write, non-echo confirmation, pad restore, identity and retained uncertainty passed");
    } else {
        assert(argc == 1); good(); errors(); bootstrap_failures();
        puts("AIC SDIO transport: setup, identity, retained unrelated messages, bounds, deadlines and uncertain writes passed");
    }
    return 0;
}
