/* Host model of the shared USB0 service loop (apps/board/usb0_service.h).
 *
 * Only TinyUSB's vendor stream, the polled controller and time are modeled:
 * 512-byte bulk packets, a 4 KiB TX FIFO and a host that collects a written IN
 * packet, or delivers the next OUT packet, a fixed delay after the device arms
 * the endpoint. Each modeled pass costs a few microseconds and vTaskDelay(1)
 * advances to the next 1 ms tick. The actual service burst, pass and run code
 * is compiled from the header. No hardware is accessed. */
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
void vTaskDelay(TickType_t ticks);
struct a733_native_usb { int unused; };
static struct a733_native_usb model_providers;
static int model_live = 1, stopped;
int a733_native_usb_health(struct a733_native_usb *n);
int a733_native_usb_step(struct a733_native_usb *n);
static jmp_buf finish;
static _Noreturn void stop(void) { stopped = 1; longjmp(finish, 2); }
#define USB0_PID 0xefe8
#define USB0_PRODUCT "model"
#define USB0_SERIAL "model"
#define USB0_LIVE() model_live
#define USB0_PROVIDERS (&model_providers)
#include "../apps/board/usb0_service.h"

#define TICK_US 1000u
#define FIFO 4096u
#define PACKET 512u
static uint64_t now_ns;             /* modeled time */
static unsigned host_delay_ns;      /* host turnaround after an endpoint is armed */
static int host_reads = 1;          /* zero models an abandoned IN read */
/* IN: FIFO bytes, armed packet and its collection time. */
static unsigned tx_fifo, tx_armed, tx_done, tx_events;
static uint64_t tx_collect;
/* OUT: bytes the host still has to send, RX FIFO bytes and arm state. */
static unsigned out_left, rx_fifo, rx_events, rx_armed = 1;
static uint64_t rx_deliver;
/* Response produced by the modeled application exchange. */
static unsigned response_bytes, response_sent, response_transferred, request_bytes, request_got;
static unsigned ticks, passes, tick_passes, max_tick_passes, tick_packets, max_tick_packets, packets;
static uint64_t tick_start, max_tick_busy;
static unsigned tick_limit, last_tick_passes;

