// CPU side of an exact mimus linear layer computed by the NPU as base-16
// digit partial sums (docs/npu-mimus-linear.md).
//
// MimusNpuDigitSplit writes each int16 activation code x as four digits,
// x = d0 + 16 d1 + 256 d2 + 4096 d3, with d0, d1, d2 in [-8, 7] and d3 in
// [-8, 8], into one INT8 input per reduction chunk. Input c holds rows
// 4r + j = digit j of activation row r over chunk c, chunks[c] bytes each.
//
// MimusNpuDigitCombine reads the INT16 output of each chunk, row 4r + j and
// column n = P[c][4r + j][n], forms S[r][n] = sum_j 16^j sum_c P[c][4r + j][n]
// and applies the read-out of MimusGemmS16FC in the same float32 order:
// acc = 0; acc += float(S) * ws[n] * xs[r]; out = acc + bias[n].
// Any INT16 partial has |P| <= 32,768, so with at most eight chunks
// |S| <= 8 * 32,768 * 4,369 < 2^31 and the sums use int32 lanes even if the
// device returned out-of-contract values.
//
// Build for the host with checks/npu-mimus-kernel.cc, or for AArch64 with
// the mimus backend's flags to count instructions (tools/npu-mimus-cost.py).
#include <cstddef>
#include <cstdint>

namespace {
constexpr uint32_t kDigits = 4;
constexpr uint32_t kMaxChunks = 8;
constexpr uint32_t kMaxChunk = 32;
static_assert(int64_t{kMaxChunks} * 32768 * (1 + 16 + 256 + 4096) < (int64_t{1} << 31),
              "recombined sums must fit int32 lanes");
constexpr uint32_t kLanes = 8;
}  // namespace

extern "C" int MimusNpuDigitSplit(const int16_t* x, uint32_t rows, uint32_t k, const uint32_t* chunks,
                                  uint32_t count, int8_t* const* inputs) {
  if (count == 0 || count > kMaxChunks) return 0;
  uint32_t total = 0;
  for (uint32_t c = 0; c < count; ++c) {
    if (chunks[c] == 0 || chunks[c] > kMaxChunk) return 0;
    total += chunks[c];
  }
  if (total != k) return 0;
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0, lo = 0; c < count; lo += chunks[c], ++c) {
      const uint32_t length = chunks[c];
      const int16_t* __restrict source = x + size_t{r} * k + lo;
      int8_t* __restrict d0 = inputs[c] + size_t{r} * kDigits * length;
      int8_t* __restrict d1 = d0 + length;
      int8_t* __restrict d2 = d1 + length;
      int8_t* __restrict d3 = d2 + length;
      for (uint32_t i = 0; i < length; ++i) {
        // Each step removes a digit in [-8, 7]; v - d is a multiple of 16.
        int32_t v = source[i];
        const int32_t a = ((v + 8) & 15) - 8;
        v = (v - a) >> 4;
        const int32_t b = ((v + 8) & 15) - 8;
        v = (v - b) >> 4;
        const int32_t e = ((v + 8) & 15) - 8;
        d0[i] = static_cast<int8_t>(a);
        d1[i] = static_cast<int8_t>(b);
        d2[i] = static_cast<int8_t>(e);
        d3[i] = static_cast<int8_t>((v - e) >> 4);
      }
    }
  }
  return 1;
}

// n must be a multiple of eight, as for every mimus linear layer.
extern "C" int MimusNpuDigitCombine(const int16_t* const* partials, uint32_t count, uint32_t rows,
                                    uint32_t n, const float* ws, const float* xs, const float* bias,
                                    float* out) {
  if (count == 0 || count > kMaxChunks || n % kLanes) return 0;
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t n0 = 0; n0 < n; n0 += kLanes) {
      int32_t sum[kLanes] = {};
      for (uint32_t c = 0; c < count; ++c)
        for (uint32_t j = 0; j < kDigits; ++j) {
          const int16_t* __restrict p = partials[c] + (size_t{r} * kDigits + j) * n + n0;
          for (uint32_t i = 0; i < kLanes; ++i) sum[i] += int32_t{p[i]} * (int32_t{1} << (4 * j));
        }
      float* __restrict o = out + size_t{r} * n + n0;
      for (uint32_t i = 0; i < kLanes; ++i) {
        float acc = 0;
        acc += static_cast<float>(sum[i]) * ws[n0 + i] * xs[r];
        o[i] = acc + (bias != nullptr ? bias[n0 + i] : 0.0f);
      }
    }
  }
  return 1;
}
