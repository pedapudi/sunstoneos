// Paced adapter contract with actual engine types, model creation and formatter.
// The linker replaces recognition only; arithmetic and scheduling are not tested.
#include "../apps/mimus/recorded.h"
#include "../apps/mimus/replay_runtime.h"
#include "mimus/asr/recognizer.h"
#include "mimus/asr/platform.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using mimus::asr::Recognition;
static std::vector<Recognition> results;
static size_t recognized;
static std::string output;
static uint64_t clock_ns;
static uint32_t gap;

namespace mimus::asr::platform {
uint64_t MonotonicNanos() { return clock_ns += 1000; }
}
extern "C" void platform_puts(const char *text) { output += text; }
extern "C" size_t runtime_heap_peak() { return 0; }

// The driver resolves this symbol from paced.o, so a signature change fails setup.
mimus::asr::Result<Recognition> wrapped_recognize(
    const mimus::asr::Recognizer *, std::span<const int16_t>, mimus::asr::Arena &,
    const std::atomic<bool> *) asm(WRAPPED_RECOGNIZE);
mimus::asr::Result<Recognition> wrapped_recognize(
    const mimus::asr::Recognizer *, std::span<const int16_t>, mimus::asr::Arena &,
    const std::atomic<bool> *) {
    assert(recognized < results.size());
    return results[recognized++];
}

bool mimus_replay_run(std::span<const int16_t> pcm, ReplayConsumer consumer, void *context) {
    const uint64_t start = mimus::asr::platform::MonotonicNanos();
    for (size_t i = 0; i < results.size(); ++i) {
        const uint32_t begin = uint32_t(i) * (16000 + gap);
        const sunstone::ReplaySegment segment{begin, begin + 16000, begin, begin + 16000, 0};
        if (!consumer(context, pcm, segment, start)) return false;
    }
    char line[128];
    std::snprintf(line, sizeof(line), "REPLAY samples=%zu segments=%zu consumed=%zu state=1\n",
                  pcm.size(), results.size(), recognized);
    platform_puts(line);
    return true;
}

int main(int argc, char **argv) {
    assert(argc == 4);
    std::ifstream file(argv[1], std::ios::binary); assert(file);
    std::vector<uint8_t> bundle{std::istreambuf_iterator<char>(file), {}};
    assert(bundle.size() > 128 && !std::memcmp(bundle.data(), "SUNMIM01", 8));
    uint64_t sizes[3]; std::memcpy(sizes, bundle.data() + 8, sizeof(sizes));
    std::span<const uint8_t> inputs[3]; size_t offset = 128;
    for (unsigned i = 0; i < 3; ++i) {
        assert(offset <= bundle.size() && sizes[i] && sizes[i] <= bundle.size() - offset);
        inputs[i] = {bundle.data() + offset, size_t(sizes[i])};
        offset = (offset + sizes[i] + 63) & ~size_t(63);
    }
    auto model = mimus::asr::Model::Load(inputs[0], inputs[1], true, mimus::asr::kTargetOrangePi);
    assert(model);
    auto engine = mimus::asr::Recognizer::Create(*model); assert(engine);
    Recognition plain;
    plain.text = "hello world";
    plain.frames = 25;
    Recognition scored = plain;
    scored.punct = true;
    scored.words.resize(2);
    scored.words[0].scored = scored.words[1].scored = true;
    scored.words[0].mark = mimus::asr::Mark::kComma;
    scored.words[1].mark = mimus::asr::Mark::kQuestion;
    scored.words[1].capital = true;
    const std::string name = argv[2];
    bool success = true;
    if (name == "no-head-empty") results = {plain};
    else if (name == "no-head-scores") { scored.punct = false; results = {scored}; }
    else if (name == "head") results = {scored};
    else if (name == "head-unscored") { scored.words[0].scored = false; results = {scored}; }
    else if (name == "head-misaligned") { scored.words.pop_back(); results = {scored}; success = false; }
    else if (name == "empty") { plain.text.clear(); results = {plain}; }
    else if (name == "short-pause") { results = {plain, plain}; gap = 16000; }
    else if (name == "long-pause") { results = {plain, plain}; gap = 40000; }
    else { assert(false && "unknown case"); }
    const bool passed = mimus_paced_run(*engine, inputs[2]);
    std::ofstream(argv[3]) << output;
    assert(passed == success);
    assert(recognized == results.size());
    if (!success) {
        assert(output == "ERROR word alignment differs\n");
        return 0;
    }
    assert(output.find("PASS mimus: paced utterance replay completed\n") != std::string::npos);
    if (name == "head" || name == "head-unscored") {
        assert(output.find(name == "head" ? "MARKS 13\nCAPS 01\nFORMATTED Hello, World\n"
                                          : "MARKS 93\nCAPS 01\nFORMATTED Hello World\n") != std::string::npos);
        assert(output.find("FLUSH ?\n") != std::string::npos);
    } else {
        assert(output.find("MARKS ") == std::string::npos && output.find("CAPS ") == std::string::npos);
        if (name == "empty") assert(output.find("FORMATTED \nREPLAY ") != std::string::npos && output.find("FLUSH \n") != std::string::npos);
        else {
            assert(output.find("FORMATTED Hello world\n") != std::string::npos);
            assert(output.find("FLUSH .\n") != std::string::npos);
            if (name == "short-pause") assert(output.find("FORMATTED hello world\n") != std::string::npos);
            if (name == "long-pause") assert(output.find("FORMATTED . Hello world\n") != std::string::npos);
        }
    }
    std::printf("PASS %s: %zu recognition calls, no arithmetic executed\n", name.c_str(), recognized);
}
