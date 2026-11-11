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
  /// fine shape's strength; the tint the star's light is multiplied by: off white where the kind's clouds or haze have
  /// a colour of their own (ammonia's, methane's, sulphur's, dust), barely, only to tell the kinds apart, where its ices
  /// are white (2026-11-11)
  struct atmosphere_clouds_t
    {
    float bottom, top, opacity, cover, detail;
    float tint[3];
    };

  ///\brief in the order of atmosphere_kind_e from ammonia (unknown has none)
  inline constexpr std::array<atmosphere_clouds_t, 15> atmosphere_clouds_table{{
    {0.0100f, 0.0400f, 0.30f, 0.55f, 1.0f, {0xea / 255.f, 0xdb / 255.f, 0xc0 / 255.f}},  // Ammonia
    {0.0030f, 0.0120f, 0.39f, 0.55f, 1.0f, {0xe4 / 255.f, 0xea / 255.f, 0xf4 / 255.f}},  // Argon
    {0.0026f, 0.0104f, 0.78f, 0.55f, 1.0f, {0xf0 / 255.f, 0xe8 / 255.f, 0xea / 255.f}},  // ArgonRich
    {0.0091f, 0.0366f, 0.63f, 0.55f, 1.0f, {0xe6 / 255.f, 0xee / 255.f, 0xf8 / 255.f}},  // CarbonDioxide
    {0.0031f, 0.0124f, 0.92f, 0.45f, 0.5f, {0xe8 / 255.f, 0xf0 / 255.f, 0xf8 / 255.f}},  // CarbonDioxideRich
    {0.0015f, 0.0060f, 0.30f, 0.35f, 0.5f, {0xde / 255.f, 0xe6 / 255.f, 0xf0 / 255.f}},  // Helium
    {0.0100f, 0.0400f, 0.89f, 0.55f, 1.0f, {0xe8 / 255.f, 0xcf / 255.f, 0xb0 / 255.f}},  // Methane
    {0.0026f, 0.0103f, 0.72f, 0.45f, 0.5f, {0xee / 255.f, 0xdd / 255.f, 0xcb / 255.f}},  // MethaneRich
    {0.0015f, 0.0060f, 0.30f, 0.45f, 0.5f, {0xec / 255.f, 0xe4 / 255.f, 0xf0 / 255.f}},  // Neon
    {0.0017f, 0.0067f, 0.58f, 0.55f, 1.0f, {0xf0 / 255.f, 0xe8 / 255.f, 0xea / 255.f}},  // NeonRich
    {0.0027f, 0.0109f, 0.50f, 0.55f, 1.0f, {0xf0 / 255.f, 0xe8 / 255.f, 0xea / 255.f}},  // Nitrogen
    {0.0035f, 0.0140f, 0.79f, 0.45f, 0.5f, {0xe8 / 255.f, 0xf0 / 255.f, 0xf8 / 255.f}},  // Oxygen
    {0.0038f, 0.0152f, 0.81f, 0.55f, 1.0f, {0xb8 / 255.f, 0xa8 / 255.f, 0x98 / 255.f}},  // SilicateVapour
    {0.0032f, 0.0126f, 0.30f, 0.45f, 0.5f, {0xeb / 255.f, 0xe0 / 255.f, 0xb4 / 255.f}},  // SulphurDioxide
    {0.0100f, 0.0400f, 0.98f, 0.45f, 0.5f, {1.f, 1.f, 1.f}},                             // Water
  }};

  ///\brief a kind's clouds, its share of the opacity times intensity (clouds_physics_intensity, 1 = as the physics
  /// gives, 0 = none), at most all of it; over 1 the cover too comes up to a condensing gas's, all of it at 2
  [[nodiscard]]
  constexpr auto atmosphere_clouds(atmosphere_kind_e kind, float intensity) noexcept -> atmosphere_clouds_t
    {
    if(kind == atmosphere_kind_e::unknown)
      return {};
    atmosphere_clouds_t c{atmosphere_clouds_table[static_cast<std::size_t>(kind) - 1u]};
    float const i{intensity > 0.f ? intensity : 0.f};
    c.opacity = c.opacity * i < 1.f ? c.opacity * i : 1.f;
    if(i > 1.f)
      c.cover += (0.55f - c.cover) * (i - 1.f < 1.f ? i - 1.f : 1.f);
    return c;
    }

  ///\brief the clouds with their tint's distance from white times strength (clouds_tint, 0 = white, 1 = the table's,
  /// 2 = twice as far), each channel within 0..1
  [[nodiscard]]
  constexpr auto atmosphere_clouds_tinted(atmosphere_clouds_t c, float strength) noexcept -> atmosphere_clouds_t
    {
    float const s{strength > 0.f ? strength : 0.f};
    for(float & t: c.tint)
      {
      float const v{1.f - (1.f - t) * s};
      t = v < 0.f ? 0.f : v;
      }
    return c;
    }

  ///\brief what the storms of a kind's planet follow: its typical landable planet's surface temperature (K), the main
  /// gas's molar mass (g/mol), the median radius (km) and the median R/L (below) of the kind, from the scans
  /// (2026-11-10, both commanders' galaxies)
  struct atmosphere_physics_t
    {
    float kelvin, molar, radius_km, rossby_median;
    };

  inline constexpr std::array<atmosphere_physics_t, 15> atmosphere_physics_table{{
    {165.f, 17.f, 1351.f, 0.05f},    // Ammonia
    {69.f, 40.f, 2953.f, 0.09f},     // Argon
    {83.f, 31.f, 4143.f, 1.04f},     // ArgonRich
    {175.f, 44.f, 1115.f, 0.05f},    // CarbonDioxide
    {230.f, 34.f, 3738.f, 0.88f},    // CarbonDioxideRich
    {28.f, 4.f, 7711.f, 3.13f},      // Helium
    {93.f, 16.f, 654.f, 0.02f},      // Methane
    {140.f, 26.f, 3942.f, 1.46f},    // MethaneRich
    {31.f, 20.2f, 5435.f, 3.70f},    // Neon
    {63.f, 25.f, 5529.f, 3.62f},     // NeonRich
    {79.f, 28.f, 3399.f, 1.61f},     // Nitrogen
    {181.f, 32.f, 3070.f, 0.24f},    // Oxygen
    {1808.f, 60.f, 4330.f, 0.41f},   // SilicateVapour
    {270.f, 64.f, 2937.f, 0.12f},    // SulphurDioxide
    {445.f, 18.f, 436.f, 0.02f},     // Water
  }};

  ///\brief how many storms the planet holds across: its radius over the Rossby radius of deformation
  /// L = sqrt(R* T / M) / f, f = 2 omega sin 45 degrees (gravity cancels out). day_s 0 = not known: the kind's median
  /// R/L times this radius over the kind's median radius
  [[nodiscard]]
  inline auto rossby_ratio(atmosphere_kind_e kind, float radius_m, float day_s) noexcept -> float
    {
    if(kind == atmosphere_kind_e::unknown or not(radius_m > 0.f))
      return 0.f;
    atmosphere_physics_t const & p{atmosphere_physics_table[static_cast<std::size_t>(kind) - 1u]};
    if(not(day_s > 0.f))
      return p.rossby_median * radius_m / (p.radius_km * 1000.f);
    float const f{2.f * (6.2831853f / day_s) * 0.70710678f};
    return radius_m * f / std::sqrt(8.314f * p.kelvin / (p.molar / 1000.f));
    }

  struct planet_turn_t
    {
    bool ok;        ///< a small turn about y alone: not across the game's change of its frame
    double day_s;   ///< the day it gives
    double about_y; ///< the turn, radians, + about +y
    };

  ///\brief the planet's turn between two reads of the shell's object matrix (VS b0 rows 9..11, xyz, each row of unit
  /// length) seconds apart: the old rows' transpose times the new, in the planet's frame; the angle from the parts off
  /// the diagonal, as one this small is lost in the trace's float
  [[nodiscard]]
  inline auto planet_turn(float const (&old_rows)[9], float const (&new_rows)[9], double seconds) noexcept -> planet_turn_t
    {
    auto const m{[&](std::uint32_t i, std::uint32_t j) noexcept -> double
                   {
                     double v{};
                     for(std::uint32_t k{}; k != 3u; ++k)
                       v += static_cast<double>(old_rows[k * 3u + i]) * new_rows[k * 3u + j];
                     return v;
                   }};
    double const about_y{0.5 * (m(0, 2) - m(2, 0))};
    double const off{std::fabs(m(1, 2) - m(2, 1)) + std::fabs(m(0, 1) - m(1, 0))};
    double const a{std::fabs(about_y)};
    if(not(seconds > 0.) or a < 1e-9 or a > 0.05 or off > 0.2 * a + 1e-6)
      return {false, 0., about_y};
    return {true, 6.283185307179586 * seconds / a, about_y};
    }

  ///\brief a storm: its centre on the unit sphere in the planet's frame (y the axis it turns on), its size as an angle
  /// (radians), how far it twists the clouds at its middle (radians, + the way the planet turns)
  struct cyclone_t
    {
    float x, y, z, size, twist;
    };

  inline constexpr std::size_t cyclones_most{8u};

  struct cyclone_set_t
    {
    std::uint32_t count;
    std::array<cyclone_t, cyclones_most> at;
    };

  ///\brief the storms of design v1 (2026-11-10) for this R/L, laid out from the planet's seed (the same planet always
  /// the same): under 0.3 none; to 1 one to three large ones and one at each pole; to 4 three to six of a medium size
  /// between 20 and 60 degrees; more, six to eight small ones. A storm turns the way the planet does on the half the
  /// planet turns about from below (spin +1: about +y), the other way on the other half
  [[nodiscard]]
  inline auto cyclones_of(float rossby, std::uint64_t seed, float spin) noexcept -> cyclone_set_t
    {
    cyclone_set_t set{};
    if(not(rossby >= 0.3f))
      return set;
    std::uint64_t state{seed * 0x9e3779b97f4a7c15ull + 0x632be59bd9b4e019ull};
    auto const next{[&state]() noexcept -> float
                      {
                        state += 0x9e3779b97f4a7c15ull;
                        std::uint64_t z{state};
                        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
                        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
                        return static_cast<float>((z ^ (z >> 31)) >> 40) / static_cast<float>(1u << 24);
                      }};
    float const sense{spin < 0.f ? -1.f : 1.f};
    auto const add{[&](float latitude, float longitude, float size, float twist) noexcept
                     {
                       if(set.count == cyclones_most)
                         return;
                       float const c{std::cos(latitude)};
                       set.at[set.count++] = cyclone_t{c * std::cos(longitude), std::sin(latitude), c * std::sin(longitude), size,
                                                       (latitude < 0.f ? -twist : twist) * sense};
                     }};
    std::uint32_t n{};
    float low{}, high{}, size_from{}, size_to{}, twist{};
    if(rossby < 1.f)
      {
      n = 1u + static_cast<std::uint32_t>(next() * 2.999f);
      low = 15.f, high = 50.f, size_from = 0.25f, size_to = 0.35f, twist = 5.f;
      }
    else if(rossby < 4.f)
      {
      n = 3u + static_cast<std::uint32_t>(next() * 3.999f);
      low = 20.f, high = 60.f, size_from = 0.12f, size_to = 0.2f, twist = n <= 3u ? 5.f : 3.f;
      }
    else
      {
      n = 6u + static_cast<std::uint32_t>(next() * 2.999f);
      low = 20.f, high = 60.f, size_from = 0.08f, size_to = 0.08f, twist = 3.f;
      }
    for(std::uint32_t i{}; i != n; ++i)
      {
      float const latitude{(low + (high - low) * next()) * 0.017453293f * (next() < 0.5f ? -1.f : 1.f)};
      add(latitude, next() * 6.2831853f, size_from + (size_to - size_from) * next(), twist);
      }
    if(rossby < 1.f)
      {
      add(1.5707963f, 0.f, 0.15f, 5.f);
      add(-1.5707963f, 0.f, 0.15f, 5.f);
      }
    return set;
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
