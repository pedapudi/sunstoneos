/* AArch64 model of the RAM diagnostics USB protocol (drivers/usb/diagnostic.c).
 *
 * The actual diagnostic.c is compiled into this unit. TinyUSB's vendor
 * stream, the polled controller, the host and time are modeled: the host
 * writes requests into an OUT queue, the controller moves one 512-byte packet
 * per direction per poll, and vTaskDelay(1) advances one 1 ms tick and runs
 * the next host step. checks/usb-diagnostic-protocol/check.py runs it under
 * Unicorn, intercepts the recovery-timer SMC and reads model_report. */
#include "../../drivers/usb/diagnostic.c"

#define FIFO 4096u
#define PACKET 512u
uint8_t model_result[RESULT_SIZE + 4096] __attribute__((aligned(64)));
struct model_report {
    uint32_t done, failed_stage, failed_line, panics, exits;
    uint32_t ticks, rejected, resets, stage_ticks[16];
} model_report;

static uint8_t host_out[8192], rx[FIFO], tx[FIFO], host_in[8192];
static unsigned out_len, out_pos, rx_count, tx_count, in_len, out_event, tx_event, host_reads = 1;
static uint64_t now;
static TickType_t ticks;
static unsigned stage, stage_start, waited;

_Noreturn static void finish(void) { __asm__ volatile("brk #0x733"); for (;;) {} }
_Noreturn void platform_panic(const char *file, unsigned line) { (void)file; (void)line; ++model_report.panics; finish(); }
_Noreturn void platform_exit(unsigned status) { (void)status; ++model_report.exits; finish(); }
void platform_puts(const char *text) { (void)text; }
void platform_init(void) {}
uint64_t counter(void) { now += 24; return now; }
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskStartScheduler(void) {}
TaskHandle_t xTaskCreateStatic(TaskFunction_t entry, const char *name, uint32_t depth, void *argument,
                               UBaseType_t priority, StackType_t *stack, StaticTask_t *state)
{ (void)entry; (void)name; (void)depth; (void)argument; (void)priority; (void)stack; return state; }
void a733_usb_trace(uint32_t event, uint32_t first, uint32_t second)
{ (void)first; (void)second; if (event == 12) ++model_report.rejected; if (event == 13) ++model_report.resets; }
void a733_usb_poll_enable(bool enable) { (void)enable; }
void a733_usb_prepare(void) {}
/* The controller notices one OUT packet the host sent and one IN packet it collected. */
void a733_usb_poll(void)
{
    now += 48;
    if (out_pos < out_len && FIFO - rx_count >= PACKET) out_event = 1;
    if (tx_count && host_reads) tx_event = 1;
}
bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t *init) { (void)rhport; (void)init; return true; }
bool tud_mounted(void) { return true; }
tusb_speed_t tud_speed_get(void) { return TUSB_SPEED_HIGH; }
bool tud_disconnect(void) { return true; }
bool tud_control_status(uint8_t rhport, tusb_control_request_t const *request) { (void)rhport; (void)request; return true; }
bool tud_task_event_ready(void) { return out_event || tx_event; }
void tud_task_ext(uint32_t timeout, bool isr)
{
    (void)timeout; (void)isr; now += 24;
    if (out_event) {
        unsigned n = out_len - out_pos < PACKET ? out_len - out_pos : PACKET;
        for (unsigned i = 0; i < n; ++i) rx[rx_count + i] = host_out[out_pos + i];
        rx_count += n; out_pos += n; out_event = 0;
    }
    if (tx_event) {
        unsigned n = tx_count < PACKET ? tx_count : PACKET;
        for (unsigned i = 0; i < n; ++i) host_in[in_len + i] = tx[i];
        for (unsigned i = n; i < tx_count; ++i) tx[i - n] = tx[i];
        in_len += n; tx_count -= n; tx_event = 0;
        tud_vendor_tx_cb(0, n);
    }
}
uint32_t tud_vendor_n_available(uint8_t i) { (void)i; return rx_count; }
uint32_t tud_vendor_n_read(uint8_t i, void *buffer, uint32_t bytes)
{
    (void)i; uint8_t *p = buffer; if (bytes > rx_count) bytes = rx_count;
    for (unsigned j = 0; j < bytes; ++j) p[j] = rx[j];
    for (unsigned j = bytes; j < rx_count; ++j) rx[j - bytes] = rx[j];
    rx_count -= bytes; return bytes;
}
void tud_vendor_n_read_flush(uint8_t i) { (void)i; rx_count = 0; }
uint32_t tud_vendor_n_write(uint8_t i, const void *buffer, uint32_t bytes)
{
    (void)i; const uint8_t *p = buffer; if (bytes > FIFO - tx_count) bytes = FIFO - tx_count;
    for (unsigned j = 0; j < bytes; ++j) tx[tx_count + j] = p[j];
    tx_count += bytes; return bytes;
}
uint32_t tud_vendor_n_write_available(uint8_t i) { (void)i; return FIFO - tx_count; }
uint32_t tud_vendor_n_write_flush(uint8_t i) { (void)i; return 0; }
bool tud_vendor_n_write_clear(uint8_t i) { (void)i; tx_count = 0; return true; }

