#include "service.h"
#include "recorded.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "boot.h"
#include "mimus/asr/model.h"
#include <atomic>
#include <cstring>

namespace {
enum State : uint32_t { Idle, Busy, Done, Failed };
enum Error : uint32_t { Ok, Invalid, Owned, Offset, Checksum, Incomplete };
struct Asset {
    uint8_t *data;
    uint32_t capacity, size, received, crc;
    bool valid;
};
Asset assets[] = {
    {reinterpret_cast<uint8_t *>(0x43000000), 8 * 1024 * 1024, 0, 0, 0, false},
    {reinterpret_cast<uint8_t *>(0x43800000), 8 * 1024 * 1024, 0, 0, 0, false},
    {reinterpret_cast<uint8_t *>(0x44000000), 2 * 1024 * 1024, 0, 0, 0, false},
};
std::atomic<State> state{Idle};
uint32_t generation;
StaticQueue_t queue_state;
uint8_t queue_storage[sizeof(uint32_t)];
QueueHandle_t requests;
uint32_t word(const uint8_t *data) {
    uint32_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}
}

void mimus_service_init() {
    requests = xQueueCreateStatic(1, sizeof(uint32_t), queue_storage, &queue_state);
    configASSERT(requests);
}
bool mimus_service_wait(std::span<const uint8_t> inputs[3]) {
    uint32_t request;
    configASSERT(xQueueReceive(requests, &request, portMAX_DELAY) == pdTRUE);
    configASSERT(state.load(std::memory_order_acquire) == Busy);
    for (unsigned i = 0; i < 3; ++i) inputs[i] = {assets[i].data, assets[i].size};
    return request != 0;
}
void mimus_service_finish(bool success) {
    state.store(success ? Done : Failed, std::memory_order_release);
}

extern "C" bool usb_application_dispatch(uint32_t command, const uint8_t *input,
                                          uint32_t size, uint8_t *output, unsigned *length) {
    if (command < 0x10 || command > 0x16) return false;
    Error error = Ok;
    *length = 4;
    const State current = state.load(std::memory_order_acquire);
    if (command == 0x10) {
        if (size != 0) error = Invalid;
        else {
            uint32_t valid = 0;
            for (unsigned i = 0; i < 3; ++i) if (assets[i].valid) valid |= 1u << i;
            const uint32_t status[] = {Ok, current, generation,
                current == Done || current == Failed ? uint32_t(RESULT->log_length) : 0u, valid};
            std::memcpy(output, status, sizeof(status));
            *length = sizeof(status);
        }
    } else if (current == Busy) error = Owned;
    else if (command == 0x11) {
        if (size != 12 || word(input) >= 3) error = Invalid;
        else {
            Asset &asset = assets[word(input)];
            const uint32_t bytes = word(input + 4);
            if (!bytes || bytes > asset.capacity) error = Invalid;
            else {
                // A retained engine refers to model slots; mark it stale before 0x12 writes.
                if (word(input) < 2) mimus_engine_invalidate();
                asset.size = bytes;
                asset.received = 0;
                asset.crc = word(input + 8);
                asset.valid = false;
                state.store(Idle, std::memory_order_release);
            }
        }
    } else if (command == 0x12) {
        if (size <= 8 || word(input) >= 3) error = Invalid;
        else {
            Asset &asset = assets[word(input)];
            if (asset.valid || word(input + 4) != asset.received) error = Offset;
            else if (size - 8 > asset.size - asset.received) error = Invalid;
            else {
                std::memcpy(asset.data + asset.received, input + 8, size - 8);
                asset.received += size - 8;
            }
        }
    } else if (command == 0x13) {
        if (size != 4 || word(input) >= 3) error = Invalid;
        else {
            Asset &asset = assets[word(input)];
            if (!asset.size || asset.received != asset.size) error = Incomplete;
            else if (mimus::asr::Crc32({asset.data, asset.size}) != asset.crc) error = Checksum;
            else asset.valid = true;
        }
    } else if (command == 0x14 || command == 0x16) {
        if (size != 0) error = Invalid;
        else if (!assets[0].valid || !assets[1].valid || !assets[2].valid) error = Incomplete;
        else {
            RESULT->log_length = 0;
            ++generation;
            state.store(Busy, std::memory_order_release);
            const uint32_t paced = command == 0x16;
            configASSERT(xQueueSend(requests, &paced, 0) == pdTRUE);
        }
    } else if (command == 0x15) {
        if (size != 4) error = Invalid;
        else if (current != Done && current != Failed) error = Incomplete;
        else if (word(input) > RESULT->log_length) error = Offset;
        else {
            uint32_t bytes = uint32_t(RESULT->log_length) - word(input);
            if (bytes > 2044) bytes = 2044;
            std::memcpy(output + 4, reinterpret_cast<const void *>(RESULT_BASE + LOG_OFFSET + word(input)), bytes);
            *length += bytes;
        }
    }
    std::memcpy(output, &error, sizeof(error));
    return true;
}
