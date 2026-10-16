// Host fixture for the actual asset protocol and recognition ownership handoff.
#include "FreeRTOS.h"
#include "queue.h"
#include "boot.h"
#include "../apps/mimus/service.h"
#include "../drivers/usb/usb_service.h"
#include "mimus/asr/model.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <vector>

static bool queued;
static uint32_t queued_value;
extern "C" QueueHandle_t xQueueGenericCreateStatic(UBaseType_t, UBaseType_t,
    uint8_t *, StaticQueue_t *state, uint8_t) { return reinterpret_cast<QueueHandle_t>(state); }
extern "C" BaseType_t xQueueGenericSend(QueueHandle_t, const void *item, TickType_t, BaseType_t) {
    assert(!queued); queued = true; std::memcpy(&queued_value, item, 4); return pdTRUE;
}
extern "C" BaseType_t xQueueReceive(QueueHandle_t, void *item, TickType_t) {
    assert(queued); queued = false; std::memcpy(item, &queued_value, 4); return pdTRUE;
}
extern "C" [[noreturn]] void platform_panic(const char *file, unsigned line) {
    std::fprintf(stderr, "panic: %s:%u\n", file, line); std::abort();
}
static std::vector<uint8_t> words(std::initializer_list<uint32_t> values) {
    std::vector<uint8_t> data(values.size() * 4);
    std::memcpy(data.data(), values.begin(), data.size()); return data;
}
static std::vector<uint8_t> request(uint32_t command, const std::vector<uint8_t> &input,
                                    uint32_t expected = 0) {
    uint8_t output[2048]; unsigned length = 0;
    assert(usb_application_dispatch(command, input.data(), input.size(), output, &length));
    assert(length >= 4 && length <= sizeof(output));
    uint32_t code; std::memcpy(&code, output, 4); assert(code == expected);
    return {output + 4, output + length};
}
int main() {
    assert(mmap(reinterpret_cast<void *>(0x43000000), 18 * 1024 * 1024,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
           == reinterpret_cast<void *>(0x43000000));
    assert(mmap(reinterpret_cast<void *>(RESULT_BASE), RESULT_SIZE,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
           == reinterpret_cast<void *>(RESULT_BASE));
    mimus_service_init();
    request(0x14, {}, 5);
    request(0x11, {}, 1);
    request(0x11, words({3, 1, 0}), 1);
    request(0x11, words({2, 0, 0}), 1);
    request(0x11, words({2, 2 * 1024 * 1024 + 1, 0}), 1);
    const std::vector<uint8_t> data{1, 2, 3, 4, 5};
    const uint32_t crc = mimus::asr::Crc32(data);
    for (unsigned slot = 0; slot < 3; ++slot) {
        request(0x11, words({slot, uint32_t(data.size()), crc ^ 1}));
        request(0x13, words({slot}), 5);
        auto packet = words({slot, 1}); packet.insert(packet.end(), data.begin(), data.end());
        request(0x12, packet, 3);
        packet = words({slot, 0}); packet.insert(packet.end(), data.begin(), data.end());
        request(0x12, packet);
        request(0x12, packet, 3);
        request(0x13, words({slot}), 4);
        request(0x11, words({slot, uint32_t(data.size()), crc}));
        request(0x12, packet);
        request(0x13, words({slot}));
    }
    for (unsigned generation = 1; generation <= 2; ++generation) {
        request(0x14, {});
        request(0x14, {}, 2);
        request(0x11, words({0, 1, 0}), 2);
        request(0x15, words({0}), 2);
        std::span<const uint8_t> inputs[3];
        mimus_service_wait(inputs);
        for (const auto input : inputs) {
            assert(input.size() == data.size());
            assert(std::memcmp(input.data(), data.data(), data.size()) == 0);
        }
        const char text[] = "recognition result";
        std::memcpy(reinterpret_cast<void *>(RESULT_BASE + LOG_OFFSET), text, sizeof(text));
        RESULT->log_length = sizeof(text);
        mimus_service_finish(generation == 1);
        const auto status = request(0x10, {});
        uint32_t status_words[4]; std::memcpy(status_words, status.data(), 16);
        assert(status_words[0] == (generation == 1 ? 2u : 3u));
        assert(status_words[1] == generation && status_words[2] == sizeof(text) && status_words[3] == 7);
        const auto result = request(0x15, words({0}));
        assert(result.size() == sizeof(text) && std::memcmp(result.data(), text, sizeof(text)) == 0);
        request(0x15, words({uint32_t(sizeof(text)) + 1}), 3);
    }
    std::puts("PASS: bounds, offset and CRC rejection, asset ownership, two generations, success/error results.");
}
