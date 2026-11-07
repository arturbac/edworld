// edworld — the GPU time of one kind of work summed up over a window of a few seconds: pure arithmetic, no D3D,
// testable anywhere. The times come from D3D11 timestamp queries read back a few frames late; the pixels from a
// pipeline statistics query (pixel shader invocations), so a time can be told per million pixels shaded.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace edworld
  {
  ///\brief the spans of one window: each time kept (up to the array's size) for the median, the sums of all of them
  struct gpu_span_window_t
    {
    std::array<float, 1024> ms{};
    std::uint32_t kept{};
    std::uint32_t count{};
    double ms_sum{};
    double megapixel_sum{};
    float ms_max{};
    };

  ///\brief what a window comes to; all zero for an empty one
  struct gpu_span_summary_t
    {
    std::uint32_t count{};
    float ms_median{};
    float ms_mean{};
    float ms_max{};
    float megapixels_mean{};
    float ms_per_megapixel{};
    };

  constexpr auto add_span(gpu_span_window_t & w, float ms, float megapixels) noexcept -> void
    {
    if(w.kept != w.ms.size())
      w.ms[w.kept++] = ms;
    ++w.count;
    w.ms_sum += ms;
    w.megapixel_sum += megapixels;
    w.ms_max = std::max(w.ms_max, ms);
    }

  [[nodiscard]]
  constexpr auto span_summary(gpu_span_window_t w) noexcept -> gpu_span_summary_t
    {
    if(w.count == 0u)
      return {};
    auto const middle{w.ms.begin() + w.kept / 2u};
    std::nth_element(w.ms.begin(), middle, w.ms.begin() + w.kept);
    return gpu_span_summary_t{
      .count = w.count,
      .ms_median = *middle,
      .ms_mean = static_cast<float>(w.ms_sum / w.count),
      .ms_max = w.ms_max,
      .megapixels_mean = static_cast<float>(w.megapixel_sum / w.count),
      .ms_per_megapixel = w.megapixel_sum > 0. ? static_cast<float>(w.ms_sum / w.megapixel_sum) : 0.f
    };
    }
  }  // namespace edworld
