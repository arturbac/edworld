// edworld: the clouds' constant buffer (clouds3d.hlsl's `clouds` at b4, 128 floats) made of the settings, the planet
// as read and the frame. One place for the game's draw (hooks.cc, from what it read) and the render test
// (test/render_test.cc, from a scene of its own), so the test draws what the game gets.
#pragma once

#include "atmosphere_kind.h"
#include "aurora.h"
#include "runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace edworld
  {
  inline constexpr std::size_t clouds_value_count{128u};

  ///\brief the planet the clouds are for, as read off the game (hooks.cc) or set by a test
  struct clouds_planet_t
    {
    atmosphere_kind_e kind{atmosphere_kind_e::unknown};  ///< unknown: none of the kinds' clouds, no storms, no aurora
    float radius{};       ///< metres
    float kind_weight{};  ///< how far the kind's clouds have come in, 0..1
    float day_s{};        ///< its day, 0 = not read yet
    float day_fade{};     ///< how far what waits for the day has come in since it was read, 0..1
    float spin{-1.f};     ///< the way it turns about y, +1 or -1
    bool volcanic{};      ///< neon's second print
    float light_rg{}, light_bg{}, light_g{};  ///< the star's light at the last read (light_g 0 = none read)
    double utc_s{};       ///< the time the storm's level and the substorms follow
    };

  ///\brief what changes each draw beside the planet
  struct clouds_frame_t
    {
    float seconds{};          ///< the clouds' clock (the wind, the aurora's motion)
    double flicker_s{};       ///< the same clock in double, for the aurora's flicker
    float pixel{1.f};         ///< 2 when the volume is drawn at half size
    float port_width{};       ///< the picture's width, pixels
    float noise_shift{};      ///< the steps' start moved by this, 0..1 (clouds_noise_shift)
    std::array<float, 12> snow{};  ///< the snow's values 112..123 (hooks.cc keeps the eye's way)
    };

  ///\brief what the values were made of, for the log lines of a change
  struct clouds_report_t
    {
    cyclone_set_t storms{};
    float rossby{};
    bool aurora_on{};
    aurora_planet_t aurora{};
    float level{};
    aurora_height_t height{};
    };

  ///\brief the shift of the steps' start (0..1) this frame: the frame times the golden ratio's fraction, so each frame's
  /// grain differs from the last ones; by edupscale's jitter frame (-1: it does not jitter) or, with mode 2 and no
  /// jitter, edworld's own count of the clouds' draws
  [[nodiscard]]
  constexpr auto clouds_noise_shift(std::uint32_t mode, int jitter_frame, std::uint64_t own_frame) noexcept -> float
    {
    if(mode == 0u or (mode != 2u and jitter_frame < 0))
      return 0.f;  // a mode over 2 as 1
    std::uint64_t const n{jitter_frame >= 0 ? static_cast<std::uint64_t>(jitter_frame) : own_frame};
    double const x{static_cast<double>(n % 1000003u) * 0.6180339887498949};
    return static_cast<float>(x - static_cast<double>(static_cast<std::uint64_t>(x)));
    }

  static_assert(clouds_noise_shift(0u, 5, 5u) == 0.f and clouds_noise_shift(1u, -1, 5u) == 0.f and clouds_noise_shift(2u, -1, 0u) == 0.f
                and clouds_noise_shift(1u, 1, 0u) > 0.618f and clouds_noise_shift(1u, 1, 0u) < 0.619f
                and clouds_noise_shift(2u, -1, 1u) > 0.618f and clouds_noise_shift(2u, 2, 7u) == clouds_noise_shift(1u, 2, 0u));

  ///\brief values 0..127 of the clouds' buffer
  inline auto clouds_values(settings_t const & s, clouds_planet_t const & p, clouds_frame_t const & f,
                            std::array<float, clouds_value_count> & v) -> clouds_report_t
    {
    v.fill(0.f);
    clouds_report_t report{};
    v[0] = s.clouds_bottom;
    v[1] = s.clouds_top;
    v[2] = s.clouds_cover;
    v[3] = s.clouds_light;
    v[4] = s.clouds_steps;
    v[5] = s.clouds_density;
    v[6] = s.clouds_opacity;
    v[7] = s.clouds_top_gain;
    v[8] = s.clouds_min_light;
    v[9] = s.clouds_under_gain;
    v[10] = s.clouds_detail;
    v[11] = s.clouds_detail_size;
    v[12] = s.clouds_detail_range;
    v[13] = s.clouds_detail_edge;
    v[14] = s.clouds_detail_gap;
    v[15] = f.seconds;
    v[16] = s.clouds_wind_low / 3.6f;
    v[17] = s.clouds_wind_high / 3.6f;
    v[18] = f.pixel;
    v[19] = static_cast<float>(s.clouds_game_fade);
    v[20] = static_cast<float>(s.clouds_mode);
    v[21] = std::max(1.f, s.clouds_far_steps);
    v[22] = s.clouds_near_from;
    v[23] = std::max(s.clouds_near_to, s.clouds_near_from + 0.001f);
    v[24] = static_cast<float>(s.clouds_debug);
    v[25] = f.port_width;
    v[26] = s.clouds_hide_below_px;
    v[27] = std::max(s.clouds_full_from_px, s.clouds_hide_below_px + 1.f);
    v[28] = std::max(s.clouds_max_steps, 1.f);
    v[29] = s.clouds_edge_hide_px;
    v[30] = std::max(s.clouds_edge_full_px, s.clouds_edge_hide_px + 0.01f);
    v[31] = std::max(s.clouds_edge_band_px, 0.01f);
    v[32] = std::max(s.clouds_haze, 0.f);
    v[43] = std::clamp(s.clouds_size_var, 0.f, 0.9f);
    // the flicker's phase, wound in double: the shader's float seconds lose the 8 Hz after hours (C23)
    v[91] = static_cast<float>(std::fmod(f.flicker_s * 50.265, 6.283185307179586));
    std::copy(f.snow.begin(), f.snow.end(), v.begin() + 112);
    bool const by_kind{s.clouds_by_kind and p.kind != atmosphere_kind_e::unknown};
    if(not by_kind)
      return report;

    // 33..42: the planet whose kind is known and its clouds
    atmosphere_clouds_t const c{atmosphere_clouds_tinted(atmosphere_clouds(p.kind, s.clouds_physics_intensity), s.clouds_tint)};
    v[33] = p.radius;
    v[34] = p.kind_weight;
    v[35] = c.bottom;
    v[36] = c.top;
    v[37] = c.cover;
    v[38] = c.opacity * s.clouds_opacity;
    v[39] = c.detail * s.clouds_detail;
    v[40] = c.tint[0];
    v[41] = c.tint[1];
    v[42] = c.tint[2];

    // 44..85: the storms, each its centre and size, then their twists, then their count; none until the day is read
    // (they would stand on the kind's median and jump), then they grow in
    report.rossby = rossby_ratio(p.kind, p.radius, p.day_s);
    if(s.clouds_cyclones and p.day_s > 0.f)
      {
      report.storms = cyclones_of(report.rossby, static_cast<std::uint64_t>(std::llround(p.radius)), p.spin);
      for(std::uint32_t i{}; i != report.storms.count; ++i)
        {
        v[44u + i * 4u] = report.storms.at[i].x;
        v[45u + i * 4u] = report.storms.at[i].y;
        v[46u + i * 4u] = report.storms.at[i].z;
        v[47u + i * 4u] = report.storms.at[i].size * std::max(p.day_fade, 0.01f);  // the shader divides by the size
        v[76u + i] = report.storms.at[i].twist;
        }
      v[84] = static_cast<float>(report.storms.count);
      }

    // 86..90: the lumps by the layer, then how far the weather moves a cloud's edge and the R/L it follows
    if(s.clouds_lumps_by_layer)
      {
      atmosphere_lumps_t const l{atmosphere_lumps(p.kind, p.radius)};
      v[86] = l.scale;
      v[87] = l.detail_m;
      v[88] = l.lean;
      }
    v[89] = atmosphere_weather_shift * std::clamp(s.clouds_weather, 0.f, 2.f);
    v[90] = report.rossby;

    // 92..111: the aurora, over the clouds' top as the storms raise it (twice the layer on a planet with storms, C17)
    float const tower{v[84] > 0.f ? 2.f : 1.f};
    if(s.aurora)
      {
      std::uint64_t const seed{static_cast<std::uint64_t>(std::llround(p.radius))};
      report.aurora = aurora_planet(p.radius, p.day_s, report.rossby, p.volcanic, seed);
      if(s.aurora_field >= 0.f)
        report.aurora.field = s.aurora_field >= 0.5f;
      float const raw_level{s.aurora_activity >= 0.f ? std::min(s.aurora_activity, 9.f) : aurora_storm_level(seed, p.utc_s)};
      // a strong star's wind adds to the storm here, once, for the substorms and the shader's oval alike (C14); no
      // light read yet: no wind added
      float const wind{p.light_g > 0.f ? aurora_star_wind(p.light_rg, p.light_bg, p.light_g) : 0.6f};
      report.level = aurora_level_with_wind(raw_level, wind);
      aurora_substorm_t const sub{aurora_substorm(report.level, report.aurora.substorm_s, p.utc_s, seed)};
      aurora_colours_t const colours{aurora_colours(p.kind)};
      // no aurora until the planet's day is read (its field and substorms come from the day), then it comes in; a
      // field set by hand (aurora_field) needs no day. Without a field of its own the glow is the same in a storm as
      // in a quiet sky (C15)
      float const fade{s.aurora_field >= 0.f ? 1.f : p.day_fade};
      report.height = aurora_height(p.kind, p.radius, tower);
      report.aurora_on = true;
      v[92] = colours.yield * std::clamp(s.aurora_gain, 0.f, 3.f) * (report.aurora.field ? sub.bright : 1.f) * fade;
      v[93] = report.level;
      v[94] = sub.dance * std::clamp(s.aurora_dance, 0.f, 2.f);
      v[95] = sub.shift;
      std::copy(colours.low, colours.low + 3, v.begin() + 96);
      v[99] = s.aurora_flicker ? 1.f : 0.f;
      std::copy(colours.high, colours.high + 3, v.begin() + 100);
      v[103] = report.aurora.field ? 1.f : 0.f;
      std::copy(report.aurora.axis, report.aurora.axis + 3, v.begin() + 104);
      v[107] = report.height.floor;
      v[108] = report.height.ceiling;
      std::copy(colours.edge, colours.edge + 3, v.begin() + 109);
      }

    // 124..127: the weather's R/L from the kind's median (before the day was read) and how far the day's R/L has come
    // in, over the same 4 s as the storms (C19)
    v[124] = rossby_ratio(p.kind, p.radius, 0.f);
    v[125] = p.day_s > 0.f ? p.day_fade : 1.f;
    v[126] = f.noise_shift;  // the steps' start moved (clouds_noise_time)

    // the storms tower to twice the layer's height (v1.4s): the kind's top raised, the tower told to the shader
    if(v[84] > 0.f)
      {
      v[36] = v[35] + (v[36] - v[35]) * 2.f;
      v[85] = 2.f;
      }
    return report;
    }
  }  // namespace edworld
