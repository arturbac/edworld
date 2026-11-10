// edworld — the atmosphere's kind from the game's own table of the light through it; pure arithmetic, no D3D, testable
// anywhere.
//
// The game's shell of the atmosphere reads a table (its PS t0, R11G11B10, 256x512 or 128x256). The hue of its upper
// half where it is bright, red over green and blue over green, is the same for one kind of atmosphere at every star and
// every planet measured (2026-11-10: 24 planets, all 15 kinds found on landable planets, both commanders; Argon at K6, G9
// and M7 stars 0.398/2.500 to 0.001; Argon with 15% nitrogen as pure Argon): it follows the kind the journal names, not
// the mix of gases. The kinds stand apart by 0.010 or more (CarbonDioxide and Methane the nearest), ten times the spread
// within a kind; eleven kinds were seen on one planet only, so their spread is taken from CarbonDioxide and Argon.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace edworld
  {
  enum struct atmosphere_kind_e : std::uint8_t
    {
    unknown,
    ammonia,
    argon,
    argon_rich,
    carbon_dioxide,
    carbon_dioxide_rich,
    helium,
    methane,
    methane_rich,
    neon,
    neon_rich,
    nitrogen,
    oxygen,
    silicate_vapour,
    sulphur_dioxide,
    water,
    };

  struct atmosphere_hue_t
    {
    bool ok;
    float r_g, b_g;
    };

  struct atmosphere_print_t
    {
    atmosphere_kind_e kind;
    std::string_view name;  ///< the journal's AtmosphereType
    float r_g, b_g;
    };

  /// measured 2026-11-10 (the median over each kind's planets, four digits)
  inline constexpr std::array<atmosphere_print_t, 15> atmosphere_prints{{
    {atmosphere_kind_e::ammonia, "Ammonia", 0.4897f, 1.4194f},
    {atmosphere_kind_e::argon, "Argon", 0.3981f, 2.5000f},
    {atmosphere_kind_e::argon_rich, "ArgonRich", 0.5158f, 2.7826f},
    {atmosphere_kind_e::carbon_dioxide, "CarbonDioxide", 0.6199f, 1.9350f},
    {atmosphere_kind_e::carbon_dioxide_rich, "CarbonDioxideRich", 0.6479f, 1.8755f},
    {atmosphere_kind_e::helium, "Helium", 0.8922f, 2.7077f},
    {atmosphere_kind_e::methane, "Methane", 0.6190f, 1.9459f},
    {atmosphere_kind_e::methane_rich, "MethaneRich", 0.6286f, 1.9213f},
    {atmosphere_kind_e::neon, "Neon", 0.8035f, 1.4802f},
    {atmosphere_kind_e::neon_rich, "NeonRich", 0.7216f, 1.6897f},
    {atmosphere_kind_e::nitrogen, "Nitrogen", 0.6953f, 1.7551f},
    {atmosphere_kind_e::oxygen, "Oxygen", 0.7320f, 1.6538f},
    {atmosphere_kind_e::silicate_vapour, "SilicateVapour", 0.7692f, 1.2273f},
    {atmosphere_kind_e::sulphur_dioxide, "SulphurDioxide", 0.7500f, 1.6180f},
    {atmosphere_kind_e::water, "Water", 0.8268f, 1.4177f},
  }};

  /// the farthest a hue may lie from its kind's, in either ratio: four times the spread within a kind, under half the
  /// nearest two kinds' distance
  inline constexpr float atmosphere_tolerance{0.004f};

  inline constexpr std::uint32_t dxgi_r11g11b10_float{26u};

  ///\brief one channel of a small float without a sign (R11G11B10): mantissa bits, then five of exponent
  [[nodiscard]]
  inline auto small_float(std::uint32_t v, std::uint32_t mantissa) noexcept -> float
    {
    std::uint32_t const e{(v >> mantissa) & 31u};
    std::uint32_t const m{v & ((1u << mantissa) - 1u)};
    float const f{static_cast<float>(m) / static_cast<float>(1u << mantissa)};
    return e == 0u ? std::ldexp(f, -14) : std::ldexp(1.f + f, static_cast<int>(e) - 15);
    }

  ///\brief the middle of n values (the mean of the middle two when n is even), reordering them
  [[nodiscard]]
  inline auto median_of(float * v, std::size_t n) noexcept -> float
    {
    if(n == 0u)
      return 0.f;
    std::size_t const half{n / 2u};
    std::nth_element(v, v + half, v + n);
    float const upper{v[half]};
    if(n % 2u)
      return upper;
    return 0.5f * (upper + *std::max_element(v, v + half));
    }

  ///\brief the hue of the table's upper half where its green is over half its brightest: the median of red over green
  /// and of blue over green. rows: the mapped table (pitch in bytes); scratch: two of capacity floats each, the
  /// table's upper half at most (not ok when there is more or nothing bright)
  [[nodiscard]]
  inline auto atmosphere_hue(std::uint8_t const * rows, std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
                             float * scratch_r, float * scratch_b, std::size_t capacity) noexcept -> atmosphere_hue_t
    {
    std::uint32_t const top{height / 2u};
    if(not rows or width == 0u or top == 0u or pitch < width * 4u or std::size_t{width} * top > capacity)
      return {};
    auto const texel{[&](std::uint32_t x, std::uint32_t y) noexcept -> std::uint32_t
                       {
                         std::uint32_t v;
                         std::memcpy(&v, rows + std::size_t{y} * pitch + x * 4u, sizeof v);
                         return v;
                       }};
    float brightest{};
    for(std::uint32_t y{}; y != top; ++y)
      for(std::uint32_t x{}; x != width; ++x)
        brightest = std::max(brightest, small_float((texel(x, y) >> 11) & 0x7ffu, 6u));
    if(not(brightest > 0.f))
      return {};
    std::size_t n{};
    for(std::uint32_t y{}; y != top; ++y)
      for(std::uint32_t x{}; x != width; ++x)
        {
        std::uint32_t const v{texel(x, y)};
        float const g{small_float((v >> 11) & 0x7ffu, 6u)};
        if(not(g > 0.5f * brightest))
          continue;
        scratch_r[n] = small_float(v & 0x7ffu, 6u) / g;
        scratch_b[n] = small_float((v >> 22) & 0x3ffu, 5u) / g;
        ++n;
        }
    return {true, median_of(scratch_r, n), median_of(scratch_b, n)};
    }

  ///\brief the kind whose hue is nearest, when within the tolerance in both ratios; unknown otherwise
  [[nodiscard]]
  constexpr auto atmosphere_kind_of(atmosphere_hue_t const & hue, float tolerance = atmosphere_tolerance) noexcept
    -> atmosphere_print_t const *
    {
    if(not hue.ok)
      return nullptr;
    atmosphere_print_t const * best{};
    float best_d{};
    for(atmosphere_print_t const & p: atmosphere_prints)
      {
      float const dr{hue.r_g > p.r_g ? hue.r_g - p.r_g : p.r_g - hue.r_g};
      float const db{hue.b_g > p.b_g ? hue.b_g - p.b_g : p.b_g - hue.b_g};
      float const d{dr > db ? dr : db};
      if(d <= tolerance and (not best or d < best_d))
        {
        best = &p;
        best_d = d;
        }
      }
    return best;
    }
  }  // namespace edworld
