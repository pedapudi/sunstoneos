/* Native bounded worker. Published register and protocol references are pinned
 * in docs/radio-transport.md; no Linux driver code or allocation is used. */
#include "aic8800_sdio_transport.h"

enum { BLOCK_LO_READ, BLOCK_HI_READ, BLOCK_LO_WRITE, BLOCK_HI_WRITE,
    BLOCK_LO_VERIFY, BLOCK_HI_VERIFY, ENABLE_READ, ENABLE_WRITE, ENABLE_READY,
    PAD_F2, PAD_F1, SETTLE, BYTE_MODE, IRQ_READ, IRQ_WRITE,
    FUNCTION_IRQ_READ, FUNCTION_IRQ_WRITE, CREDIT, TX, RX_STATUS, RX_LENGTH,
    RX, PARSE };
static int span(const void *p, size_t n) { return p && n <= UINTPTR_MAX - (uintptr_t)p; }
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    return (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an;
}
static int independent(const struct aic8800_sdio_transport *t, const void *p, size_t n)
{
    return span(p, n) && !overlap(t, sizeof(*t), p, n) &&
        !overlap(t->card, sizeof(*t->card), p, n) &&
        !overlap(t->card->host, sizeof(*t->card->host), p, n);
}
static void fail(struct aic8800_sdio_transport *t, enum aic8800_transport_error e)
{
    if (t->state == AIC8800_TRANSPORT_FAULT) return;
    t->error = e; t->state = AIC8800_TRANSPORT_FAULT;
    sunxi_sdio_stop(t->card->host, e == AIC8800_TRANSPORT_TIMEOUT ?
                    SUNXI_SDIO_TIMEOUT : SUNXI_SDIO_CARD_ERROR);
    t->last_result = t->card->host->result;
}
static int observe(struct aic8800_sdio_transport *t, uint64_t *now)
{
    if (!sunxi_sdio_time(t->card->host, now)) { fail(t, AIC8800_TRANSPORT_HOST_ERROR); return 0; }
    if (*now >= t->deadline || (t->enable_deadline && *now >= t->enable_deadline)) {
        fail(t, AIC8800_TRANSPORT_TIMEOUT); return 0;
    }
    return 1;
}
static int later(uint64_t now, uint64_t delta, uint64_t *out)
{
    if (delta > UINT64_MAX - now) return 0;
    *out = now + delta; return 1;
}
int aic8800_transport_init(struct aic8800_sdio_transport *t, struct sdio_card *c,
                          const struct aic8800_transport_config *cfg)
{
    if (!span(t, sizeof(*t)) || !span(c, sizeof(*c)) || !span(cfg, sizeof(*cfg)) ||
        !span(c->host, sizeof(*c->host)) || overlap(t, sizeof(*t), c, sizeof(*c)) ||
        overlap(t, sizeof(*t), cfg, sizeof(*cfg)) || overlap(t, sizeof(*t), c->host, sizeof(*c->host)) ||
        overlap(c, sizeof(*c), cfg, sizeof(*cfg)) || overlap(c->host, sizeof(*c->host), cfg, sizeof(*cfg)) ||
        t->initialized || !c->initialized || !c->host->initialized || c->state != SDIO_CARD_READY ||
        c->host->state != SUNXI_SDIO_READY || c->host->entered || c->entered ||
        cfg->variant != AIC8800_TRANSPORT_D80_BOOTSTRAP ||
        (cfg->pad != AIC8800_PAD_KEEP && cfg->pad != AIC8800_PAD_WRITE_80) ||
        !cfg->ticks_per_ms || !cfg->command_timeout || !cfg->poll_interval ||
        c->function_count < 2 || !(c->capabilities & 2) ||
        !c->functions[1].has_manufacturer || c->functions[1].manufacturer != 0xc8a1 ||
        c->functions[1].device != 0x0182 || c->functions[1].max_block_bytes < 512 ||
        !c->functions[1].enable_timeout_ms ||
        cfg->ticks_per_ms > UINT64_MAX / c->functions[1].enable_timeout_ms) return 0;
    struct aic8800_sdio_transport initial = {0};
    initial.card = c; initial.config = *cfg; initial.initialized = 1; *t = initial; return 1;
}
int aic8800_transport_setup(struct aic8800_sdio_transport *t, uint64_t deadline)
{
    if (!t || !t->initialized || t->entered || t->state != AIC8800_TRANSPORT_NEW ||
        t->card->host->state != SUNXI_SDIO_READY || deadline <= t->card->host->last_now) return 0;
    t->deadline = deadline; t->phase = BLOCK_LO_READ; t->state = AIC8800_TRANSPORT_SETUP; return 1;
}
static void encode(struct aic8800_sdio_transport *t)
{
    for (size_t i = 0; i < sizeof(t->tx); ++i) t->tx[i] = 0;
    t->codec_result = aic8800_sdio_encode_identity_read(
        (enum aic8800_identity_register)t->expected, t->tx, sizeof(t->tx));
    if (t->codec_result != AIC8800_CODEC_OK) fail(t, AIC8800_TRANSPORT_MESSAGE_ERROR);
}
int aic8800_transport_read_identity(struct aic8800_sdio_transport *t,
                                  const struct aic8800_identity_admission *a, uint64_t deadline)
{
    if (!t || !t->initialized || t->entered || t->state != AIC8800_TRANSPORT_READY ||
        !independent(t, a, sizeof(*a)) || !a->record || a->address != 0x40500058 || a->value != 0x40 ||
        a->awake != 1 || a->no_outstanding_debug_reads != 1 ||
        t->card->host->state != SUNXI_SDIO_READY || deadline <= t->card->host->last_now) return 0;
    t->admission = *a; t->deadline = deadline; t->wait_until = 0;
    t->state = AIC8800_TRANSPORT_IDENTITY; t->phase = CREDIT; encode(t); return 1;
}
static int cmd52(struct aic8800_sdio_transport *t, unsigned fn, uint32_t addr, int write, uint8_t value,
                 struct sunxi_sdio_request *r)
{
    return sdio_card_cmd52(t->card, fn, addr, write, 0, value, r);
}
static int admit(struct aic8800_sdio_transport *t, uint64_t now)
{
    struct sunxi_sdio_request r = {0};
    uint64_t deadline;
    if (now < t->wait_until) return 1;
    if (!later(now, t->config.command_timeout, &deadline)) return 0;
    if (deadline > t->deadline) deadline = t->deadline;
    if (t->enable_deadline && deadline > t->enable_deadline) deadline = t->enable_deadline;
    int ok;
    switch (t->phase) {
    case BLOCK_LO_READ: case BLOCK_LO_VERIFY: ok = cmd52(t, 0, 0x110, 0, 0, &r); break;
    case BLOCK_HI_READ: case BLOCK_HI_VERIFY: ok = cmd52(t, 0, 0x111, 0, 0, &r); break;
    case BLOCK_LO_WRITE: ok = cmd52(t, 0, 0x110, 1, 0, &r); break;
    case BLOCK_HI_WRITE: ok = cmd52(t, 0, 0x111, 1, 2, &r); break;
    case ENABLE_READ: ok = cmd52(t, 0, 2, 0, 0, &r); break;
    case ENABLE_WRITE: ok = cmd52(t, 0, 2, 1, t->prior_enable | 2, &r); break;
    case ENABLE_READY: ok = cmd52(t, 0, 3, 0, 0, &r); break;
    case PAD_F2: ok = cmd52(t, 0, 0xf2, 1, 0x7f, &r); break;
    case PAD_F1: ok = cmd52(t, 0, 0xf1, 1, 0x80, &r); break;
    case SETTLE: t->phase = BYTE_MODE; return 1;
    case BYTE_MODE: ok = cmd52(t, 1, 7, 1, 1, &r); break;
    case IRQ_READ: ok = cmd52(t, 0, 4, 0, 0, &r); break;
    case IRQ_WRITE: ok = cmd52(t, 0, 4, 1, t->prior_irq | 7, &r); break;
    case FUNCTION_IRQ_READ: ok = cmd52(t, 1, 0, 0, 0, &r); break;
    case FUNCTION_IRQ_WRITE: ok = cmd52(t, 1, 0, 1, t->prior_function_irq | 7, &r); break;
    case CREDIT: ok = cmd52(t, 1, 3, 0, 0, &r); break;
    case RX_STATUS: ok = cmd52(t, 1, 4, 0, 0, &r); break;
    case RX_LENGTH: ok = cmd52(t, 1, 5, 0, 0, &r); break;
    case TX: ok = sdio_card_cmd53(t->card, 1, 0x10, 1, 0, 512, t->tx, sizeof(t->tx), &r); break;
    case RX: ok = sdio_card_cmd53(t->card, 1, 0x0f, 0, 0,
                     t->rx_bytes > 512 ? 512 : 0, t->rx, t->rx_bytes, &r); break;
    default: return 0;
    }
    if (!ok || !sunxi_sdio_submit(t->card->host, &r, deadline)) return 0;
    t->pending = 1; return 1;
}
static void delay(struct aic8800_sdio_transport *t, uint64_t now, uint64_t ticks)
{
    if (!later(now, ticks, &t->wait_until)) fail(t, AIC8800_TRANSPORT_TIMEOUT);
}
static void rx_size(struct aic8800_sdio_transport *t, size_t bytes)
{
    t->rx_bytes = bytes; t->cursor = 0;
    if (!bytes || bytes > sizeof(t->rx) || (bytes & 3) || (bytes > 512 && bytes % 512)) {
        fail(t, AIC8800_TRANSPORT_RX_BOUNDS); return;
    }
    t->phase = RX;
}
static void completed(struct aic8800_sdio_transport *t, uint64_t now)
{
    uint8_t value = (uint8_t)t->last_result.response;
    switch (t->phase) {
    case BLOCK_LO_READ: t->prior_block_bytes = value; ++t->phase; break;
    case BLOCK_HI_READ: t->prior_block_bytes |= (uint16_t)value << 8; ++t->phase; break;
    case BLOCK_LO_VERIFY: if (value) { fail(t, AIC8800_TRANSPORT_SETUP_ERROR); break; } ++t->phase; break;
    case BLOCK_HI_VERIFY: if (value != 2) { fail(t, AIC8800_TRANSPORT_SETUP_ERROR); break; } ++t->phase; break;
    case ENABLE_READ: t->prior_enable = value; ++t->phase; break;
    case ENABLE_WRITE:
        if (!later(now, t->config.ticks_per_ms * t->card->functions[1].enable_timeout_ms,
                   &t->enable_deadline)) { fail(t, AIC8800_TRANSPORT_TIMEOUT); break; }
        ++t->phase; break;
    case ENABLE_READY:
        if (value & 2) { t->enable_deadline = 0; ++t->phase; }
        else delay(t, now, t->config.poll_interval);
        break;
    case PAD_F2:
        if (t->config.pad == AIC8800_PAD_WRITE_80) ++t->phase;
        else { t->phase = SETTLE; delay(t, now, t->config.ticks_per_ms); }
        break;
    case PAD_F1: t->phase = SETTLE; delay(t, now, t->config.ticks_per_ms); break;
    case IRQ_READ: t->prior_irq = value; ++t->phase; break;
    case FUNCTION_IRQ_READ: t->prior_function_irq = value; ++t->phase; break;
    case FUNCTION_IRQ_WRITE: t->state = AIC8800_TRANSPORT_READY; break;
    case CREDIT:
        if (value && sizeof(t->tx) < (size_t)value * 1536) t->phase = TX;
        else delay(t, now, t->config.poll_interval);
        break;
    case TX: t->phase = RX_STATUS; break;
    case RX_STATUS:
        t->raw_rx_status = value;
        if (value & 0x80) { fail(t, AIC8800_TRANSPORT_UNSUPPORTED_IRQ); break; }
        if (!value) { delay(t, now, t->config.poll_interval); break; }
        if ((value | 8u) > 120u) {
            if ((value | 8u) == 127u) t->phase = RX_LENGTH;
            else rx_size(t, (value & 7u) * 512u);
        } else if (value == 120) t->phase = RX_LENGTH;
        else rx_size(t, (value & 0x7fu) * 512u);
        break;
    case RX_LENGTH: t->byte_length = value; rx_size(t, (size_t)value * 4); break;
    case RX: t->phase = PARSE; break;
    default: ++t->phase; break;
    }
}
static void aggregate_done(struct aic8800_sdio_transport *t)
{
    t->cursor = t->rx_bytes;
    if (!t->have[t->expected]) { t->phase = RX_STATUS; return; }
    if (!t->expected) { t->expected = 1; t->phase = CREDIT; encode(t); return; }
    t->codec_result = aic8800_sdio_identity(&t->confirmations[0], &t->confirmations[1], &t->identity);
    if (t->codec_result != AIC8800_CODEC_OK) fail(t, AIC8800_TRANSPORT_MESSAGE_ERROR);
    else t->state = AIC8800_TRANSPORT_COMPLETE;
}
static void parse(struct aic8800_sdio_transport *t)
{
    if (t->held) return;
    if (t->cursor == t->rx_bytes) { aggregate_done(t); return; }
    struct aic8800_sdio_response r;
    t->codec_result = aic8800_sdio_decode_identity_response(t->rx + t->cursor,
        t->rx_bytes - t->cursor, (enum aic8800_identity_register)t->expected, &r);
    if (t->codec_result == AIC8800_CODEC_END) { aggregate_done(t); return; }
    if (t->codec_result == AIC8800_CODEC_OK && !t->have[t->expected]) {
        t->confirmations[t->expected] = (struct aic8800_sdio_response){ .is_memory_read = 1,
            .address = r.address, .value = r.value };
        t->have[t->expected] = 1; t->cursor += r.consumed;
    } else if (t->codec_result == AIC8800_CODEC_OK || t->codec_result == AIC8800_CODEC_UNRELATED) {
        t->unrelated = r; t->held = 1;
    } else fail(t, AIC8800_TRANSPORT_MESSAGE_ERROR);
}
void aic8800_transport_poll(struct aic8800_sdio_transport *t)
{
    uint64_t now;
    if (!t || !t->initialized || t->entered ||
        (t->state != AIC8800_TRANSPORT_SETUP && t->state != AIC8800_TRANSPORT_IDENTITY)) return;
    t->entered = 1;
    if (!observe(t, &now)) goto out;
    if (t->pending) {
        sunxi_sdio_poll(t->card->host);
        if (t->card->host->state == SUNXI_SDIO_FAULT) { fail(t, AIC8800_TRANSPORT_HOST_ERROR); goto out; }
        if (t->card->host->state != SUNXI_SDIO_DONE) goto post;
        if (!sunxi_sdio_take_result(t->card->host, &t->last_result)) { fail(t, AIC8800_TRANSPORT_HOST_ERROR); goto out; }
        t->pending = 0;
        if (!observe(t, &now)) goto out;
        completed(t, now);
        /* Parsing a newly received aggregate starts on the next worker call. */
        if (t->phase == PARSE) goto post;
    }
    if (t->state == AIC8800_TRANSPORT_SETUP || t->state == AIC8800_TRANSPORT_IDENTITY) {
        if (t->phase == PARSE) parse(t);
        else if (!admit(t, now)) fail(t, AIC8800_TRANSPORT_HOST_ERROR);
    }
post:
    if (t->state != AIC8800_TRANSPORT_FAULT) (void)observe(t, &now);
out:
    t->entered = 0;
}
int aic8800_transport_peek(const struct aic8800_sdio_transport *t, struct aic8800_sdio_response *out)
{
    if (!t || !t->initialized || t->entered || !t->held || !independent(t, out, sizeof(*out))) return 0;
    *out = t->unrelated; return 1;
}
int aic8800_transport_consume(struct aic8800_sdio_transport *t)
{
    if (!t || !t->initialized || t->entered || !t->held || t->state == AIC8800_TRANSPORT_FAULT) return 0;
    t->cursor += t->unrelated.consumed; t->held = 0; return 1;
}
