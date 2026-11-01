/* Native mimus application on the SunstoneOS application interface. The OS
 * authenticates the INPUT spans before invoking mimus_main. */
#include "ss-adapter.h"
#include "../mimus/recorded.h"
#include "../mimus/parallel_runtime.h"
#include "runtime.h"
extern "C" {
#include "FreeRTOS.h"
#include "task.h"
void runtime_smp_init(void);
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
#include "dispatch.h"
#endif
}
#include <atomic>
#include <cstring>

namespace {
alignas(64) uint8_t scratch[97280];
uint8_t transcript[16384];
unsigned used;
std::atomic<unsigned> state{0}, started{0}, overlap{0}, generation{1}, acknowledged{0};
std::atomic<bool> stopped{false};
bool keyboard() { return ss_grant()->devices & SS_DEVICE_USB_KEYBOARD; }
bool storage() { return ss_grant()->devices & SS_DEVICE_USB_STORAGE; }
[[noreturn]] void failed()
{
    stopped.store(true, std::memory_order_release);
    state.store(2, std::memory_order_release);
    ss_fail(1);
}
void wait_clock()
{
    for (TickType_t waited = 0; !ss_clock_ready(); ++waited) {
        if (waited >= pdMS_TO_TICKS(1000) || ss_live() != 1) failed();
        ss_progress();
        vTaskDelay(1);
    }
}
void put16(uint8_t *p, uint16_t value)
{ for (unsigned i = 0; i < 2; ++i) p[i] = uint8_t(value >> (8 * i)); }
void put32(uint8_t *p, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i)); }
bool capture(unsigned current, std::span<const uint8_t> &input)
{
    if (!(ss_grant()->devices & (SS_DEVICE_AUDIO_I2S | SS_DEVICE_AUDIO_HEADSET))) return true;
    for (;;) {
        void *pcm = nullptr;
        size_t bytes = 0;
        int r = ss_capture(current, &pcm, &bytes);
        if (r < 0 || ss_live() != 1) return false;
        if (r == 1) {
            const size_t capacity = size_t(ss_grant()->capture_frames) * 2;
            const bool headset = ss_grant()->devices & SS_DEVICE_AUDIO_HEADSET;
            if (!pcm || !bytes || (bytes & 1) || bytes > capacity ||
                (!headset && bytes != capacity) || bytes > UINT32_MAX - 36) return false;
            static_assert(SS_CAPTURE_HEADROOM >= 44);
            auto *wav = static_cast<uint8_t *>(pcm) - 44;
            std::memcpy(wav, "RIFF", 4); put32(wav + 4, 36 + uint32_t(bytes));
            std::memcpy(wav + 8, "WAVEfmt ", 8); put32(wav + 16, 16);
            put16(wav + 20, 1); put16(wav + 22, 1); put32(wav + 24, 16000);
            put32(wav + 28, 32000); put16(wav + 32, 2); put16(wav + 34, 16);
            std::memcpy(wav + 36, "data", 4); put32(wav + 40, uint32_t(bytes));
            input = {wav, bytes + 44};
            return true;
        }
        ss_progress(); vTaskDelay(1);
    }
}
}

extern "C" void mimus_init(void)
{
    runtime_smp_init(); runtime_init();
    const struct ss_grant *grant = ss_grant();
    configASSERT(grant && grant->worker_cores);
    unsigned first = 0;
    while (!(grant->worker_cores & (1u << first))) ++first;
    /* The generated grant gives the paired worker first; the runtime checks
     * that all remaining worker cores equal the granted set. */
    if (grant->worker_cores & (1u << (grant->main_core ^ 1u))) first = grant->main_core ^ 1u;
    mimus_parallel_init(grant->main_core, first);
    configASSERT(mimus_host_start());
}

