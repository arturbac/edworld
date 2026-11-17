// edworld (PoC, 2026-11-17): crystals of the air's own gas falling near the ground, under the clouds, on foot - only
// where the kind read off the render says its gas condenses at the planet's cold (research of 2026-11-16: an ammonia
// atmosphere lies on its saturation curve on most planets, a methane one near it; both below their gas' triple point,
// so crystals, no rain). No journal: the kind from the colour, the cold ammonia from its second colour (a cold,
// oversaturated planet, Spoihee IT-F d12-727 6 c and 7 e), on foot outdoors and the gravity from Status.json.
#pragma once

#include "atmosphere_kind.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace edworld
  {
  ///\brief how many crystals: under a full cloud, and in a clear sky (as a part of the most, which snow_gain scales);
  /// and the kind's air near the ground, kg/m3 (its typical planet's pressure and temperature, the scans' medians of
  /// 2026-11-16), which sets how fast they fall
  struct snow_t
    {
    float under_clouds;
    float clear;
    float air_density;
    };

  ///\brief the most crystals: the share of the 0.8 m cells that hold one under a full cloud at 1 (all of them was a
  /// blizzard at 6 c, 2026-11-17, one in 20 hardly seen)
  inline constexpr float snow_most{0.15f};

  ///\brief by the kind: ammonia light under the clouds, its cold second colour steadily and a little in a clear sky
  /// (diamond dust); methane light and denser (a heavy column of air, little gravity); any other none
  [[nodiscard]]
  constexpr auto snow_of(atmosphere_kind_e kind, bool cold) noexcept -> snow_t
    {
    switch(kind)
      {
      // 166 Pa, 165 K, 17 g/mol; 6006 Pa, 95 K, 16 g/mol
      case atmosphere_kind_e::ammonia: return cold ? snow_t{1.f, 0.05f, 0.00206f} : snow_t{0.5f, 0.f, 0.00206f};
      case atmosphere_kind_e::methane: return snow_t{0.8f, 0.f, 0.1217f};
      default:                         return snow_t{0.f, 0.f, 0.f};
      }
    }

  ///\brief Status.json Flags2: on foot (bit 0), on a planet (bit 4), outdoors (bit 15: 33041 on foot outside at 6 c,
  /// 2026-11-17)
  inline constexpr std::uint64_t flags2_on_foot{1ull << 0}, flags2_on_foot_on_planet{1ull << 4}, flags2_on_foot_exterior{1ull << 15};

  [[nodiscard]]
  constexpr auto snow_outdoors(std::uint64_t flags2) noexcept -> bool
    {
    std::uint64_t const all{flags2_on_foot | flags2_on_foot_on_planet | flags2_on_foot_exterior};
    return (flags2 & all) == all;
    }

  ///\brief how fast a crystal falls, metres a second: a small ice crystal about 0.3 m/s in the Earth's air (1.2 kg/m3 at
  /// 1 g); for a drag that grows with the square of the speed the speed goes with the square root of the gravity (in g)
  /// over the air's density, so in a thin air far faster (an ammonia world's ~1/600 of the Earth's: about 25 times,
  /// 2.6 m/s at 0.13 g; methane's 1/10: 0.23 m/s at 0.06 g) and the wind only leans the fall
  [[nodiscard]]
  inline auto snow_fall_speed(double gravity, double air_density) noexcept -> float
    {
    double const v{0.3 * std::sqrt(std::clamp(gravity, 0.01, 3.0) * 1.2 / std::max(air_density, 1e-4))};
    return static_cast<float>(std::clamp(v, 0.05, 8.0));
    }

  ///\brief where the eye walked on the ground since a reference fix: metres east and north from Status.json's latitude
  /// and longitude, wrapped at snow_wrap_m so a float keeps centimetres; a new reference past 10 km
  inline constexpr double snow_wrap_m{1024.0};

  struct snow_walk_t
    {
    double latitude{}, longitude{};
    bool have{};
    };

  [[nodiscard]]
  inline auto snow_walked(snow_walk_t & from, double latitude, double longitude, double radius) noexcept -> std::array<float, 2>
    {
    constexpr double to_rad{3.14159265358979323846 / 180.0};
    if(not from.have)
      from = snow_walk_t{latitude, longitude, true};
    double const east{(longitude - from.longitude) * to_rad * radius * std::cos(from.latitude * to_rad)};
    double const north{(latitude - from.latitude) * to_rad * radius};
    if(std::fabs(east) > 10000.0 or std::fabs(north) > 10000.0)
      {
      from = snow_walk_t{latitude, longitude, true};
      return {0.f, 0.f};
      }
    return {static_cast<float>(std::fmod(east, snow_wrap_m)), static_cast<float>(std::fmod(north, snow_wrap_m))};
    }
  }  // namespace edworld
