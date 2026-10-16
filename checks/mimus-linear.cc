#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "runtime.h"
#include "parallel_runtime.h"
#include "mimus/asr/kernels.h"
#include "mimus/asr/scratch.h"
#include <array>
#include <cstring>
#include <vector>

extern "C" void runtime_smp_init(void);
namespace {
using namespace mimus::asr;
struct Case { unsigned columns, inner, rows, padding, offset; bool grouped, narrow; };
constexpr Case cases[] = {
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
    for (size_t i = 0; i < count; ++i)
        for (unsigned acc = 0; acc < 2; ++acc) configASSERT(expected[2 * i + acc] == run(cases[i], acc));
    mimus_parallel_report();
    platform_puts("PASS SMP linear: serial parity and output guards; 12 shapes, plain and residual output\n");
    platform_exit(0);
}
}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *name) { platform_panic(name, 0); }
extern "C" void kernel_main(void) {
    platform_init(); runtime_smp_init(); runtime_init(); mimus_parallel_init();
    configASSERT(xTaskCreateStaticAffinitySet(check, "linear_check", 16384, nullptr, 3,
                                              task_stack, &task_state, 1));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
