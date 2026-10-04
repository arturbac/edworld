// edworld — pure arithmetic on what a panel draw carries; no D3D, testable anywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace edworld
  {
  ///\brief the hash EDVR names shaders by: FNV-1a 64 over the whole bytecode
  [[nodiscard]]
  constexpr auto fnv1a64(std::uint8_t const * data, std::size_t bytes) noexcept -> std::uint64_t
    {
    std::uint64_t h{1469598103934665603ull};
    for(std::size_t i{}; i != bytes; ++i)
      {
      h ^= data[i];
      h *= 1099511628211ull;
      }
    return h;
    }

  struct ndc_t
    {
    float x;
    float y;
    float w;
    };

  ///\brief clip -> normalised device coordinates; none behind the eye
  [[nodiscard]]
  inline auto clip_to_ndc(float const (&clip)[4]) noexcept -> std::optional<ndc_t>
    {
    if(not(clip[3] > 1e-4f))
      return std::nullopt;
    return ndc_t{clip[0] / clip[3], clip[1] / clip[3], clip[3]};
    }

  ///\brief rows 4..7 of cb0 transform a point with one dp4 each; the local origin (0,0,0,1) lands on their w column
  inline auto anchor_from_cb0(float const (&cb0)[12][4], float (&clip)[4]) noexcept -> void
    {
    for(int r{}; r != 4; ++r)
      clip[r] = cb0[4 + r][3];
    }

  ///\brief a point in the panel's local space through rows 4..7
  inline auto project_local(float const (&cb0)[12][4], float x, float y, float z, float (&clip)[4]) noexcept -> void
    {
    for(int r{}; r != 4; ++r)
      clip[r] = cb0[4 + r][0] * x + cb0[4 + r][1] * y + cb0[4 + r][2] * z + cb0[4 + r][3];
    }

  ///\brief the instance record the panel family reads at VS t33 (stride 336): uniform scale @4, orientation as
  /// 4 x unorm16 @8, position (float3) @16 - EDVR's decode (fss_panel_rect.cpp, fss_panel_vs.h)
  struct record_t
    {
    float scale;
    float orientation[4];
    float position[3];
    };

  [[nodiscard]]
  inline auto decode_record(std::uint8_t const * r) noexcept -> record_t
    {
    record_t out{};
    std::uint32_t xy{}, zw{};
    std::memcpy(&out.scale, r + 4, 4);
    std::memcpy(&xy, r + 8, 4);
    std::memcpy(&zw, r + 12, 4);
    std::memcpy(out.position, r + 16, 12);
    out.orientation[0] = static_cast<float>(xy & 0xFFFFu) * 0.000031f - 1.0f;
    out.orientation[1] = static_cast<float>(xy >> 16u) * 0.000031f - 1.0f;
    out.orientation[2] = static_cast<float>(zw & 0xFFFFu) * 0.000031f - 1.0f;
    out.orientation[3] = static_cast<float>(zw >> 16u) * 0.000031f - 1.0f;
    return out;
    }

  ///\brief "81216C77F90DEDD6" -> value; hex only, 1..16 digits
  [[nodiscard]]
  constexpr auto parse_hash(std::string_view text) noexcept -> std::optional<std::uint64_t>
    {
    while(not text.empty() and (text.front() == ' ' or text.front() == '\t'))
      text.remove_prefix(1);
    while(not text.empty() and (text.back() == ' ' or text.back() == '\t' or text.back() == '\r'))
      text.remove_suffix(1);
    if(text.empty() or text.size() > 16)
      return std::nullopt;
    std::uint64_t v{};
    for(char const c: text)
      {
      std::uint64_t d{};
      if(c >= '0' and c <= '9')
        d = static_cast<std::uint64_t>(c - '0');
      else if(c >= 'a' and c <= 'f')
        d = static_cast<std::uint64_t>(c - 'a' + 10);
      else if(c >= 'A' and c <= 'F')
        d = static_cast<std::uint64_t>(c - 'A' + 10);
      else
        return std::nullopt;
      v = (v << 4) | d;
      }
    return v;
    }
  }  // namespace edworld
