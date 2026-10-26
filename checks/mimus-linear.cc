#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "runtime.h"
#include "parallel_runtime.h"
#include "mimus/asr/kernels.h"
#include "mimus/asr/scratch.h"
#include <array>
#include <atomic>
#include "timing.h"
#include <cstring>
#include <vector>

extern "C" void runtime_smp_init(void);
namespace {
using namespace mimus::asr;
struct Case { unsigned columns, inner, rows, padding, offset; bool grouped, narrow; };
constexpr Case cases[] = {
    {32, 176, 128, 0, 0, false, false}, // all eight aligned matrix ranges
    {256, 16, 128, 0, 0, false, false}, // eight private residual accumulators
    {32, 176, 129, 1, 1, false, false}, // two-range gap repair stays bounded
    {33, 176, 128, 0, 0, false, false}, // partial group with eight-row quantizer

    {32, 176, 15, 0, 0, false, false}, // below parallel row threshold
    {32, 176, 16, 0, 0, false, false},
    {176, 704, 33, 0, 0, false, false},
    {32, 176, 17, 1, 1, false, false}, // base and stride cross cache lines
    {33, 176, 33, 0, 0, false, false}, // partial last packed group
    {8, 176, 17, 0, 1, false, false},  // stride below one cache line
    {256, 16, 17, 0, 0, false, false}, // private accumulator boundary
    {264, 16, 17, 0, 0, false, false},
    {704, 176, 17, 0, 0, false, false}, // several weight blocks
    {1025, 176, 17, 0, 1, false, false}, // vocabulary-width output
    {32, 176, 17, 1, 0, true, false},  // grouped weights retain serial sums
    {32, 176, 17, 1, 0, false, true},  // narrow activations retain their path
};
alignas(64) uint8_t fast_memory[97280];
StaticTask_t task_state;
StackType_t task_stack[16384];
StaticTask_t service_state;
StackType_t service_stack[1024];
TaskHandle_t service_task;
std::atomic<unsigned> entered, service_progress;
void service(void *) {
    configASSERT(portGET_CORE_ID() == 0);
    configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
    configASSERT(entered.load(std::memory_order_acquire) == (1u << kMaxWorkers) - 1);
    service_progress.store(1, std::memory_order_release);
    vTaskSuspend(nullptr);
    platform_panic("service resumed", 0);
}
void adapter_check() {
    struct alignas(64) Output { unsigned visits, core; } slots[kMaxWorkers]{};
    struct Context { Output *slots; uint64_t deadline; } ctx{slots, counter() + COUNTER_HZ};
    ParallelFor(kMaxWorkers, [](void *opaque, size_t a, size_t b, int worker) {
        auto &c = *static_cast<Context *>(opaque);
        configASSERT(a == unsigned(worker) && b == a + 1);
        c.slots[worker].core = portGET_CORE_ID();
        entered.fetch_or(1u << worker, std::memory_order_release);
        if (worker == 0) {
            while (entered.load(std::memory_order_acquire) != (1u << kMaxWorkers) - 1)
                configASSERT(counter() < c.deadline);
            xTaskNotifyGive(service_task);
        }
        while (!service_progress.load(std::memory_order_acquire))
            configASSERT(counter() < c.deadline);
        c.slots[worker].visits = 1;
    }, &ctx);
    for (unsigned i = 0; i < kMaxWorkers; ++i)
        configASSERT(slots[i].visits == 1 && slots[i].core == i);
    // Reuse borrowed stack contexts after each complete join, including counts
    // below capacity and counts not divisible by the worker count.
    for (unsigned repeat = 0; repeat < 16; ++repeat) {
        for (size_t n : {size_t(1), size_t(2), size_t(7), size_t(8), size_t(9), size_t(17)}) {
            struct alignas(64) Item { unsigned value; } result[17]{};
            ParallelFor(n, [](void *opaque, size_t a, size_t b, int worker) {
                auto *out = static_cast<Item *>(opaque);
                configASSERT(portGET_CORE_ID() == unsigned(worker));
                for (size_t i = a; i < b; ++i) out[i].value = unsigned(i + 1);
            }, result);
            for (size_t i = 0; i < 17; ++i) configASSERT(result[i].value == (i < n ? i + 1 : 0));
        }
    }
    configASSERT(kMaxWorkers == 2 || !AsyncAvailable());
    platform_puts("PASS SMP adapter: private joins, short ranges and core-0 service progress\n");
}


template<class T> T *aligned(std::vector<uint8_t> &storage, size_t count) {
    storage.resize(count * sizeof(T) + 64);
    return reinterpret_cast<T *>((reinterpret_cast<uintptr_t>(storage.data()) + 63) & ~uintptr_t(63));
}
std::vector<uint8_t> run(const Case &c, bool accumulate) {
    const unsigned kp = c.grouped ? (c.inner + 31) / 32 * 32 : c.inner;
    const unsigned stride = c.grouped ? kp / 2 : kp;
    std::vector<uint8_t> codes_storage, scales_storage;
    uint8_t *codes = aligned<uint8_t>(codes_storage, c.columns * stride);
    const unsigned scale_count = c.columns * (c.grouped ? kp / 32 : 1);
    uint16_t *scales = aligned<uint16_t>(scales_storage, scale_count);
    for (unsigned i = 0; i < c.columns * stride; ++i) {
        uint8_t value = uint8_t(i * 73 + (i / stride) * 31);
        if (c.grouped) {
            unsigned lo = value & 15, hi = value >> 4;
            codes[i] = uint8_t((lo == 8 ? 9 : lo) | ((hi == 8 ? 9 : hi) << 4));
        } else codes[i] = value == 128 ? 129 : value;
    }
    for (unsigned i = 0; i < scale_count; ++i) scales[i] = uint16_t(0x2000 + i % 1024);
    TensorView weights{};
    weights.dtype = c.grouped ? DType::kI4G : DType::kI8;
    weights.act_mode = c.narrow ? ActMode::kInt8Token : ActMode::kInt16;
    weights.group = c.grouped ? 32 : 0;
    weights.rank = 2;
    weights.shape = {c.columns, c.inner, 0, 0};
    weights.k_padded = kp; weights.row_stride = stride;
    weights.data = codes; weights.scales = scales;
    std::vector<float> input(c.rows * c.inner), bias(c.columns);
    for (size_t i = 0; i < input.size(); ++i) input[i] = float(int(i * 47 % 257) - 128) / 16;
    for (size_t i = 0; i < bias.size(); ++i) bias[i] = float(int(i % 11) - 5) / 8;
    const uint32_t input_crc = Crc32({reinterpret_cast<const uint8_t *>(input.data()), input.size() * 4});
    const uint32_t weight_crc = Crc32({codes, c.columns * stride});
    const size_t ldy = c.columns + c.padding, bytes = c.rows * ldy * sizeof(float);
    std::vector<uint8_t> output(bytes + 256, 0xa5);
    uintptr_t base = (reinterpret_cast<uintptr_t>(output.data()) + 63) & ~uintptr_t(63);
    float *y = reinterpret_cast<float *>(base + 64 + c.offset * 4);
    for (unsigned r = 0; r < c.rows; ++r)
        for (unsigned n = 0; n < c.columns; ++n) y[r * ldy + n] = -1.25f;
    SetInternalArena(fast_memory);
    Arena arena({reinterpret_cast<uint8_t *>(0x46000000), 16 * 1024 * 1024});
    const auto result = accumulate
        ? LinearAccumulate(weights, bias.data(), input.data(), c.rows, c.inner, y, ldy, 0.5f, arena)
        : Linear(weights, bias.data(), input.data(), c.rows, c.inner, y, ldy, arena);
    configASSERT(result.has_value());
    SetInternalArena({});
    configASSERT(input_crc == Crc32({reinterpret_cast<const uint8_t *>(input.data()), input.size() * 4}));
    configASSERT(weight_crc == Crc32({codes, c.columns * stride}));
    auto *data = reinterpret_cast<uint8_t *>(y);
    for (size_t i = 0; i < 64; ++i) configASSERT(data[-64 + ptrdiff_t(i)] == 0xa5 && data[bytes + i] == 0xa5);
    for (unsigned r = 0; r < c.rows; ++r)
        for (size_t i = c.columns * 4; i < ldy * 4; ++i) configASSERT(data[r * ldy * 4 + i] == 0xa5);
    return {data - 64, data + bytes + 64};
}
void check(void *) {
    constexpr size_t count = sizeof(cases) / sizeof(cases[0]);
    std::array<std::vector<uint8_t>, count * 2> expected;
    for (size_t i = 0; i < count; ++i)
        for (unsigned acc = 0; acc < 2; ++acc) expected[2 * i + acc] = run(cases[i], acc);
    mimus_parallel_bind();
    adapter_check();
    for (size_t i = 0; i < count; ++i)
        for (unsigned acc = 0; acc < 2; ++acc) configASSERT(expected[2 * i + acc] == run(cases[i], acc));
    mimus_parallel_report();
    platform_puts("PASS SMP linear: serial parity and output guards; 16 shapes, plain and residual output\n");
    platform_exit(0);
}
}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *name) { platform_panic(name, 0); }
extern "C" void kernel_main(void) {
    platform_init(); runtime_smp_init(); runtime_init(); mimus_parallel_init();
    service_task = xTaskCreateStaticAffinitySet(service, "service_check", 1024, nullptr, 4,
                                                service_stack, &service_state, 1);
    configASSERT(service_task);
    configASSERT(xTaskCreateStaticAffinitySet(check, "linear_check", 16384, nullptr, 3,
                                              task_stack, &task_state, 1));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
