// edworld (PoC, 2026-11-17): crystals of the air's own gas falling near the ground, under the clouds - only
// where the kind read off the render says its gas condenses at the planet's cold (research of 2026-11-16: an ammonia
// atmosphere lies on its saturation curve on most planets, a methane one near it; both below their gas' triple point,
// so crystals, no rain). No journal: the kind from the colour, the cold ammonia from its second colour (a cold,
// oversaturated planet, Spoihee IT-F d12-727 6 c and 7 e); on foot outdoors or in a craft in the planet's air, the
// eye's way and the gravity from Status.json.
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

  ///\brief the snow's parts drawn (clouds3d.hlsl snow_parts): the crystals 1, the haze 2
  inline constexpr float snow_parts_all{3.f};

  ///\brief snow_bench: the parts of the 10 s step since a start (all, none, the haze, the crystals) and its name
  struct snow_bench_step_t
    {
    float parts;
    char const * name;
    };

  [[nodiscard]]
  constexpr auto snow_bench_step(std::uint64_t seconds) noexcept -> snow_bench_step_t
    {
    constexpr snow_bench_step_t steps[4]{{3.f, "all"}, {0.f, "none"}, {2.f, "the haze alone"}, {1.f, "the crystals alone"}};
    return steps[(seconds / 10u) % 4u];
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

  ///\brief Status.json Flags: docked (bit 0), supercruise (4), in the main ship (24), a fighter (25), an SRV (26)
  inline constexpr std::uint64_t flags_docked{1ull << 0}, flags_supercruise{1ull << 4},
    flags_in_craft{(1ull << 24) | (1ull << 25) | (1ull << 26)};

  ///\brief where it may snow: on foot outdoors (Status.json with the place and no altitude), or in a ship, a fighter or
  /// an SRV with an altitude out of supercruise and not docked (a planet's port has its pads in hangars too); how high
  /// is the shader's (5 km over the planet's radius)
  [[nodiscard]]
  constexpr auto snow_where(std::uint64_t flags, std::uint64_t flags2, bool has_ground, bool has_position) noexcept -> bool
    {
    if(has_ground)
      return snow_outdoors(flags2);
    return has_position and (flags & flags_in_craft) != 0 and (flags & (flags_docked | flags_supercruise)) == 0;
    }

  ///\brief the gravity in g for the fall: Status.json has it on foot only, so the last one read on foot on a planet of
  /// the same radius stands in a craft; 0.1 g without one
  struct snow_gravity_t
    {
    double radius{}, gravity{};
    };

  [[nodiscard]]
  inline auto snow_gravity(snow_gravity_t & last, bool has_ground, double gravity, double radius) noexcept -> double
    {
    if(has_ground and gravity > 0.0)
      last = snow_gravity_t{radius, gravity};
    return last.gravity > 0.0 and std::fabs(last.radius - radius) < 1.0 ? last.gravity : 0.1;
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

  ///\brief where the eye went since a reference fix: metres east, north and up from Status.json's latitude, longitude
  /// and altitude (none on foot); a new reference past 10 km or as the eye leaves or enters a craft, the way so far
  /// carried on (`carry`), so the cells do not jump there (C6). Between the fixes the eye goes on at the speed of the last
  /// two (a fast flight would jump by tens of metres at each new fix) for half as long again as the fixes came, 2 s at
  /// the most, and then stays where that took it (C11). A fix is timed when the game wrote it (`fix_at`, the state
  /// thread's read less the file's age), not when the render thread saw it (C8); what a new fix still differs from the
  /// way extrapolated to it is taken in over snow_settle_s instead of in one frame
  inline constexpr double snow_wrap_m{1024.0};
  inline constexpr double snow_coast_s{2.0};
  inline constexpr double snow_settle_s{0.5};

  struct snow_walk_t
    {
    ///\brief the reference fix
    double latitude{}, longitude{}, altitude{};
    bool have{};
    bool on_foot{};
    bool from_mean{};  ///< Status.json Flags bit 29 at the reference
    ///\brief the last fix (metres from the reference), when the game wrote it and how long after the one before
    /// (seconds by the caller's clock), and the speed between the two
    double east{}, north{}, up{}, at{}, gap{};
    double v_east{}, v_north{}, v_up{};
    ///\brief the way before the current reference, and where the eye was last shown (both unwrapped)
    double carry_east{}, carry_north{}, carry_up{};
    double shown_east{}, shown_north{}, shown_up{};
    ///\brief the difference a new fix made against the way extrapolated to it, taken in from `settle_from` on
    double settle_east{}, settle_north{}, settle_up{}, settle_from{};
    };

  ///\brief metres from the first reference, unwrapped, then the speed in metres a second (0 once the fixes stop)
  struct snow_eye_t
    {
    std::array<double, 3> place;
    std::array<float, 3> speed;
    };

  [[nodiscard]]
  inline auto snow_walked(snow_walk_t & from, double latitude, double longitude, double altitude, bool on_foot, double radius,
                          double now, double fix_at, bool from_mean = false) noexcept -> snow_eye_t
    {
    constexpr double to_rad{3.14159265358979323846 / 180.0};
    double const height{on_foot ? 0.0 : altitude};
    bool const switched{from.have and from.from_mean != from_mean};
    // a new reference here: the way shown so far carried on, the speed kept
    auto const reference{[&](bool first)
                           {
                           snow_walk_t const kept{from};
                           from = snow_walk_t{.latitude = latitude, .longitude = longitude, .altitude = height, .have = true,
                                              .on_foot = on_foot, .at = fix_at, .gap = kept.gap,
                                              .v_east = kept.v_east, .v_north = kept.v_north, .v_up = kept.v_up};
                           if(not first)
                             {
                             from.carry_east = kept.shown_east;
                             from.carry_north = kept.shown_north;
                             from.carry_up = kept.shown_up;
                             from.shown_east = kept.shown_east;
                             from.shown_north = kept.shown_north;
                             from.shown_up = kept.shown_up;
                             }
                           }};
    if(not from.have)
      reference(true);
    else if(from.on_foot != on_foot or switched)
      {
      reference(false);
      if(switched)  // the altitude's reference moved, the eye did not: no speed out of it
        from.v_east = from.v_north = from.v_up = 0.0;
      }
    from.from_mean = from_mean;
    double east{(longitude - from.longitude) * to_rad * radius * std::cos(from.latitude * to_rad)};
    double north{(latitude - from.latitude) * to_rad * radius};
    double up{height - from.altitude};
    if(std::fabs(east) > 10000.0 or std::fabs(north) > 10000.0 or std::fabs(up) > 10000.0)
      {
      reference(false);
      east = north = up = 0.0;
      }
    auto const coasting{[&](double t) { return std::min(std::max(t - from.at, 0.0), std::min(1.5 * from.gap, snow_coast_s)); }};
    if(east != from.east or north != from.north or up != from.up)
      {
      // where the way extrapolated from the last fix stood at this fix's time, against the fix itself
      double const ahead{coasting(fix_at)};
      double const was_east{from.east + from.v_east * ahead}, was_north{from.north + from.v_north * ahead},
        was_up{from.up + from.v_up * ahead};
      bool const was_going{from.v_east != 0.0 or from.v_north != 0.0 or from.v_up != 0.0};
      double const seconds{fix_at - from.at};
      if(seconds >= snow_coast_s)
        from.v_east = from.v_north = from.v_up = 0.0;
      else if(seconds > 0.05)
        {
        from.v_east = (east - from.east) / seconds;
        from.v_north = (north - from.north) / seconds;
        from.v_up = (up - from.up) / seconds;
        from.gap = seconds;
        }
      // the difference taken in over snow_settle_s, only for a way that went on (a start is the eye's own move); a
      // large one (a teleport, a new place) at once
      double const de{was_east - east}, dn{was_north - north}, du{was_up - up};
      if(was_going and de * de + dn * dn + du * du < 50.0 * 50.0)
        {
        from.settle_east = de;
        from.settle_north = dn;
        from.settle_up = du;
        from.settle_from = now;
        }
      else
        from.settle_east = from.settle_north = from.settle_up = 0.0;
      from.east = east;
      from.north = north;
      from.up = up;
      from.at = fix_at;
      }
    double const since{now - from.at};
    bool const going{since < std::min(1.5 * from.gap, snow_coast_s)};
    double const coast{coasting(now)};
    double const settle{std::max(0.0, 1.0 - (now - from.settle_from) / snow_settle_s)};
    snow_eye_t eye{};
    eye.place = {from.carry_east + from.east + from.v_east * coast + from.settle_east * settle,
                 from.carry_north + from.north + from.v_north * coast + from.settle_north * settle,
                 from.carry_up + from.up + from.v_up * coast + from.settle_up * settle};
    from.shown_east = eye.place[0];
    from.shown_north = eye.place[1];
    from.shown_up = eye.place[2];
    if(going)
      eye.speed = {static_cast<float>(from.v_east), static_cast<float>(from.v_north), static_cast<float>(from.v_up)};
    return eye;
    }

  ///\brief the cells' place for the shader: the eye's way, and the snow's own fall and the wind's drift summed here in
  /// double over the time (they grew with the session in the shader's float, C5), wrapped to snow_wrap_m (the shader
  /// numbers its cells round it, so the wrap changes no crystal)
  struct snow_drift_t
    {
    double fall{}, east{}, north{};
    double at{-1.0};
    };

  [[nodiscard]]
  inline auto snow_wrapped(double m) noexcept -> float
    {
    double const w{std::fmod(m, snow_wrap_m)};
    return static_cast<float>(w < 0.0 ? w + snow_wrap_m : w);
    }

  ///\brief a smooth value 0..1 along t (value noise, one value at each whole t, eased between)
  [[nodiscard]]
  inline auto snow_noise(double t, std::uint64_t seed) noexcept -> double
    {
    auto const at{[seed](std::int64_t i) noexcept
                    {
                    std::uint64_t z{static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ull + seed};
                    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
                    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
                    return static_cast<double>((z ^ (z >> 31)) >> 11) / 9007199254740992.0;
                    }};
    double const f{std::floor(t)};
    double const x{t - f};
    double const e{x * x * (3.0 - 2.0 * x)};
    std::int64_t const i{static_cast<std::int64_t>(f)};
    return at(i) + (at(i + 1) - at(i)) * e;
    }

  ///\brief the wind's drift near the ground, m/s east and north: a slow wind, 0.3..1.5 m/s, turning over minutes
  [[nodiscard]]
  inline auto snow_drift_speed(double seconds) noexcept -> std::array<double, 2>
    {
    double const angle{6.283185307179586 * snow_noise(seconds * 0.01, 31u)};
    double const speed{0.3 + 1.2 * snow_noise(seconds * 0.03, 53u)};
    return {std::cos(angle) * speed, std::sin(angle) * speed};
    }

  ///\brief one frame: the fall (up is +, so the cells rise as the crystals fall through them) and the drift summed
  inline auto snow_drift_step(snow_drift_t & d, double fall_speed, double now) noexcept -> std::array<double, 2>
    {
    std::array<double, 2> const v{snow_drift_speed(now)};
    double const dt{d.at < 0.0 ? 0.0 : std::clamp(now - d.at, 0.0, 0.25)};
    d.at = now;
    d.fall += fall_speed * dt;
    d.east += v[0] * dt;
    d.north += v[1] * dt;
    return v;
    }
  }  // namespace edworld
