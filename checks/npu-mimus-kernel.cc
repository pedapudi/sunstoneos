// Run the mimus portable int16 GEMM with its float read-out (MimusGemmS16FC)
// on one exported linear-layer study and compare its output with the study's
// reference bit for bit. Build against an unmodified mimus checkout:
//
//   clang++-21 -std=c++20 -O1 -ffp-contract=off \
//     -I MIMUS/firmware/components/asr/include checks/npu-mimus-kernel.cc \
//     checks/npu-mimus-digits.cc \
//     MIMUS/firmware/components/asr/src/gemm_combine_portable.cc -o kernel
//   kernel STUDY_DIRECTORY
//
// The directory holds raw little-endian files written by
// tools/npu-mimus-linear.py: x.i16 (rows x k activation codes), w.i8 (n x k
// weight codes), ws.f32, xs.f32, bias.f32 and reference.f32 (rows x n). The
// check packs the weights in the kernel's groups of eight output channels.
//
// A digit-form study also holds chunks.u32 and partials.i16, the simulator's
// INT16 partial sums. The check then splits x.i16 with MimusNpuDigitSplit,
// requires the result to equal the simulator's input files
// export_nbg_unify/input_C.dat, recombines the partials with
// MimusNpuDigitCombine and counts read-out floats whose bytes differ from
// MimusGemmS16FC. A pass requires zero mismatches.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "mimus/asr/gemm.h"

extern "C" int MimusNpuDigitSplit(const int16_t* x, uint32_t rows, uint32_t k, const uint32_t* chunks,
                                  uint32_t count, int8_t* const* inputs);
extern "C" int MimusNpuDigitCombine(const int16_t* const* partials, uint32_t count, uint32_t rows,
                                    uint32_t n, const float* ws, const float* xs, const float* bias,
                                    float* out);

namespace mimus::asr {
void MaybeYield() {}  // The host reference runs on one thread.
}  // namespace mimus::asr

static bool exists(const std::string& path) { return std::ifstream(path, std::ios::binary).good(); }

template <class T>
static std::vector<T> load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
  if (bytes.size() % sizeof(T) != 0) throw std::runtime_error("odd size " + path);
  std::vector<T> out(bytes.size() / sizeof(T));
  std::memcpy(out.data(), bytes.data(), bytes.size());
  return out;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s STUDY_DIRECTORY\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const auto x = load<int16_t>(dir + "/x.i16");
  const auto w = load<int8_t>(dir + "/w.i8");
  const auto ws = load<float>(dir + "/ws.f32");
  const auto xs = load<float>(dir + "/xs.f32");
  const auto bias = load<float>(dir + "/bias.f32");
  const auto reference = load<float>(dir + "/reference.f32");
  const size_t n = ws.size(), rows = xs.size(), k = x.size() / rows;
  if (n % 8 || k % 8 || w.size() != n * k || bias.size() != n || reference.size() != rows * n) {
    std::fprintf(stderr, "FAIL inconsistent study shapes\n");
    return 1;
  }
  // Group g starts at g * w_stride; element (8 g + c, k) is at k * 8 + c.
  std::vector<int16_t> packed(n * k);
  for (size_t row = 0; row < n; ++row)
    for (size_t col = 0; col < k; ++col)
      packed[row / 8 * 8 * k + col * 8 + row % 8] = w[row * k + col];
  std::vector<int16_t> activations(x.begin(), x.end());
  std::vector<float> out(rows * n);
  MimusGemmS16Args args{activations.data(), packed.data(), out.data(), static_cast<uint32_t>(rows),
                        static_cast<uint32_t>(n / 8), static_cast<uint32_t>(k),
                        static_cast<uint32_t>(k), static_cast<uint32_t>(8 * k),
                        static_cast<uint32_t>(n), ws.data(), xs.data(), bias.data()};
  MimusGemmS16FC(&args);
  size_t equal = 0;
  for (size_t i = 0; i < out.size(); ++i) equal += std::memcmp(&out[i], &reference[i], 4) == 0;
  std::printf("%s mimus kernel: %zu of %zu float outputs bit-identical to the study reference\n",
              equal == out.size() ? "PASS" : "FAIL", equal, out.size());
  if (equal != out.size()) return 1;
  if (!exists(dir + "/chunks.u32")) return 0;

  const auto chunks = load<uint32_t>(dir + "/chunks.u32");
  const auto partials = load<int16_t>(dir + "/partials.i16");
  const size_t count = chunks.size(), stacked = 4 * rows;
  if (partials.size() != count * stacked * n) {
    std::fprintf(stderr, "FAIL partials do not match %zu chunks of %zu x %zu\n", count, stacked, n);
    return 1;
  }
  std::vector<std::vector<int8_t>> digits(count);
  std::vector<int8_t*> inputs(count);
  for (size_t c = 0; c < count; ++c) {
    digits[c].resize(stacked * chunks[c]);
    inputs[c] = digits[c].data();
  }
  if (!MimusNpuDigitSplit(x.data(), static_cast<uint32_t>(rows), static_cast<uint32_t>(k), chunks.data(),
                          static_cast<uint32_t>(count), inputs.data())) {
    std::fprintf(stderr, "FAIL digit split refused the chunk lengths\n");
    return 1;
  }
  for (size_t c = 0; c < count; ++c) {
    const auto simulated = load<int8_t>(dir + "/export_nbg_unify/input_" + std::to_string(c) + ".dat");
    if (simulated != digits[c]) {
      std::fprintf(stderr, "FAIL digit split differs from simulator input %zu\n", c);
      return 1;
    }
  }
  std::vector<const int16_t*> views(count);
  for (size_t c = 0; c < count; ++c) views[c] = partials.data() + c * stacked * n;
  std::vector<float> combined(rows * n);
  MimusNpuDigitCombine(views.data(), static_cast<uint32_t>(count), static_cast<uint32_t>(rows),
                       static_cast<uint32_t>(n), ws.data(), xs.data(), bias.data(), combined.data());
  size_t mismatches = 0;
  for (size_t i = 0; i < out.size(); ++i) mismatches += std::memcmp(&combined[i], &out[i], 4) != 0;
  std::printf("%s digit read-out: %zu chunks, digit split equals the simulator inputs, "
              "%zu of %zu floats differ from MimusGemmS16FC\n",
              mismatches ? "FAIL" : "PASS", count, mismatches, out.size());
  return mismatches ? 1 : 0;
}
