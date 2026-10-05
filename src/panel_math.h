// edworld — pure arithmetic on what a panel draw carries; no D3D, testable anywhere.
#pragma once

#include <cmath>
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
  // ---- the panel family's vertex (VB1, stride 40): what the list under the jump panel is placed by ----
  // Read off the game's draws on 2026-10-05 (tools/geometry_dump.py): bytes 0..15 the packed position (pva, EDVR's
  // decode), bytes 16..19 the surface coordinate as two unorm16 halves, u = (h / 32767 - 1) * 16 - the vertex UV
  // EDVR's transcription scales by 16 (its 0.000031 is the shader's 1/32767, rounded in the listing).
  inline constexpr std::uint32_t vertex_stride{40};

  struct vec3_t
    {
    float x, y, z;
    };

  ///\brief the local position packed in a vertex's first three words (EDVR's edvrDecodePos)
  [[nodiscard]]
  inline auto decode_local(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept -> vec3_t
    {
    std::uint32_t const fmt{(z >> 24u) & 0x7Fu};
    if(fmt == 64u)
      {
      float const lx{static_cast<float>(x & 0xFFFFu)}, hx{static_cast<float>(x >> 16u)};
      float const ly{static_cast<float>(y & 0xFFFFu)}, hy{static_cast<float>(y >> 16u)};
      float const s{std::exp2(hy * 0.000244f) - 1.f};
      return {(lx * 0.000031f - 1.f) * s, (hx * 0.000031f - 1.f) * s, (ly * 0.000031f - 1.f) * s};
      }
    std::uint32_t const sh{(z >> 24u) & 31u};
    float const scl{1.f / static_cast<float>((1u << ((20u - fmt) & 31u)) - 1u)};
    float const offs{static_cast<float>(1u << sh)};
    std::uint32_t const xb{x & 0x1FFFFFu};
    std::uint32_t const yb{(x >> 21u) + ((y & 0x3FFu) << 11u)};
    std::uint32_t const zb{(y >> 10u) & 0x1FFFFFu};
    return {static_cast<float>(xb) * scl - offs, static_cast<float>(yb) * scl - offs, static_cast<float>(zb) * scl - offs};
    }

  ///\brief the surface coordinate (0..1) of a vertex, from its fifth word
  [[nodiscard]]
  inline auto decode_uv(std::uint32_t word, float & u, float & v) noexcept -> void
    {
    u = (static_cast<float>(word & 0xFFFFu) / 32767.f - 1.f) * 16.f;
    v = (static_cast<float>(word >> 16u) / 32767.f - 1.f) * 16.f;
    }

  ///\brief how a flat panel maps its local x, y to its surface's pixels: px = a x + b y + c, py = d x + e y + f;
  /// z is the panel's plane
  struct panel_map_t
    {
    float a, b, c, d, e, f;
    float z;
    };

  ///\brief the map through three of a panel's corners (local, pixels); none when they are in a line
  [[nodiscard]]
  inline auto map_from(vec3_t const (&p)[3], float const (&px)[3][2]) noexcept -> std::optional<panel_map_t>
    {
    float const x1{p[1].x - p[0].x}, y1{p[1].y - p[0].y}, x2{p[2].x - p[0].x}, y2{p[2].y - p[0].y};
    float const det{x1 * y2 - x2 * y1};
    if(not(std::fabs(det) > 1e-9f))
      return std::nullopt;
    auto solve = [&](float q0, float q1, float q2, float & ka, float & kb, float & kc)
      {
      float const r1{q1 - q0}, r2{q2 - q0};
      ka = (r1 * y2 - r2 * y1) / det;
      kb = (x1 * r2 - x2 * r1) / det;
      kc = q0 - ka * p[0].x - kb * p[0].y;
      };
    panel_map_t m{};
    solve(px[0][0], px[1][0], px[2][0], m.a, m.b, m.c);
    solve(px[0][1], px[1][1], px[2][1], m.d, m.e, m.f);
    m.z = (p[0].z + p[1].z + p[2].z) / 3.f;
    return m;
    }

  ///\brief a surface pixel back to the panel's local plane; none when the map cannot be inverted
  [[nodiscard]]
  inline auto local_of(panel_map_t const & m, float px, float py) noexcept -> std::optional<vec3_t>
    {
    float const det{m.a * m.e - m.b * m.d};
    if(not(std::fabs(det) > 1e-12f))
      return std::nullopt;
    float const rx{px - m.c}, ry{py - m.f};
    return vec3_t{(rx * m.e - ry * m.b) / det, (m.a * ry - m.d * rx) / det, m.z};
    }
  }  // namespace edworld