extern "C" void mimus_main(const struct ss_grant *grant)
{
    if (!grant || grant != ss_grant() || grant->region_count < 5 ||
        reinterpret_cast<uintptr_t>(grant->regions[3].base) != kMimusArenaBase ||
        grant->regions[3].bytes != kMimusArenaBytes) failed();
    mimus_parallel_bind();
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
    if (grant->devices & SS_DEVICE_GPU) { wait_clock(); os_gpu_open(); }
#endif
    std::span<const uint8_t> input[3];
    for (unsigned i = 0; i < 3; ++i)
        input[i] = {static_cast<const uint8_t *>(grant->regions[i].base), grant->regions[i].bytes};
    for (;;) {
        for (;;) {
            int trigger = mimus_trigger_step();
            if (trigger < 0 || ss_live() != 1) failed();
            if (trigger == 1) break;
            ss_progress(); vTaskDelay(1);
        }
        const unsigned current = generation.load(std::memory_order_acquire);
        if (!capture(current, input[2])) failed();
        wait_clock();
        started.store(current, std::memory_order_release);
        bool ok = mimus_transcribe_recorded(input, scratch, &stopped, kMimusWarm | kMimusVerified);
        started.store(0, std::memory_order_release);
        if (!ok || ss_live() != 1 || state.load(std::memory_order_acquire) == 2 ||
            (storage() && overlap.load(std::memory_order_acquire) != current)) failed();
        state.store(1, std::memory_order_release);
        if (!keyboard()) {
            mimus_parallel_stop();
            ss_finish(transcript, used);
        }
        while (acknowledged.load(std::memory_order_acquire) != current) {
            if (ss_live() != 1) failed();
            ss_progress(); vTaskDelay(1);
        }
        if (current == UINT32_MAX) failed();
        used = 0; overlap.store(0, std::memory_order_release);
        generation.store(current + 1, std::memory_order_release);
        state.store(0, std::memory_order_release);
        if (!mimus_trigger_rearm()) failed();
    }
}

extern "C" void mimus_native_checkpoint(void)
{
    if (ss_live() != 1) failed();
    if (ss_throttled()) { vTaskDelay(1); if (ss_live() != 1) failed(); }
    ss_progress();
}
extern "C" void mimus_native_inference_progress(void) { ss_progress(); }
extern "C" void platform_puts(const char *text)
{
    mimus_native_checkpoint();
    size_t n = 0;
    while (text[n] && n < sizeof(transcript) - used) ++n;
    if (n == sizeof(transcript) - used) failed();
    std::memcpy(transcript + used, text, n); used += n;
}
extern "C" void platform_hex(uint64_t x)
{
    char b[17]; static const char h[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) b[i] = h[(x >> (60 - 4 * i)) & 15];
    b[16] = 0; platform_puts(b);
}
extern "C" unsigned mimus_started(void) { return started.load(std::memory_order_acquire); }
extern "C" unsigned mimus_generation(void) { return generation.load(std::memory_order_acquire); }
extern "C" void mimus_storage_completed(unsigned current)
{ if (current && started.load(std::memory_order_acquire) == current) overlap.store(current, std::memory_order_release); }
extern "C" int mimus_result_ack(unsigned current)
{
    if (!keyboard() || ss_live() != 1 || state.load(std::memory_order_acquire) != 1 ||
        current != generation.load(std::memory_order_acquire) || acknowledged.load(std::memory_order_acquire) == current) return 0;
    state.store(3, std::memory_order_release);
    acknowledged.store(current, std::memory_order_release); return 1;
}
extern "C" int mimus_result_copy(uint32_t offset, size_t bytes, uint8_t *out, size_t *copied, uint32_t *total)
{
    *copied = 0; *total = 0;
    const unsigned ready = state.load(std::memory_order_acquire);
    if (ready == 2) return -1;
    if (ready != 1) return 0;
    /* ss_finish ends the main task; its resident transcript stays readable. */
    if (keyboard() && ss_live() != 1) return -1;
    if (storage() && overlap.load(std::memory_order_acquire) != generation.load(std::memory_order_acquire)) return -1;
    *total = used;
    if (offset > used || bytes > used - offset || bytes > SS_HOST_PAYLOAD_BYTES) return -1;
    std::memcpy(out, transcript + offset, bytes); *copied = bytes; return 1;
}
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
extern "C" void os_dispatch_stop(void) { failed(); }
#endif
