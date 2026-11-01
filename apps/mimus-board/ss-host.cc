/* Application operations 0..4. Receiving the next request confirms that the
 * previous reply reached the host; only then may ACK release a transcript. */
#include "ss-adapter.h"
extern "C" {
#include "keyboard.h"
#include "FreeRTOS.h"
#include "task.h"
}
#include <atomic>
#include <cstring>

namespace {
StaticTask_t responder_state;
StackType_t responder_stack[2048];
uint8_t sector[512];
std::atomic<unsigned> sector_generation{0};
bool keyboard() { return ss_grant()->devices & SS_DEVICE_USB_KEYBOARD; }
bool storage() { return ss_grant()->devices & SS_DEVICE_USB_STORAGE; }
[[noreturn]] void fail() { ss_fail(2); }
void put32(uint8_t *p, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i)); }
void devices()
{
    if (ss_live() != 1) return;
    const unsigned current = mimus_started();
    if (storage() && current && sector_generation.load(std::memory_order_acquire) != current) {
        int r = ss_storage_read(0, sector);
        if (r < 0) fail();
        if (r == 1) {
            mimus_storage_completed(current);
            sector_generation.store(current, std::memory_order_release);
        }
    }
    if (keyboard() && mimus_keyboard_empty()) {
        struct ss_key_report raw;
        int r = ss_key_report(&raw);
        if (r < 0) fail();
        if (r == 1) {
            mimus_keyboard_report report{raw.attachment, raw.sequence, raw.address, raw.speed,
                raw.interface, raw.protocol, raw.endpoint, raw.subclass, raw.mode, raw.bytes, {}};
            std::memcpy(report.data, raw.data, sizeof(report.data));
            if (mimus_keyboard_offer(&report) != 1) fail();
        }
    }
}
struct Delivery {
    unsigned generation = 0, text = 0, sector = 0, trigger = 0;
    unsigned response_generation = 0, op = 0, offset = 0, count = 0, total = 0;
    bool successful = false;
    void delivered()
    {
        if (!successful || !keyboard()) return;
        if (response_generation != mimus_generation()) fail();
        if (op == 1 && offset <= text && offset + count > text) text = offset + count;
        if (op == 2) sector = 1;
        if (op == 3) trigger = 1;
        if (op == 4 && (!complete() || !mimus_result_ack(response_generation))) fail();
    }
    bool complete() const { return text == total && sector && trigger; }
};
void responder(void *)
{
    Delivery delivery;
    uint8_t payload[SS_HOST_PAYLOAD_BYTES];
    for (;;) {
        devices();
        struct ss_host_request request;
        int received = ss_host_recv(&request, 1000);
        if (received < 0) for (;;) vTaskSuspend(nullptr);
        if (!received) continue;
        delivery.delivered();
        const unsigned op = request.op, current = mimus_generation();
        const bool keys = keyboard();
        if (op > (keys ? 4u : storage() ? 2u : 1u) || request.flags || request.count > sizeof(payload) ||
            ((!keys || op == 0) && request.argument) ||
            (op == 0 && (request.offset || request.count)) || (op == 1 && !request.count) ||
            (op == 2 && (request.offset || request.count != 512)) ||
            (op == 3 && (request.offset || request.count != 128)) ||
            (op == 4 && (request.offset || request.count))) fail();
        if (keys && delivery.generation != current) {
            delivery.generation = current;
            delivery.text = delivery.sector = delivery.trigger = 0;
        }
        size_t copied = 0;
        uint32_t total = 0;
        int r = mimus_result_copy(op == 1 ? request.offset : 0, op == 1 ? request.count : 0,
                                  payload, &copied, &total);
        if (keys && op && request.argument != current) r = -1;
        if (r == 1 && storage() && !mimus_storage_ready(current)) r = 0;
        if (r == 1 && op == 2) { r = mimus_sector_copy(payload); copied = r == 1 ? 512 : 0; }
        if (r == 1 && op == 3) { r = mimus_trigger_copy(payload); copied = r == 1 ? 128 : 0; }
        delivery.total = total;
        if (r == 1 && op == 4 && !delivery.complete()) r = -1;
        if (keys && mimus_generation() != current) r = 0;
        if (r != 1) copied = 0;
        struct ss_host_reply reply{};
        reply.version = keys ? 3 : 1;
        reply.state = r < 0 ? 2 : r == 1 ? 1 : 0;
        reply.total = total; reply.bytes = uint32_t(copied); reply.data = payload;
        if (keys) { put32(reply.tag, current); put32(reply.tag + 8, UINT32_MAX); }
        delivery.response_generation = current; delivery.op = op;
        delivery.offset = request.offset; delivery.count = unsigned(copied); delivery.successful = r == 1;
        if (ss_host_send(&reply) != 1) fail();
    }
}
}

extern "C" int mimus_host_start(void)
{
    return xTaskCreateStaticAffinitySet(responder, "mimus-host", 2048, nullptr, 3,
        responder_stack, &responder_state, UBaseType_t{1} << ss_grant()->main_core) != nullptr;
}
extern "C" int mimus_storage_ready(unsigned current)
{ return sector_generation.load(std::memory_order_acquire) == current; }
extern "C" int mimus_sector_copy(uint8_t out[512])
{
    if (!mimus_storage_ready(mimus_generation())) return 0;
    std::memcpy(out, sector, sizeof(sector)); return 1;
}
extern "C" int mimus_trigger_step(void)
{ return ss_live() != 1 ? -1 : keyboard() ? mimus_keyboard_step() : 1; }
extern "C" int mimus_trigger_copy(uint8_t out[128])
{ return keyboard() ? mimus_keyboard_copy(out) : -1; }
extern "C" int mimus_trigger_rearm(void)
{ return ss_live() == 1 && mimus_keyboard_rearm(); }