static void put(uint32_t value) { for (unsigned i = 0; i < 4; ++i) host_out[out_len++] = (uint8_t)(value >> (8 * i)); }
static uint32_t word(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void send(uint32_t magic, uint32_t sequence, uint32_t command, uint32_t length, const uint8_t *body, unsigned bytes)
{
    put(magic); put(sequence); put(command); put(length);
    for (unsigned i = 0; i < bytes; ++i) host_out[out_len++] = body[i];
}
static const uint8_t *log_bytes(void) { return model_result + LOG_OFFSET; }
#define CHECK(condition) do { if (!(condition)) { model_report.failed_stage = stage + 1; model_report.failed_line = __LINE__; finish(); } } while (0)
/* Consume one complete response from the start of host_in once it arrived. */
static int reply(uint32_t sequence, uint32_t command, uint32_t length, const uint8_t *body)
{
    if (in_len < 16 + length) return 0;
    CHECK(word(host_in) == MESSAGE_MAGIC && word(host_in + 4) == sequence && word(host_in + 8) == command &&
          word(host_in + 12) == length);
    for (unsigned i = 0; i < length; ++i) CHECK(host_in[16 + i] == body[i]);
    for (unsigned i = 16 + length; i < in_len; ++i) host_in[i - 16 - length] = host_in[i];
    in_len -= 16 + length;
    return 1;
}
static uint8_t pattern[2048];
static const tusb_control_request_t reset_setup = {
    .bmRequestType = 0x40, .bRequest = RESET_REQUEST, .wValue = 0, .wIndex = 0, .wLength = 0};

static void next(void) { model_report.stage_ticks[stage] = ticks - stage_start; ++stage; stage_start = ticks; waited = 0; }
/* One host step per tick. Each stage issues its request once, then waits. */
static void host(void)
{
    uint8_t offset[4];
    ++waited; CHECK(waited < 40);
    switch (stage) {
    case 0: /* Status. */
        if (waited == 1) send(MESSAGE_MAGIC, 1, 1, 0, NULL, 0);
        else if (in_len >= 56) { CHECK(word(host_in + 8) == 0x80000001U && word(host_in + 4) == 1 && word(host_in + 12) == 40); in_len = 0; next(); }
        break;
    case 1: /* Full 2,048-byte echo: five OUT and five IN packets. */
        if (waited == 1) send(MESSAGE_MAGIC, 2, 2, 2048, pattern, 2048);
        else if (reply(2, 0x80000002U, 2048, pattern)) next();
        break;
    case 2: /* Log without payload: the first 2,048 bytes. */
        if (waited == 1) send(MESSAGE_MAGIC, 3, 3, 0, NULL, 0);
        else if (reply(3, 0x80000003U, 2048, log_bytes())) next();
        break;
    case 3: case 4: case 5: { /* Log at offsets 2048, 4096 and 6000 of 5,000 bytes. */
        static const unsigned at[] = {2048, 4096, 6000}, size[] = {2048, 904, 0};
        unsigned k = stage - 3;
        for (unsigned i = 0; i < 4; ++i) offset[i] = (uint8_t)(at[k] >> (8 * i));
        if (waited == 1) send(MESSAGE_MAGIC, 4 + k, 3, 4, offset, 4);
        else if (reply(4 + k, 0x80000003U, size[k], log_bytes() + (k < 2 ? at[k] : 0))) next();
        break;
    }
    case 6: /* Bad magic is rejected with the received sequence. */
        if (waited == 1) send(0x12345678U, 7, 1, 0, NULL, 0);
        else if (reply(7, MESSAGE_REJECTED, 0, NULL)) next();
        break;
    case 7: /* An oversized length is rejected; no body is read. */
        if (waited == 1) send(MESSAGE_MAGIC, 8, 2, 4096, NULL, 0);
        else if (reply(8, MESSAGE_REJECTED, 0, NULL)) { CHECK(rx_count == 0); next(); }
        break;
    case 8: /* Truncated request: the host resets the channel, then a fresh request succeeds. */
        if (waited == 1) send(MESSAGE_MAGIC, 9, 2, 100, pattern, 40);
        else if (waited == 4) {
            CHECK(!in_len && received == 56);
            CHECK(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &reset_setup));
            CHECK(!received && !response_size);
            send(MESSAGE_MAGIC, 10, 2, 8, pattern, 8);
        } else if (waited > 4 && reply(10, 0x80000002U, 8, pattern)) next();
        break;
    case 9: /* Abandoned response: reset discards it, and the next reply arrives alone. */
        if (waited == 1) { host_reads = 0; send(MESSAGE_MAGIC, 11, 2, 2048, pattern, 2048); }
        else if (waited == 5) {
            CHECK(response_size == 2064 && !in_len);
            CHECK(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &reset_setup));
            host_reads = 1; send(MESSAGE_MAGIC, 12, 2, 4, pattern, 4);
        } else if (waited > 5 && reply(12, 0x80000002U, 4, pattern)) next();
        break;
    case 10: /* A misaligned stream: the echo completes and the trailing bytes are rejected. */
        if (waited == 1) {
            send(MESSAGE_MAGIC, 13, 2, 8, pattern, 8);
            send(0xffffffffU, 14, 0, 0, NULL, 0);
        } else if (waited > 1 && reply(13, 0x80000002U, 8, pattern)) next();
        break;
    case 11:
        if (reply(14, MESSAGE_REJECTED, 0, NULL)) { CHECK(!in_len); next(); }
        break;
    case 12: /* Other vendor control requests stall. */
        { tusb_control_request_t other = reset_setup; other.bRequest = 2; CHECK(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &other)); }
        { tusb_control_request_t in = reset_setup; in.bmRequestType = 0xc0; CHECK(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &in)); }
        { tusb_control_request_t interface = reset_setup; interface.bmRequestType = 0x41; CHECK(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &interface)); }
        next();
        break;
    default:
        model_report.done = 1; finish();
    }
}
void vTaskDelay(TickType_t delay)
{
    (void)delay;
    ++ticks; model_report.ticks = ticks;
    now = (now / 24000 + 1) * 24000;
    host();
}
void model_run(void)
{
    for (unsigned i = 0; i < sizeof(pattern); ++i) pattern[i] = (uint8_t)(i * 7 + 3);
    for (unsigned i = 0; i < 5000; ++i) model_result[LOG_OFFSET + i] = (uint8_t)('a' + i % 26);
    RESULT->log_length = 5000;
    RESULT->mode = 3;
    usb_task(NULL);
}
