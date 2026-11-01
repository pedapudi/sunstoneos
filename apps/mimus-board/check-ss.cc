/* Run the application and responder on host threads. Device requests,
 * scheduling and inference are modeled; the adapter and keyboard gate run. */
#include "ss-adapter.h"
#include "../mimus/recorded.h"
#include "FreeRTOS.h"
#include "task.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" void platform_puts(const char *);
extern "C" void mimus_native_checkpoint(void);
namespace {
struct Stopped {};
struct Finished {};
struct Failed {};
struct ss_grant grant;
TaskFunction_t host_task;
std::atomic<int> live{1};
std::atomic<unsigned> failures{0}, calls{0}, captures{0}, clocks{0}, progresses{0}, stops{0};
bool omit_overlap;
bool headset;
std::mutex mutex;
std::condition_variable changed;
std::deque<struct ss_key_report> keys;
ss_host_request pending;
bool request_ready, reply_ready, ending;
ss_host_reply response;
std::vector<uint8_t> response_bytes;
std::string result;
alignas(64) uint8_t capture_buffer[64 + 32000];
std::thread host_thread;
uint32_t little32(const uint8_t *p)
{ return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
struct Reply { ss_host_reply header; std::vector<uint8_t> bytes; };
Reply exchange(unsigned op, unsigned count = 0, unsigned offset = 0, unsigned generation = 0)
{
    std::unique_lock lock(mutex);
    assert(!request_ready && !reply_ready);
    pending = {uint8_t(op), 0, uint16_t(count), offset, generation};
    request_ready = true; changed.notify_all();
    assert(changed.wait_for(lock, std::chrono::seconds(3), [] { return reply_ready || failures.load(); }));
    assert(reply_ready && !failures.load());
    Reply answer{response, response_bytes};
    reply_ready = false; changed.notify_all();
    return answer;
}
template<class Predicate> void wait(Predicate predicate)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < until);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void key(unsigned sequence, unsigned code)
{
    struct ss_key_report report{};
    report.attachment = 23; report.sequence = sequence; report.address = 3; report.speed = 1;
    report.protocol = 1; report.endpoint = 0x81; report.subclass = 1; report.bytes = 8;
    report.data[2] = uint8_t(code);
    std::lock_guard lock(mutex); keys.push_back(report);
}
void shutdown()
{
    { std::lock_guard lock(mutex); ending = true; changed.notify_all(); }
    host_thread.join();
}
}

