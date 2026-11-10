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

  ///\brief the clouds of a kind (design v1.1, 2026-11-10): the layer's floor and top over the surface as parts of the
  /// radius, from the scale height R T / (M g) of the kind's typical planet pressed into 0.006..0.04; how much of
  /// clouds_opacity they get, from the kind's typical pressure (at least 0.3); the cover, 0.55 where the main gas
  /// condenses at the kind's typical temperature, 0.45 a haze where it does not, 0.35 for Helium, which never does; the
  /// fine shape's strength; the tint the star's light is multiplied by
  struct atmosphere_clouds_t
    {
    float bottom, top, opacity, cover, detail;
    float tint[3];
    };

  ///\brief in the order of atmosphere_kind_e from ammonia (unknown has none)
  inline constexpr std::array<atmosphere_clouds_t, 15> atmosphere_clouds_table{{
    {0.0100f, 0.0400f, 0.30f, 0.55f, 1.0f, {0xf3 / 255.f, 0xe9 / 255.f, 0xd2 / 255.f}},  // Ammonia
    {0.0030f, 0.0120f, 0.39f, 0.55f, 1.0f, {0xf2 / 255.f, 0xf2 / 255.f, 0xf5 / 255.f}},  // Argon
    {0.0026f, 0.0104f, 0.78f, 0.55f, 1.0f, {0xf2 / 255.f, 0xf2 / 255.f, 0xf5 / 255.f}},  // ArgonRich
    {0.0091f, 0.0366f, 0.63f, 0.55f, 1.0f, {0xf0 / 255.f, 0xf4 / 255.f, 0xfa / 255.f}},  // CarbonDioxide
    {0.0031f, 0.0124f, 0.92f, 0.45f, 0.5f, {1.f, 1.f, 1.f}},                             // CarbonDioxideRich
    {0.0015f, 0.0060f, 0.30f, 0.35f, 0.5f, {0xdd / 255.f, 0xe3 / 255.f, 0xea / 255.f}},  // Helium
    {0.0100f, 0.0400f, 0.89f, 0.55f, 1.0f, {0xf5 / 255.f, 0xf0 / 255.f, 0xe6 / 255.f}},  // Methane
    {0.0026f, 0.0103f, 0.72f, 0.45f, 0.5f, {0xf5 / 255.f, 0xe6 / 255.f, 0xd0 / 255.f}},  // MethaneRich
    {0.0015f, 0.0060f, 0.30f, 0.45f, 0.5f, {0xe8 / 255.f, 0xec / 255.f, 0xf0 / 255.f}},  // Neon
    {0.0017f, 0.0067f, 0.58f, 0.55f, 1.0f, {1.f, 1.f, 1.f}},                             // NeonRich
    {0.0027f, 0.0109f, 0.50f, 0.55f, 1.0f, {1.f, 1.f, 1.f}},                             // Nitrogen
    {0.0035f, 0.0140f, 0.79f, 0.45f, 0.5f, {1.f, 1.f, 1.f}},                             // Oxygen
    {0.0038f, 0.0152f, 0.81f, 0.55f, 1.0f, {0xb8 / 255.f, 0xb2 / 255.f, 0xaa / 255.f}},  // SilicateVapour
    {0.0032f, 0.0126f, 0.30f, 0.45f, 0.5f, {0xed / 255.f, 0xe3 / 255.f, 0xa8 / 255.f}},  // SulphurDioxide
    {0.0100f, 0.0400f, 0.98f, 0.45f, 0.5f, {1.f, 1.f, 1.f}},                             // Water
  }};

  ///\brief a kind's clouds, its share of the opacity times intensity (clouds_physics_intensity, 1 = as the physics
  /// gives, 0 = none), at most all of it
  [[nodiscard]]
  constexpr auto atmosphere_clouds(atmosphere_kind_e kind, float intensity) noexcept -> atmosphere_clouds_t
    {
    if(kind == atmosphere_kind_e::unknown)
      return {};
    atmosphere_clouds_t c{atmosphere_clouds_table[static_cast<std::size_t>(kind) - 1u]};
    float const i{intensity > 0.f ? intensity : 0.f};
    c.opacity = c.opacity * i < 1.f ? c.opacity * i : 1.f;
    return c;
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
