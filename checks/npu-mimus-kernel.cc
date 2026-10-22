// Run the mimus portable int16 GEMM with its float read-out (MimusGemmS16FC)
// on one exported linear-layer study and compare its output with the study's
// reference bit for bit. Build against an unmodified mimus checkout:
//
//   clang++-21 -std=c++20 -O1 -I MIMUS/firmware/components/asr/include \
//     checks/npu-mimus-kernel.cc \
//     MIMUS/firmware/components/asr/src/gemm_combine_portable.cc -o kernel
//   kernel STUDY_DIRECTORY
//
// The directory holds raw little-endian files written by
// tools/npu-mimus-linear.py: x.i16 (rows x k activation codes), w.i8 (n x k
// weight codes), ws.f32, xs.f32, bias.f32 and reference.f32 (rows x n). The
// check packs the weights in the kernel's groups of eight output channels.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "mimus/asr/gemm.h"

namespace mimus::asr {
void MaybeYield() {}  // The host reference runs on one thread.
}  // namespace mimus::asr

template <class T>
static std::vector<T> load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
  if (!in.good() && !in.eof()) throw std::runtime_error("cannot read " + path);
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
  return equal == out.size() ? 0 : 1;
}