extern "C" const struct ss_grant *ss_grant(void) { return &grant; }
extern "C" int ss_live(void) { return live.load(); }
extern "C" void ss_progress(void) { ++progresses; }
extern "C" int ss_throttled(void) { return 1; }
extern "C" int ss_clock_ready(void)
{ if (grant.capture_frames) assert(captures.load()); ++clocks; return 1; }
extern "C" int ss_capture(uint32_t sequence, void **pcm, size_t *bytes)
{
    assert(sequence == 1 && grant.capture_frames == (headset ? 48000u : 16000u));
    ++captures; *pcm = capture_buffer + 64; *bytes = 32000; return 1;
}
extern "C" int ss_storage_read(uint64_t sector, uint8_t data[512])
{
    assert(!sector && mimus_started());
    if (omit_overlap) return 0;
    for (unsigned i = 0; i < 512; ++i) data[i] = uint8_t(i);
    return 1;
}
extern "C" int ss_key_report(struct ss_key_report *report)
{
    std::lock_guard lock(mutex);
    if (keys.empty()) return 0;
    *report = keys.front(); keys.pop_front(); return 1;
}
extern "C" int ss_host_recv(struct ss_host_request *request, uint32_t wait_us)
{
    assert(wait_us == 1000);
    std::unique_lock lock(mutex);
    changed.wait_for(lock, std::chrono::microseconds(wait_us), [] { return request_ready || ending; });
    if (ending) return -1;
    if (!request_ready) return 0;
    assert(!reply_ready);
    *request = pending; request_ready = false; return 1;
}
extern "C" int ss_host_send(const struct ss_host_reply *reply)
{
    std::lock_guard lock(mutex);
    assert(!reply_ready && reply->bytes <= SS_HOST_PAYLOAD_BYTES);
    response = *reply;
    const auto *p = static_cast<const uint8_t *>(reply->data);
    response_bytes.assign(p, p + reply->bytes);
    reply_ready = true; changed.notify_all(); return 1;
}
extern "C" void ss_finish(const void *bytes, size_t count)
{ result.assign(static_cast<const char *>(bytes), count); live = -1; throw Finished{}; }
extern "C" void ss_fail(uint32_t)
{ ++failures; live = -1; changed.notify_all(); throw Failed{}; }
extern "C" void vTaskDelay(TickType_t) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
extern "C" void vTaskSuspend(TaskHandle_t) { throw Stopped{}; }
extern "C" TaskHandle_t xTaskCreateStaticAffinitySet(TaskFunction_t task, const char *,
    configSTACK_DEPTH_TYPE depth, void *, UBaseType_t priority, StackType_t *, StaticTask_t *, UBaseType_t affinity)
{
    assert(depth == 2048 && priority == 3 && affinity == (1u << grant.main_core));
    host_task = task; return reinterpret_cast<TaskHandle_t>(1);
}
void mimus_parallel_init(unsigned owner, unsigned worker) { assert(owner == 6 && worker == 7); }
void mimus_parallel_bind() {}
void mimus_parallel_stop() { ++stops; }
bool mimus_transcribe_recorded(std::span<const uint8_t> input[3], std::span<uint8_t> scratch,
    const std::atomic<bool> *stopped, unsigned options)
{
    const unsigned current = ++calls;
    assert(mimus_started() == current && options == (kMimusWarm | kMimusVerified) && !stopped->load());
    assert(scratch.size() == 97280 && input[0].data() == grant.regions[0].base &&
           input[1].size() == grant.regions[1].bytes);
    if (grant.capture_frames) {
        assert(input[2].data() == capture_buffer + 20 && input[2].size() == 32044);
        assert(!std::memcmp(input[2].data(), "RIFF", 4) && little32(input[2].data() + 4) == 32036 &&
               little32(input[2].data() + 40) == 32000);
        for (unsigned i = 0; i < 20; ++i) assert(capture_buffer[i] == 0xa5);
        for (unsigned i = 64; i < sizeof(capture_buffer); ++i) assert(capture_buffer[i] == 0xa5);
    }
    if ((grant.devices & SS_DEVICE_USB_STORAGE) && !omit_overlap)
        wait([&] { return mimus_storage_ready(current); });
    mimus_native_checkpoint();
    platform_puts(current == 1 ? "first transcript\n" : "second transcript\n");
    return true;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const bool keyboard = !std::strcmp(argv[1], "keyboard");
    headset = !std::strcmp(argv[1], "headset");
    const bool capture = !std::strcmp(argv[1], "capture") || headset;
    omit_overlap = !std::strcmp(argv[1], "no-overlap");
    grant.main_core = 6; grant.worker_cores = 1u << 7; grant.region_count = 5;
    grant.devices = keyboard ? SS_DEVICE_USB_STORAGE | SS_DEVICE_USB_KEYBOARD :
        capture ? headset ? SS_DEVICE_AUDIO_HEADSET : SS_DEVICE_AUDIO_I2S : omit_overlap ? SS_DEVICE_USB_STORAGE : 0;
    uint8_t inputs[3][16]{};
    for (unsigned i = 0; i < 3; ++i) grant.regions[i] = {inputs[i], sizeof(inputs[i])};
    grant.regions[3] = {reinterpret_cast<void *>(kMimusArenaBase), kMimusArenaBytes};
    grant.capture_frames = capture ? headset ? 48000 : 16000 : 0;
    std::memset(capture_buffer, 0xa5, sizeof(capture_buffer));
    mimus_init(); assert(host_task);
    host_thread = std::thread([] { try { host_task(nullptr); } catch (const Stopped &) {} catch (const Failed &) {} });
    if (!keyboard) {
        try { mimus_main(&grant); assert(false); } catch (const Finished &) {} catch (const Failed &) {}
        if (omit_overlap) assert(failures == 1 && !stops && result.empty());
        else {
            assert(!failures && stops == 1 && result == "first transcript\n" && clocks && progresses);
            Reply ready = exchange(0); assert(ready.header.state == 1 && ready.header.total == result.size());
            Reply text = exchange(1, unsigned(result.size()));
            assert(std::string(text.bytes.begin(), text.bytes.end()) == result);
        }
        shutdown(); return 0;
    }
    std::thread recognition([] { try { mimus_main(&grant); } catch (const Failed &) {} });
    unsigned sequence = 0;
    for (unsigned current = 1; current <= 2; ++current) {
        key(++sequence, 0x28); key(++sequence, 0); key(++sequence, 0x28);
        Reply status;
        do { status = exchange(0); } while (status.header.state == 0);
        const std::string expected = current == 1 ? "first transcript\n" : "second transcript\n";
        assert(status.header.state == 1 && little32(status.header.tag) == current && status.header.total == expected.size());
        Reply early = exchange(4, 0, 0, current); assert(early.header.state == 2);
        Reply text = exchange(1, unsigned(expected.size()), 0, current);
        assert(std::string(text.bytes.begin(), text.bytes.end()) == expected);
        Reply sector = exchange(2, 512, 0, current);
        assert(sector.header.state == 1 && sector.bytes.size() == 512);
        for (unsigned i = 0; i < 512; ++i) assert(sector.bytes[i] == uint8_t(i));
        Reply trigger = exchange(3, 128, 0, current);
        assert(trigger.header.state == 1 && trigger.bytes.size() == 128 && little32(trigger.bytes.data()) == 0x4752544b);
        Reply ack = exchange(4, 0, 0, current); assert(ack.header.state == 1);
        assert(mimus_generation() == current); // ACK has not yet been delivery-confirmed.
        (void)exchange(0); // Confirms ACK delivery and permits buffer reuse.
        wait([&] { return mimus_generation() == current + 1; });
    }
    assert(calls == 2 && !failures);
    live = -1;
    recognition.join(); assert(failures == 1);
    shutdown();
}