uint64_t a733_smp_counter(void) { now_ns += 100; return now_ns * 24 / 1000; }
int a733_native_usb_health(struct a733_native_usb *n) { (void)n; now_ns += 500; return 1; }
int a733_native_usb_step(struct a733_native_usb *n) { (void)n; return 1; }
bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t *init) { (void)rhport; (void)init; return true; }
tusb_speed_t tud_speed_get(void) { return TUSB_SPEED_HIGH; }
bool tud_task_event_ready(void) { return tx_events || rx_events; }
uint32_t tud_vendor_n_available(uint8_t i) { (void)i; return rx_fifo; }
uint32_t tud_vendor_n_write_available(uint8_t i) { (void)i; return FIFO - tx_fifo; }
uint32_t tud_vendor_n_read(uint8_t i, void *p, uint32_t n)
{ (void)i; (void)p; if (n > rx_fifo) n = rx_fifo; rx_fifo -= n; now_ns += 200; return n; }
static void arm_in(void)
{
    if (tx_armed || !tx_fifo) return;
    tx_armed = tx_fifo < PACKET ? tx_fifo : PACKET; tx_fifo -= tx_armed;
    tx_collect = now_ns + host_delay_ns; now_ns += 1000; /* 128 word PIO writes */
}
uint32_t tud_vendor_n_write(uint8_t i, const void *p, uint32_t n)
{ (void)i; (void)p; if (n > FIFO - tx_fifo) n = FIFO - tx_fifo; tx_fifo += n; now_ns += 200; return n; }
uint32_t tud_vendor_n_write_flush(uint8_t i) { (void)i; arm_in(); return 0; }
/* The polled controller notices a collected IN packet or a delivered OUT one. */
void dcd_int_handler(uint8_t rhport)
{
    (void)rhport; now_ns += 1500;
    if (tx_armed && host_reads && now_ns >= tx_collect) { tx_done = tx_armed; tx_armed = 0; ++tx_events; }
    if (rx_armed && out_left && now_ns >= rx_deliver) { rx_armed = 0; ++rx_events; }
}
void tud_task_ext(uint32_t timeout, bool isr)
{
    (void)timeout; (void)isr; now_ns += 500;
    if (tx_events) { tx_events = 0; ++packets; ++tick_packets; tud_vendor_tx_cb(0, tx_done); arm_in(); }
    if (rx_events) {
        rx_events = 0; unsigned n = out_left < PACKET ? out_left : PACKET;
        out_left -= n; rx_fifo += n; ++packets; ++tick_packets;
        if (FIFO - rx_fifo >= PACKET) { rx_armed = 1; rx_deliver = now_ns + host_delay_ns; }
    }
}
void tud_vendor_tx_cb(uint8_t interface, uint32_t bytes)
{
    (void)interface; response_transferred += bytes;
    if (response_transferred == response_bytes && response_sent == response_bytes) response_bytes = 0;
}
static void exchange(void)
{
    ++passes; ++tick_passes;
    if (response_bytes) {
        response_sent += tud_vendor_write(NULL, response_bytes - response_sent);
        tud_vendor_flush(); return;
    }
    unsigned n = tud_vendor_available();
    if (n) {
        request_got += tud_vendor_read(NULL, n);
        if (!rx_armed && FIFO - rx_fifo >= PACKET && out_left) { rx_armed = 1; rx_deliver = now_ns + host_delay_ns; }
    }
}
void vTaskDelay(TickType_t t)
{
    (void)t;
    uint64_t busy = now_ns - tick_start;
    if (busy > max_tick_busy) max_tick_busy = busy;
    if (tick_passes > max_tick_passes) max_tick_passes = tick_passes;
    if (tick_packets > max_tick_packets) max_tick_packets = tick_packets;
    ++ticks; last_tick_passes = tick_passes; tick_passes = tick_packets = 0;
    now_ns = (now_ns / (TICK_US * 1000) + 1) * TICK_US * 1000; tick_start = now_ns;
    if (ticks >= tick_limit || (tick_limit > 100 && !response_bytes && !out_left && !rx_fifo && request_got == request_bytes))
        longjmp(finish, 1);
}
static void reset(unsigned response, unsigned request, unsigned delay_ns, int reads, unsigned limit)
{
    now_ns = tick_start = 0; host_delay_ns = delay_ns; host_reads = reads; tick_limit = limit;
    tx_fifo = tx_armed = tx_done = tx_events = 0; rx_fifo = rx_events = 0; rx_armed = 1; rx_deliver = delay_ns;
    response_bytes = response; response_sent = response_transferred = 0;
    out_left = request_bytes = request; request_got = 0;
    ticks = passes = tick_passes = max_tick_passes = tick_packets = max_tick_packets = packets = 0;
    max_tick_busy = 0; stopped = 0; a733_usb_poll_enable(true);
}
static void run(const char *name, unsigned response, unsigned request, unsigned delay_ns, int reads, unsigned limit)
{
    reset(response, request, delay_ns, reads, limit);
    if (!setjmp(finish)) usb0_service_run(exchange);
    printf("{\"case\":\"%s\",\"response\":%u,\"request\":%u,\"host_delay_ns\":%u,\"ticks\":%u,\"packets\":%u,"
           "\"passes\":%u,\"max_tick_passes\":%u,\"last_tick_passes\":%u,\"max_tick_packets\":%u,\"max_tick_busy_us\":%llu,"
           "\"transferred\":%u,\"received\":%u,\"stopped\":%d}\n",
           name, response, request, delay_ns, ticks, packets, passes, max_tick_passes, last_tick_passes, max_tick_packets,
           (unsigned long long)(max_tick_busy / 1000), response_transferred, request_got, stopped);
}
int main(void)
{
    /* A 64 KiB response and a 2,064-byte diagnostic-sized request with a host
     * that turns each packet around in 20 us. */
    run("response-64k", 65536, 0, 20000, 1, 1000);
    run("request-2064", 0, 2064, 20000, 1, 1000);
    run("response-1160", 1160, 0, 20000, 1, 1000);
    /* A host slower than the post-progress window still advances one packet per tick. */
    run("slow-host", 4096, 0, 400000, 1, 1000);
    /* Idle and abandoned hosts cost one pass per tick. */
    run("idle", 0, 0, 20000, 1, 50);
    run("abandoned-read", 4096, 0, 20000, 0, 50);
    return 0;
}
