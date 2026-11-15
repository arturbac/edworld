// edworld — the aurora over a planet whose atmosphere's kind is known (design v1, 2026-11-15); pure arithmetic, no D3D,
// testable anywhere.
//
// Only what edworld reads off the render: the kind (its colours), the planet's radius and day (whether it has a field
// of its own and how its dipole leans), the star's light (its class, in clouds3d.hlsl as aurora_star_activity here) and
// the time (how stormy it is). The game models no magnetic field, so whether a planet has one is a guess from its size
// and turn, the same planet always the same. The physics behind each number is general knowledge, not measured.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "atmosphere_kind.h"

namespace edworld
  {
  ///\brief a kind's aurora: the light low in a curtain and high in it (linear, the brightest channel 1), how much
  /// light its gas gives for the same particles (oxygen 1), and nitrogen's pink lower edge (all 0 = none)
  struct aurora_colours_t
    {
    float low[3], high[3];
    float yield;
    float edge[3];
    };

  ///\brief in the order of atmosphere_kind_e from ammonia; the "Rich" kinds by their main gas (NeonRich, ArgonRich
  /// and MethaneRich mostly nitrogen, CarbonDioxideRich mostly oxygen), as their clouds
  inline constexpr std::array<aurora_colours_t, 15> aurora_colours_table{{
    {{0.75f, 0.35f, 1.00f}, {1.00f, 0.20f, 0.25f}, 0.5f, {}},                  // Ammonia: nitrogen's violet, hydrogen's red
    {{0.70f, 0.50f, 1.00f}, {0.45f, 0.32f, 0.80f}, 0.3f, {}},                  // Argon: lilac, faint in the visible
    {{0.45f, 0.28f, 1.00f}, {0.40f, 0.30f, 1.00f}, 0.6f, {1.f, 0.3f, 0.5f}},   // ArgonRich: nitrogen
    {{0.15f, 1.00f, 0.35f}, {1.00f, 0.15f, 0.10f}, 0.5f, {}},                  // CarbonDioxide: oxygen's green and red
    {{0.15f, 1.00f, 0.35f}, {1.00f, 0.15f, 0.10f}, 1.0f, {}},                  // CarbonDioxideRich: oxygen
    {{1.00f, 0.85f, 0.30f}, {0.90f, 0.80f, 0.40f}, 0.2f, {}},                  // Helium: 587.6 nm yellow, faint
    {{0.95f, 0.40f, 1.00f}, {1.00f, 0.20f, 0.25f}, 0.4f, {}},                  // Methane: hydrogen and CH
    {{0.45f, 0.28f, 1.00f}, {0.40f, 0.30f, 1.00f}, 0.6f, {1.f, 0.3f, 0.5f}},   // MethaneRich: nitrogen
    {{1.00f, 0.45f, 0.08f}, {1.00f, 0.12f, 0.06f}, 0.8f, {}},                  // Neon: 585 and 640 nm
    {{0.45f, 0.28f, 1.00f}, {0.40f, 0.30f, 1.00f}, 0.6f, {1.f, 0.3f, 0.5f}},   // NeonRich: nitrogen
    {{0.45f, 0.28f, 1.00f}, {0.40f, 0.30f, 1.00f}, 0.6f, {1.f, 0.3f, 0.5f}},   // Nitrogen: N2+ 391/428 nm, pink edge
    {{0.15f, 1.00f, 0.35f}, {1.00f, 0.15f, 0.10f}, 1.0f, {}},                  // Oxygen: 557.7 nm low, 630 nm high
    {{1.00f, 0.75f, 0.20f}, {1.00f, 0.50f, 0.15f}, 0.5f, {}},                  // SilicateVapour: sodium's yellow
    {{1.00f, 0.15f, 0.10f}, {0.30f, 0.90f, 0.30f}, 0.4f, {}},                  // SulphurDioxide: oxygen as on Io
    {{0.20f, 1.00f, 0.35f}, {1.00f, 0.20f, 0.20f}, 0.6f, {}},                  // Water: oxygen and hydrogen
  }};

  ///\brief the aurora's colours of a kind; an unknown kind has none (yield 0)
  [[nodiscard]]
  constexpr auto aurora_colours(atmosphere_kind_e kind) noexcept -> aurora_colours_t
    {
    if(kind == atmosphere_kind_e::unknown)
      return {};
    return aurora_colours_table[static_cast<std::size_t>(kind) - 1u];
    }

  ///\brief how active the star is for its class, read off the red over green of its light in the game (VS b1 row 293,
  /// one value per class: M 1.044, K 1.032, G 1.012, F 1.003, A and hotter 0.982, brown dwarfs 0.67..2.43, median
  /// 2.04; 80 planets 2026-11-15), the bounds half way between: brown dwarfs 0.3, M 1, K 0.6, G 0.5, F 0.3, hotter
  /// 0.1 (no convective envelope). clouds3d.hlsl works the same out of the light itself
  [[nodiscard]]
  constexpr auto aurora_star_activity(float red_over_green) noexcept -> float
    {
    if(red_over_green >= 1.5f)
      return 0.3f;
    if(red_over_green >= 1.038f)
      return 1.f;
    if(red_over_green >= 1.022f)
      return 0.6f;
    if(red_over_green >= 1.0075f)
      return 0.5f;
    if(red_over_green >= 0.9925f)
      return 0.3f;
    return 0.1f;
    }

  ///\brief the star's wind at the planet: its activity times its light's level, which follows the distance only
  /// weakly (the game squeezes it: about the flux to the power 0.14), so within 0.5..2 of the level 468 the light has
  /// at the Earth's flux
  [[nodiscard]]
  inline auto aurora_star_wind(float red_over_green, float green) noexcept -> float
    {
    float const level{std::sqrt((green > 0.f ? green : 0.f) / 468.f)};
    return aurora_star_activity(red_over_green) * (level < 0.5f ? 0.5f : level > 2.f ? 2.f : level);
    }

  inline auto aurora_unit(std::uint64_t & state) noexcept -> float
    {
    state += 0x9e3779b97f4a7c15ull;
    std::uint64_t z{state};
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return static_cast<float>((z ^ (z >> 31)) >> 40) / static_cast<float>(1u << 24);
    }

  ///\brief the planet's own: whether it has a field (if not, only a faint glow over the night side, as Mars' and
  /// Venus' from the star's wind), how its dipole leans off the axis it turns on (y) as a unit vector in the planet's
  /// frame, and its storms' period (seconds)
  struct aurora_planet_t
    {
    bool field;
    float axis[3];
    float lean_deg;
    float substorm_s;
    };

  ///\brief the chance of a field of its own by the radius (0.15 under 1000 km, 0.4 to 3000 km, 0.7 past), times 0.3
  /// for a slow turn (R/L under 0.3 or a day over 10 days: a weak dynamo), times 1.3 for volcanism (a warm inside); the
  /// dipole leans 0..12 degrees on nine planets of ten, 15..60 on the tenth (as Uranus); all from the seed
  [[nodiscard]]
  inline auto aurora_planet(float radius_m, float day_s, float rossby, bool volcanic, std::uint64_t seed) noexcept -> aurora_planet_t
    {
    std::uint64_t state{seed * 0xd1b54a32d192ed03ull + 0x8bb84b93962eacc9ull};
    float chance{radius_m < 1000e3f ? 0.15f : radius_m < 3000e3f ? 0.4f : 0.7f};
    if(rossby < 0.3f or day_s > 10.f * 86400.f)
      chance *= 0.3f;
    if(volcanic)
      chance *= 1.3f;
    aurora_planet_t p{};
    p.field = aurora_unit(state) < chance;
    float const odd{aurora_unit(state)};
    float const lean{odd < 0.9f ? 12.f * aurora_unit(state) : 15.f + 45.f * aurora_unit(state)};
    float const towards{6.2831853f * aurora_unit(state)};
    float const l{lean * 0.017453293f};
    p.axis[0] = std::sin(l) * std::cos(towards);
    p.axis[1] = std::cos(l);
    p.axis[2] = std::sin(l) * std::sin(towards);
    p.lean_deg = lean;
    p.substorm_s = (1.5f + 1.5f * aurora_unit(state)) * 3600.f;
    return p;
    }

  ///\brief how stormy it is now, as the Kp index: 0..9 for each 3 h of the time (UTC, the game's own), from the
  /// planet's seed, 0..2 six times in ten, 3..4 about three, 5..6 one, 7..9 rarely; the last 20 minutes of a span go
  /// over to the next
  [[nodiscard]]
  inline auto aurora_storm_level(std::uint64_t seed, double utc_s) noexcept -> float
    {
    constexpr double span{10800.};
    auto const level_of{[seed](std::int64_t block) noexcept -> float
                          {
                            std::uint64_t state{seed * 0x9e3779b97f4a7c15ull ^ static_cast<std::uint64_t>(block) * 0xbf58476d1ce4e5b9ull};
                            float const u{aurora_unit(state)};
                            if(u < 0.6f)
                              return 3.f * u / 0.6f;
                            if(u < 0.88f)
                              return 3.f + 2.f * (u - 0.6f) / 0.28f;
                            if(u < 0.97f)
                              return 5.f + 2.f * (u - 0.88f) / 0.09f;
                            return 7.f + 2.99f * (u - 0.97f) / 0.03f;
                          }};
    std::int64_t const block{static_cast<std::int64_t>(std::floor(utc_s / span))};
    float const into{static_cast<float>(utc_s - static_cast<double>(block) * span)};
    float const now{level_of(block)};
    constexpr float over{1200.f};
    if(into < static_cast<float>(span) - over)
      return now;
    float const f{(into - (static_cast<float>(span) - over)) / over};
    return now + (level_of(block + 1) - now) * f * f * (3.f - 2.f * f);
    }

  ///\brief where a substorm is (from K 3 up): growing the first 40% of its period (a quiet arc drifting toward the
  /// equator), breaking up for 2% (a part of the arc brightening, in the shader in seconds), expanding to 60% (toward
  /// the pole, the dance at its height), recovering to the end (pulsing patches). bright: the whole aurora's level,
  /// changing over minutes, so it never flashes as one; dance 0..1, less in a weak storm; shift: the oval's boundary
  /// moved, degrees
  struct aurora_substorm_t
    {
    float bright, dance, shift;
    };

  [[nodiscard]]
  inline auto aurora_substorm(float level, float period_s, double utc_s, std::uint64_t seed) noexcept -> aurora_substorm_t
    {
    if(level < 3.f or not(period_s > 0.f))
      return {1.f, 0.15f * level / 9.f, 0.f};
    double const offset{static_cast<double>(seed % 100000u)};
    float const phase{static_cast<float>(std::fmod(utc_s + offset, static_cast<double>(period_s)) / static_cast<double>(period_s))};
    auto const smooth{[](float a, float b, float x) noexcept -> float
                        {
                          float const t{x <= a ? 0.f : x >= b ? 1.f : (x - a) / (b - a)};
                          return t * t * (3.f - 2.f * t);
                        }};
    aurora_substorm_t s{};
    if(phase < 0.4f)
      s = {1.f, 0.15f, -2.f * phase / 0.4f};
    else if(phase < 0.42f)
      {
      float const f{smooth(0.4f, 0.42f, phase)};
      s = {1.f + 1.5f * f, 0.15f + 0.85f * f, -2.f + 2.f * f};
      }
    else if(phase < 0.6f)
      s = {2.5f - 0.5f * smooth(0.42f, 0.6f, phase), 1.f, 3.f * smooth(0.42f, 0.6f, phase)};
    else
      {
      float const f{smooth(0.6f, 1.f, phase)};
      s = {2.f - 1.f * f, 0.4f - 0.25f * f, 3.f - 3.f * f};
      }
    // a weak storm dances less: three quarters of it at K 4, all from K 6
    float const strong{(level + 2.f) / 8.f};
    s.dance *= strong < 1.f ? strong : 1.f;
    return s;
    }
  }  // namespace edworld
