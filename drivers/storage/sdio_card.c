/* SDIO protocol facts follow the pinned kernel references in docs/sdio.md.
 * Native state machine; no Linux allocation, retry, or device-model dependency. */
#include "sdio_card.h"

enum { PROBE, READY_POLL, RCA, SELECT, CCCR, FBR, FBR_EXT, CIS_POINTER,
       TUPLE_CODE, TUPLE_LENGTH, TUPLE_DATA };
static int span(const void *p, size_t n) { return p && n <= UINTPTR_MAX - (uintptr_t)p; }
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    return (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an;
}
static int output_ok(const struct sdio_card *c, const struct sunxi_sdio_request *r)
{
    return c && c->host && span(r, sizeof(*r)) &&
           !overlap(c, sizeof(*c), r, sizeof(*r)) &&
           !overlap(c->host, sizeof(*c->host), r, sizeof(*r));
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
static struct sunxi_sdio_request direct(unsigned function, uint32_t address,
                                       int write, int raw, uint8_t value)
{
    return (struct sunxi_sdio_request){ .command = 52, .argument =
        (write ? 1u << 31 : 0) | (function << 28) | (raw ? 1u << 27 : 0) |
        (address << 9) | value };
}
int sdio_card_init(struct sdio_card *c, struct sunxi_sdio *h, const struct sdio_card_config *cfg)
{
    if (!span(c, sizeof(*c)) || !span(h, sizeof(*h)) || !span(cfg, sizeof(*cfg)) ||
        overlap(c, sizeof(*c), h, sizeof(*h)) || overlap(c, sizeof(*c), cfg, sizeof(*cfg)) ||
        overlap(h, sizeof(*h), cfg, sizeof(*cfg)) || !cfg->supported_ocr ||
        (cfg->supported_ocr & 0xff000000u) || !cfg->command_timeout || !cfg->ready_poll_interval ||
        c->initialized || !h->initialized) return 0;
    struct sdio_card initial = {0}; initial.initialized = 1; initial.host = h; initial.config = *cfg;
    *c = initial; return 1;
}
int sdio_card_start(struct sdio_card *c, uint64_t deadline)
{
    if (!c || c->entered || c->state != SDIO_CARD_NEW || !c->host ||
        c->host->state != SUNXI_SDIO_READY || deadline <= c->host->last_now) return 0;
    c->deadline = deadline; c->phase = PROBE; c->state = SDIO_CARD_ENUMERATING; return 1;
}
static void fail(struct sdio_card *c, enum sunxi_sdio_error error)
{
    sunxi_sdio_stop(c->host, error); c->state = SDIO_CARD_FAULT;
    c->last_result = c->host->result;
}
static int observe(struct sdio_card *c, uint64_t *now)
{
    if (!sunxi_sdio_time(c->host, now)) { c->state = SDIO_CARD_FAULT; c->last_result = c->host->result; return 0; }
    if (*now >= c->deadline) { fail(c, SUNXI_SDIO_TIMEOUT); return 0; }
    return 1;
}
static void next_function(struct sdio_card *c)
{
    if (c->function != 0 && !c->functions[c->function].has_manufacturer) {
        if (!c->functions[0].has_manufacturer) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
        c->functions[c->function].manufacturer = c->functions[0].manufacturer;
        c->functions[c->function].device = c->functions[0].device;
        c->functions[c->function].has_manufacturer = 1;
    }
    if (c->function == c->function_count) { c->state = SDIO_CARD_READY; return; }
    ++c->function; c->phase = FBR; c->index = 0;
}
static void tuple_done(struct sdio_card *c)
{
    struct sdio_function_identity *f = &c->functions[c->function];
    if (c->tuple_code == 0x20) {
        if (c->tuple_length < 4) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
        uint16_t manufacturer = le16(c->tuple), device = le16(c->tuple + 2);
        if (f->has_manufacturer && (f->manufacturer != manufacturer || f->device != device)) {
            fail(c, SUNXI_SDIO_CARD_ERROR); return;
        }
        f->manufacturer = manufacturer; f->device = device; f->has_manufacturer = 1;
    } else if (c->tuple_code == 0x22) {
        if (!c->tuple_length) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
        unsigned subtype = c->tuple[0];
        if (subtype == 0 || subtype == 1) {
            if (subtype != (c->function ? 1u : 0u)) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
            unsigned minimum = c->function ? (c->sdio_revision == 0 ? 28u : 42u) : 4u;
            if (c->tuple_length < minimum) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
            f->max_block_bytes = le16(c->tuple + (c->function ? 12 : 1));
            if (c->function)
                f->enable_timeout_ms = c->sdio_revision == 0 ? 1000u : (unsigned)le16(c->tuple + 28) * 10u;
            f->has_function_extension = 1;
        } else {
            if (subtype == 4 && c->tuple_length < 8) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
            ++c->skipped_tuples;
        }
    } else ++c->skipped_tuples;
    c->phase = TUPLE_CODE; c->index = 0;
}
static void response(struct sdio_card *c, uint64_t now)
{
    uint32_t value = c->last_result.response;
    uint8_t byte = (uint8_t)value;
    switch (c->phase) {
    case PROBE:
        c->ocr = value; c->function_count = (uint8_t)((value >> 28) & 7);
        c->selected_ocr = value & c->config.supported_ocr & 0x00ffffffu;
        /* This bounded path supports an I/O-only card, without memory commands. */
        if (!c->function_count || (value & (1u << 27)) || !c->selected_ocr) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
        c->phase = READY_POLL; return;
    case READY_POLL:
        if (((value >> 28) & 7) != c->function_count || (value & (1u << 27)) ||
            !(value & c->selected_ocr)) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
        c->ocr = value;
        if (value & (1u << 31)) c->phase = RCA;
        else {
            if (c->config.ready_poll_interval > c->deadline - now) c->retry_at = c->deadline;
            else c->retry_at = now + c->config.ready_poll_interval;
        }
        return;
    case RCA:
        c->rca = (uint16_t)(value >> 16);
        if (!c->rca) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
        c->phase = SELECT; return;
    case SELECT: c->phase = CCCR; c->index = 0; return;
    case CCCR:
        if (c->index == 0) {
            c->cccr_revision = byte & 15; c->sdio_revision = byte >> 4;
            if (c->cccr_revision > 3 || c->sdio_revision > 4) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
        } else if (c->index == 1) c->sd_revision = byte;
        else c->capabilities = byte;
        if (++c->index == 3) { c->phase = CIS_POINTER; c->index = 0; }
        return;
    case FBR:
        c->functions[c->function].class_code = byte & 15;
        c->phase = (byte & 15) == 15 ? FBR_EXT : CIS_POINTER;
        c->index = 0; return;
    case FBR_EXT:
        c->functions[c->function].class_code = byte; c->phase = CIS_POINTER; c->index = 0; return;
    case CIS_POINTER:
        c->functions[c->function].cis_address |= (uint32_t)byte << (8 * c->index);
        if (++c->index == 3) {
            c->cursor = c->functions[c->function].cis_address;
            if (!c->cursor || c->cursor > 0x1ffffu) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
            c->cis_bytes = 0; c->tuple_count = 0; c->index = 0; c->phase = TUPLE_CODE;
        }
        return;
    case TUPLE_CODE:
        if (++c->tuple_count > SDIO_CARD_CIS_TUPLES) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
        if (byte == 0xff) { next_function(c); return; }
        if (byte == 0) return; /* NULL tuple has no length byte. */
        c->tuple_code = byte; c->phase = TUPLE_LENGTH; return;
    case TUPLE_LENGTH:
        if (byte == 0xff) { next_function(c); return; }
        c->tuple_length = byte; c->index = 0;
        if (byte > SDIO_CARD_CIS_BYTES - c->cis_bytes ||
            byte > 0x20000u - c->cursor) { fail(c, SUNXI_SDIO_CARD_ERROR); return; }
        if (!byte) tuple_done(c);
        else c->phase = TUPLE_DATA;
        return;
    default:
        c->tuple[c->index++] = byte;
        if (c->index == c->tuple_length) tuple_done(c);
        return;
    }
}
static int admit_next(struct sdio_card *c, uint64_t now)
{
    struct sunxi_sdio_request request = {0};
    uint32_t address = 0;
    switch (c->phase) {
    case PROBE: request.command = 5; break;
    case READY_POLL:
        if (now < c->retry_at) return 1;
        request.command = 5; request.argument = c->selected_ocr; break;
    case RCA: request.command = 3; break;
    case SELECT: request.command = 7; request.argument = (uint32_t)c->rca << 16; break;
    case CCCR: {
        const uint8_t offsets[3] = {0, 1, 8}; address = offsets[c->index]; break;
    }
    case FBR: address = (uint32_t)c->function << 8; break;
    case FBR_EXT: address = ((uint32_t)c->function << 8) + 1; break;
    case CIS_POINTER: address = ((uint32_t)c->function << 8) + 9 + c->index; break;
    default:
        if (c->cursor > 0x1ffffu || c->cis_bytes >= SDIO_CARD_CIS_BYTES ||
            (c->phase == TUPLE_CODE && c->tuple_count >= SDIO_CARD_CIS_TUPLES)) return 0;
        address = c->cursor; break;
    }
    if (!request.command) request = direct(0, address, 0, 0, 0);
    uint64_t deadline = c->deadline;
    if (c->config.command_timeout < deadline - now) deadline = now + c->config.command_timeout;
    if (!sunxi_sdio_submit(c->host, &request, deadline)) return 0;
    if (c->phase >= TUPLE_CODE) { ++c->cursor; ++c->cis_bytes; }
    c->pending = 1; return 1;
}
void sdio_card_poll(struct sdio_card *c)
{
    uint64_t now;
    if (!c || c->entered || c->state != SDIO_CARD_ENUMERATING) return;
    c->entered = 1;
    if (!observe(c, &now)) goto out;
    if (c->pending) {
        sunxi_sdio_poll(c->host);
        if (c->host->state == SUNXI_SDIO_FAULT) { c->state = SDIO_CARD_FAULT; c->last_result = c->host->result; goto out; }
        if (c->host->state != SUNXI_SDIO_DONE) goto post;
        if (!sunxi_sdio_take_result(c->host, &c->last_result)) { fail(c, SUNXI_SDIO_CARD_ERROR); goto out; }
        c->pending = 0;
        if (!observe(c, &now)) goto out;
        response(c, now);
    }
    if (c->state == SDIO_CARD_ENUMERATING && !c->pending &&
        !admit_next(c, now)) fail(c, SUNXI_SDIO_CARD_ERROR);
post:
    if (c->state != SDIO_CARD_FAULT) (void)observe(c, &now);
out:
    c->entered = 0;
}
int sdio_card_cmd52(const struct sdio_card *c, unsigned function, uint32_t address,
                   int write, int raw, uint8_t value, struct sunxi_sdio_request *out)
{
    if (!output_ok(c, out) || c->state != SDIO_CARD_READY || c->host->state != SUNXI_SDIO_READY ||
        function > c->function_count || address > 0x1ffffu || (write != 0 && write != 1) ||
        (raw != 0 && raw != 1) || (raw && !write)) return 0;
    *out = direct(function, address, write, raw, value); return 1;
}
int sdio_card_cmd53(const struct sdio_card *c, unsigned function, uint32_t address,
                   int write, int increment, uint16_t block_bytes,
                   uint8_t *data, size_t bytes, struct sunxi_sdio_request *out)
{
    if (!output_ok(c, out) || c->state != SDIO_CARD_READY || c->host->state != SUNXI_SDIO_READY ||
        !function || function > c->function_count || address > 0x1ffffu ||
        (write != 0 && write != 1) || (increment != 0 && increment != 1) ||
        !span(data, bytes) || !bytes || bytes > SUNXI_SDIO_MAX_BYTES || (bytes & 3) ||
        overlap(c, sizeof(*c), data, bytes) || overlap(c->host, sizeof(*c->host), data, bytes) ||
        overlap(out, sizeof(*out), data, bytes) || (increment && bytes - 1 > 0x1ffffu - address)) return 0;
    uint32_t count;
    if (block_bytes) {
        if (block_bytes < 4 || block_bytes > 512 || (block_bytes & 3) || bytes % block_bytes ||
            block_bytes > c->functions[function].max_block_bytes || !(c->capabilities & 2)) return 0;
        count = (uint32_t)(bytes / block_bytes);
        if (!count || count > 511) return 0;
    } else {
        if (bytes > 512) return 0;
        count = bytes == 512 ? 0 : (uint32_t)bytes;
    }
    *out = (struct sunxi_sdio_request){ .command = 53, .argument = (write ? 1u << 31 : 0) |
        (function << 28) | (block_bytes ? 1u << 27 : 0) | (increment ? 1u << 26 : 0) |
        (address << 9) | count, .block_bytes = block_bytes, .data = data, .bytes = bytes };
    return 1;
}
