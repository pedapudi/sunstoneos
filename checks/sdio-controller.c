#include "sdio_card.h"
#include "aic8800_sdio_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    uint32_t regs[256], r1_status;
    uint8_t card[8][0x20000];
    uint8_t transmitted[4096];
    uint64_t now;
    unsigned accesses, words, ready_requests, command_count;
    unsigned bytes, position, status_reads, module_divisor, module_calls;
    int write, active, irq_level, allow_irq_ack, stall_fifo, hold_data_over;
    int no_cmd_done, hold_reset, r5_error, late_completion, fail_fifo, report_level, fail_module;
};
static struct model m;
static struct sunxi_sdio host;
static struct sdio_card card;

static int now_cb(void *context, uint64_t *out)
{
    struct model *x = context; ++x->accesses; *out = x->now; return 1;
}
static int barrier_cb(void *context) { ++((struct model *)context)->accesses; return 1; }
static int module_cb(void *context, unsigned divisor)
{
    struct model *x = context; ++x->accesses; ++x->module_calls;
    assert(x->regs[0x04 / 4] == 0 && !(x->regs[0x18 / 4] & (1u << 31))); /* Card clock stopped. */
    x->module_divisor = divisor; return !x->fail_module;
}
static int read_cb(void *context, uint32_t offset, uint32_t *out)
{
    struct model *x = context; ++x->accesses; assert(offset < 0x400 && !(offset & 3));
    if (offset == 0 && !x->hold_reset) x->regs[0] &= ~7u;
    if (offset == 0x38 && x->irq_level) x->regs[offset / 4] |= 1u << 16;
    if (offset == 0x3c) {
        ++x->status_reads;
        *out = x->active && !x->write && (x->stall_fifo || x->position == x->bytes) ? 4u : 0;
        if (x->active && x->write && x->stall_fifo) *out |= 8u;
        /* Optional fill level: all remaining read words are already buffered. */
        if (x->report_level && x->active && !x->write && !x->stall_fifo)
            *out |= ((x->bytes - x->position) / 4) << 17;
        return 1;
    }
    if (offset == 0x200) {
        if (x->fail_fifo && x->position == 4) return 0;
        assert(x->active && !x->write && !x->stall_fifo && x->position + 4 <= x->bytes);
        *out = 0;
        for (unsigned i = 0; i < 4; ++i) *out |= (uint32_t)(uint8_t)(x->position + i) << (8 * i);
        x->position += 4; ++x->words;
        if (x->position == x->bytes && !x->hold_data_over) x->regs[0x38 / 4] |= 8;
        return 1;
    }
    *out = x->regs[offset / 4]; return 1;
}
static int write_cb(void *context, uint32_t offset, uint32_t value)
{
    struct model *x = context; ++x->accesses; assert(offset < 0x400 && !(offset & 3));
    assert(offset != 0x78); /* eMMC reset must not be toggled for this radio. */
    if (offset == 0x38) {
        if (value & (1u << 16)) assert(x->allow_irq_ack);
        x->regs[offset / 4] &= ~value;
        if (x->late_completion && value != 0xffffu && (value & 4)) x->now += 1000;
        return 1;
    }
    if (offset == 0x200) {
        assert(x->active && x->write && !x->stall_fifo && x->position + 4 <= x->bytes);
        for (unsigned i = 0; i < 4; ++i) x->transmitted[x->position + i] = (uint8_t)(value >> (8 * i));
        x->position += 4; ++x->words;
        if (x->position == x->bytes && !x->hold_data_over) x->regs[0x38 / 4] |= 8;
        return 1;
    }
    x->regs[offset / 4] = value;
    if (offset != 0x18) return 1;
    if (value & (1u << 21)) { x->regs[offset / 4] &= ~(1u << 31); return 1; }
    assert(!(value & (1u << 12))); /* No automatic CMD12 on counted CMD53. */
    x->regs[offset / 4] &= ~(1u << 31); /* The controller has loaded the command. */
    unsigned command = value & 63;
    uint32_t arg = x->regs[0x1c / 4];
    assert(command == 3 || command == 5 || command == 7 || command == 52 || command == 53);
    ++x->command_count; x->active = command == 53;
    x->regs[0x20 / 4] = 0;
    if (!x->no_cmd_done) x->regs[0x38 / 4] |= 4;
    if (command == 5) {
        assert(!(value & (1u << 8))); /* R4 deliberately has no response CRC. */
        x->regs[0x20 / 4] = (2u << 28) | 0x00ff8000u;
        if (arg && ++x->ready_requests > 1) x->regs[0x20 / 4] |= 1u << 31;
    } else {
        assert(value & (1u << 8));
        if (command == 3) x->regs[0x20 / 4] = 0x12340000;
        else if (command == 7) { assert(arg == 0x12340000); x->regs[0x20 / 4] = x->r1_status; }
        else if (command == 52) {
            unsigned fn = (arg >> 28) & 7, addr = (arg >> 9) & 0x1ffff;
            if (arg >> 31) x->card[fn][addr] = (uint8_t)arg;
            x->regs[0x20 / 4] = x->card[fn][addr];
        } else {
            assert(((arg >> 28) & 7) == 1);
            x->bytes = x->regs[0x14 / 4]; x->position = 0; x->write = (arg >> 31) != 0;
            assert(x->bytes <= sizeof(x->transmitted));
        }
        if (x->r5_error && (command == 52 || command == 53)) x->regs[0x20 / 4] |= 1u << 8;
    }
    return 1;
}
static void cis(unsigned function, int identity)
{
    unsigned address = 0x1000 + function * 0x100;
    for (unsigned i = 0; i < 3; ++i) m.card[0][function * 256 + 9 + i] = (uint8_t)(address >> (8 * i));
    m.card[0][function * 256] = 7;
    uint8_t *p = m.card[0] + address;
    if (identity) {
        *p++ = 0x20; *p++ = 4; *p++ = 0xa1; *p++ = 0xc8; *p++ = 0x82; *p++ = 1;
    }
    *p++ = 0; /* NULL tuple. */
    *p++ = 0x80; *p++ = 2; *p++ = 0x67; *p++ = 0x89;
    *p++ = 0x22;
    if (function) {
        *p++ = 42; p[0] = 1; p[12] = 0; p[13] = 2; p[28] = 50; p += 42;
    } else {
        *p++ = 4; *p++ = 0; *p++ = 0; *p++ = 2; *p++ = 0x32;
    }
    *p = 0xff;
}
static void reset_model(void)
{
    /* Destroy the entire simulated device between independent cases. */
    memset(&host, 0, sizeof(host)); memset(&card, 0, sizeof(card));
    memset(&m, 0, sizeof(m));
    m.now = 100; m.regs[0x300 / 4] = 0x03050530;
    m.regs[0x5c / 4] = 0x81710000;
    cis(0, 1); cis(1, 0); cis(2, 1);
    m.card[0][0] = 0x43; m.card[0][1] = 3; m.card[0][8] = 2;
    struct sunxi_sdio_config cfg = { .ops = {read_cb, write_cb, barrier_cb, now_cb, module_cb},
        .context = &m, .module_hz = 750000 };
    assert(sunxi_sdio_init(&host, &cfg));
}
static void tick_host(void)
{
    ++m.now; unsigned before = m.words;
    sunxi_sdio_poll(&host);
    assert(m.words - before <= (host.wide ? SUNXI_SDIO_FIFO_BUDGET_WIDE : SUNXI_SDIO_FIFO_BUDGET));
}
static void prepare(void)
{
    assert(sunxi_sdio_prepare(&host, m.now + 100));
    for (unsigned i = 0; i < 20 && host.state == SUNXI_SDIO_PREPARING; ++i) tick_host();
    assert(host.state == SUNXI_SDIO_READY && host.card_hz == 375000);
    assert(m.regs[0x0c / 4] == 0 && m.regs[0x04 / 4] == 1u << 16);
    assert(!(m.regs[0x5c / 4] & (1u << 31)));
}
static void start_card(void)
{
    struct sdio_card_config cfg = { .supported_ocr = 0x00ff8000, .command_timeout = 100,
                                    .ready_poll_interval = 10 };
    assert(sdio_card_init(&card, &host, &cfg)); assert(sdio_card_start(&card, m.now + 5000));
}
static void enumerate(void)
{
    start_card();
    for (unsigned i = 0; i < 2000 && card.state == SDIO_CARD_ENUMERATING; ++i) { ++m.now; sdio_card_poll(&card); }
    assert(card.state == SDIO_CARD_READY && host.state == SUNXI_SDIO_READY);
    assert(card.function_count == 2 && card.rca == 0x1234 && m.ready_requests == 2);
    for (unsigned i = 0; i <= 2; ++i) {
        assert(card.functions[i].manufacturer == 0xc8a1 && card.functions[i].device == 0x0182);
        assert(card.functions[i].max_block_bytes == 512);
    }
    assert(card.functions[1].enable_timeout_ms == 500 && card.skipped_tuples == 3);
}
static void complete_after(struct sunxi_sdio_request *request)
{
    for (unsigned i = 0; i < 90 && host.state != SUNXI_SDIO_DONE && host.state != SUNXI_SDIO_FAULT; ++i) tick_host();
    assert(host.state == SUNXI_SDIO_DONE);
    struct sunxi_sdio_result result;
    assert(sunxi_sdio_take_result(&host, &result));
    assert(!result.error && result.transferred == request->bytes);
}
static void complete(struct sunxi_sdio_request *request)
{
    assert(sunxi_sdio_submit(&host, request, m.now + 100));
    complete_after(request);
}
static void success(void)
{
    reset_model(); prepare(); enumerate();
    struct sunxi_sdio_request request;
    assert(sdio_card_cmd52(&card, 1, 0x40, 1, 1, 0x7a, &request)); complete(&request);
    assert(host.result.response == 0x7a && m.card[1][0x40] == 0x7a);
    uint8_t bytes[1026]; memset(bytes, 0x56, sizeof(bytes));
    assert(aic8800_sdio_encode_identity_read(AIC8800_IDENTITY_CHIP, bytes + 1, 20) == AIC8800_CODEC_OK);
    assert(sdio_card_cmd53(&card, 1, 0x10, 1, 0, 0, bytes + 1, 20, &request));
    assert(request.argument == 0x90002014); complete(&request);
    assert(memcmp(m.transmitted, bytes + 1, 20) == 0);
    assert(sdio_card_cmd53(&card, 1, 0x0f, 0, 0, 0, bytes + 1, 512, &request));
    assert((request.argument & 511) == 0); complete(&request);
    for (unsigned i = 0; i < 512; ++i) assert(bytes[i + 1] == (uint8_t)i);
    assert(bytes[0] == 0x56 && bytes[513] == 0x56);
    assert(sdio_card_cmd53(&card, 1, 0x10, 1, 0, 512, bytes + 1, 1024, &request));
    assert(request.argument == 0x98002002);
    m.irq_level = 1; complete(&request); assert(host.sdio_pending);
    m.allow_irq_ack = 1;
    assert(sunxi_sdio_ack_irq(&host) && host.sdio_pending);
    m.irq_level = 0; assert(sunxi_sdio_ack_irq(&host) && !host.sdio_pending);
    unsigned before = m.accesses;
    struct sunxi_sdio_request snapshot = request;
    assert(!sdio_card_cmd53(&card, 1, 0x1ffff, 0, 1, 0, bytes, 4, &request));
    assert(!sdio_card_cmd53(&card, 1, 0x10, 0, 0, 0, bytes, 3, &request));
    assert(!sdio_card_cmd52(&card, 1, 0x20, 0, 1, 0, &request));
    assert(memcmp(&snapshot, &request, sizeof(request)) == 0 && before == m.accesses);
}
static void retained(void)
{
    assert(host.state == SUNXI_SDIO_FAULT);
    unsigned before = m.accesses;
    struct sunxi_sdio_result result;
    assert(sunxi_sdio_take_result(&host, &result) && result.error);
    sunxi_sdio_poll(&host); assert(!sunxi_sdio_ack_irq(&host));
    uint64_t now; assert(!sunxi_sdio_time(&host, &now));
    assert(m.accesses == before && host.state == SUNXI_SDIO_FAULT);
}
static void faults(void)
{
    reset_model(); m.regs[0x300 / 4] = 0x040000;
    assert(sunxi_sdio_prepare(&host, m.now + 100)); tick_host();
    assert(host.result.error == SUNXI_SDIO_VARIANT_ERROR); retained();
    reset_model(); m.hold_reset = 1;
    assert(sunxi_sdio_prepare(&host, m.now + 3));
    tick_host(); tick_host(); tick_host(); assert(host.result.error == SUNXI_SDIO_TIMEOUT); retained();
    reset_model(); prepare(); --m.now; sunxi_sdio_poll(&host);
    assert(host.result.error == SUNXI_SDIO_CLOCK_ERROR); retained();

    reset_model(); prepare();
    struct sunxi_sdio_request request = { .command = 52 };
    m.r5_error = 1; assert(sunxi_sdio_submit(&host, &request, m.now + 100)); tick_host(); tick_host();
    assert(host.result.error == SUNXI_SDIO_RESPONSE_ERROR); retained();

    reset_model(); prepare();
    uint8_t data[64]; memset(data, 0xa5, sizeof(data));
    request = (struct sunxi_sdio_request){ .command = 53, .argument = 0x10001e40,
                                         .data = data, .bytes = sizeof(data) };
    m.hold_data_over = 1;
    assert(sunxi_sdio_submit(&host, &request, m.now + 8));
    for (unsigned i = 0; i < 8; ++i) tick_host();
    assert(host.result.transferred == 64 && host.result.error == SUNXI_SDIO_TIMEOUT);
    assert(host.request.data == data); retained(); /* FIFO progress alone is not completion. */

    reset_model(); prepare(); m.no_cmd_done = 1;
    assert(sunxi_sdio_submit(&host, &request, m.now + 8));
    for (unsigned i = 0; i < 8; ++i) tick_host();
    assert(host.result.transferred == 64 && (host.result.interrupts & 8));
    assert(host.result.error == SUNXI_SDIO_TIMEOUT); retained();

    reset_model(); prepare(); m.late_completion = 1;
    request = (struct sunxi_sdio_request){ .command = 52 };
    assert(sunxi_sdio_submit(&host, &request, m.now + 100)); tick_host(); tick_host();
    assert(host.result.error == SUNXI_SDIO_TIMEOUT); retained();

    reset_model(); prepare(); m.card[0][9] = 0; m.card[0][10] = 0; m.card[0][11] = 2;
    start_card();
    for (unsigned i = 0; i < 100 && card.state == SDIO_CARD_ENUMERATING; ++i) { ++m.now; sdio_card_poll(&card); }
    assert(card.state == SDIO_CARD_FAULT && card.functions[0].cis_address == 0x20000);
    assert(host.result.error == SUNXI_SDIO_CARD_ERROR); retained();
}
static void select_and_io_fault(void)
{
    reset_model(); prepare();
    struct sunxi_sdio_request request = { .command = 7, .argument = 0x12340000 };
    m.r1_status = (4u << 9) | (1u << 8) | (1u << 14) | (1u << 3);
    complete(&request); /* Informational and source-unspecified status bits. */
    m.r1_status = 1u << 22; /* ILLEGAL_COMMAND remains an error. */
    assert(sunxi_sdio_submit(&host, &request, m.now + 100)); tick_host(); tick_host();
    assert(host.result.error == SUNXI_SDIO_RESPONSE_ERROR); retained();
    reset_model(); prepare(); m.fail_fifo = 1;
    uint8_t bytes[8]; memset(bytes, 0x5a, sizeof(bytes));
    request = (struct sunxi_sdio_request){ .command = 53, .argument = 0x10001e08,
        .bytes = sizeof(bytes), .data = bytes };
    assert(sunxi_sdio_submit(&host, &request, m.now + 100));
    tick_host(); tick_host(); tick_host();
    assert(host.result.error == SUNXI_SDIO_IO_ERROR && host.result.transferred == 4);
    assert(bytes[0] == 0 && bytes[3] == 3 && bytes[4] == 0x5a);
    assert(host.request.data == bytes); retained();
}
/* Four-bit default speed after the owner's CCCR write; level-sized bursts. */
static void widen(void)
{
    reset_model(); prepare(); enumerate();
    assert(!sunxi_sdio_widen(&host, 7, m.now + 100) && !sunxi_sdio_widen(&host, 33, m.now + 100));
    assert(sunxi_sdio_widen(&host, 8, m.now + 100) && !sunxi_sdio_widen(&host, 8, m.now + 100));
    for (unsigned i = 0; i < 20 && host.state == SUNXI_SDIO_WIDENING; ++i) tick_host();
    assert(host.state == SUNXI_SDIO_READY && host.wide && host.card_hz == 25000000 && !host.deadline);
    assert(m.module_calls == 1 && m.module_divisor == 8 && m.regs[0x0c / 4] == 1);
    assert(m.regs[0x04 / 4] == 1u << 16 && ((m.regs[0x140 / 4] >> 16) & 3) == 1);
    assert(!(m.regs[0x5c / 4] & 0x80000330u) && !sunxi_sdio_widen(&host, 8, m.now + 100));
    uint8_t bytes[1024]; struct sunxi_sdio_request request;
    assert(sdio_card_cmd53(&card, 1, 0x0f, 0, 0, 512, bytes, sizeof(bytes), &request));
    m.report_level = 1; unsigned reads = m.status_reads;
    assert(sunxi_sdio_submit(&host, &request, m.now + 100));
    while (host.state == SUNXI_SDIO_QUEUED) tick_host(); /* FIFO reset, then issue. */
    reads = m.status_reads; tick_host(); /* Response plus one level-sized 128-word burst. */
    assert(m.words == 128 && m.status_reads - reads == 1);
    complete_after(&request);
    for (unsigned i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == (uint8_t)i);
    /* An uncertain module change is terminal and retains the stopped controller. */
    reset_model(); prepare(); m.fail_module = 1;
    assert(sunxi_sdio_widen(&host, 8, m.now + 100));
    for (unsigned i = 0; i < 4 && host.state == SUNXI_SDIO_WIDENING; ++i) tick_host();
    assert(host.result.error == SUNXI_SDIO_IO_ERROR && !host.wide && host.card_hz == 375000); retained();
}
static void cis_shape(void)
{
    const uint8_t payloads[][3] = {{0x22, 0, 0}, {0x22, 1, 1}, {0x22, 1, 4}};
    for (unsigned n = 0; n < sizeof(payloads) / sizeof(payloads[0]); ++n) {
        reset_model(); prepare(); memcpy(m.card[0] + 0x1000, payloads[n], 3);
        start_card();
        for (unsigned i = 0; i < 100 && card.state == SDIO_CARD_ENUMERATING; ++i) {
            ++m.now; sdio_card_poll(&card);
        }
        assert(card.state == SDIO_CARD_FAULT && card.function == 0);
        assert(host.result.error == SUNXI_SDIO_CARD_ERROR); retained();
    }
}
static void reinit_refusal(void)
{
    reset_model(); prepare(); start_card();
    ++m.now; sdio_card_poll(&card);
    assert(card.pending && host.state == SUNXI_SDIO_QUEUED);
    tick_host(); assert(host.state == SUNXI_SDIO_ACTIVE);
    struct sunxi_sdio_config hc = host.config;
    struct sdio_card_config cc = card.config;
    for (unsigned fault = 0; fault < 2; ++fault) {
        struct sunxi_sdio hs = host;
        struct sdio_card cs = card;
        struct model ms = m;
        assert(!sunxi_sdio_init(&host, &hc));
        assert(!sdio_card_init(&card, &host, &cc));
        assert(memcmp(&hs, &host, sizeof(host)) == 0);
        assert(memcmp(&cs, &card, sizeof(card)) == 0);
        assert(memcmp(&ms, &m, sizeof(m)) == 0);
        if (!fault) {
            sunxi_sdio_stop(&host, SUNXI_SDIO_IO_ERROR);
            ++m.now; sdio_card_poll(&card);
            assert(host.state == SUNXI_SDIO_FAULT && card.state == SDIO_CARD_FAULT);
        }
    }
}
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "reinit") == 0) {
        reinit_refusal(); puts("SDIO model: active and fault reinitialization refused without mutation or I/O");
    } else if (argc == 2 && strcmp(argv[1], "select-io") == 0) {
        select_and_io_fault(); puts("SDIO model: CMD7 status and partial FIFO I/O failure passed");
    } else if (argc == 2 && strcmp(argv[1], "widen") == 0) {
        widen(); puts("SDIO model: four-bit widening, level-sized bursts and module failure passed");
    } else if (argc == 2 && strcmp(argv[1], "cis-shape") == 0) {
        cis_shape(); puts("SDIO model: malformed function-extension tuple shapes rejected");
    } else {
        assert(argc == 1);
        success(); faults(); select_and_io_fault(); cis_shape(); reinit_refusal(); widen();
        puts("SDIO model: enumeration, identity transfer, byte/block PIO, IRQ ownership, four-bit widening and retained faults passed");
    }
    return 0;
}
