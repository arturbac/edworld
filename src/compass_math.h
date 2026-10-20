// edworld — the HUD compass read off its interface surface: pure arithmetic, no D3D, testable anywhere.
//
// The compass by the radar is the sphere of directions around the ship seen from the front: the target's direction
// d = (right, up, forward) in the ship's frame lands on the disc at R * (right, up) (an orthographic projection), the
// dot filled while the target is ahead (forward > 0), hollow while it is behind. So the angle off the nose is
// asin(r / R), not proportional to r: near the rim the dot hardly moves. Checked on 2026-10-06 (ed-lab, a Kestrel,
// the planet as the target): asin(r / R) against 90 degrees plus the flight path angle from Status.json agreed
// within 1-2 degrees while the ship flew straight.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace edworld
  {
  ///\brief the dot as found in an RGBA copy of the compass: its centre relative to the disc's centre, in surface
  /// pixels (x right, y down as on the surface)
  struct compass_reading_t
    {
    bool found;
    ///\brief the dot's middle is lit (target ahead); a hollow ring (target behind) leaves it dark
    bool filled;
    float x;
    float y;
    ///\brief the dot's pixels (since the game's update of 2026-10-20 a filled dot ~176-206, a hollow ring ~111 on the
    /// 2200x1800 surface; before it, in an EDHM theme, ~137 and ~25)
    std::uint32_t pixels;
    ///\brief the disc's colour this frame took the dot's: a flash of the compass, nothing to read
    bool flash;
    ///\brief the disc's hue (degrees, the most common one of its coloured pixels), -1 without one
    float disc_hue;
    ///\brief the dot's hue (degrees), -1 when it is white or grey (an EDHM theme's dot)
    float dot_hue;
    };

  ///\brief a pixel's hue (degrees, 0..360), saturation and value (0..1)
  struct hsv_t
    {
    float h, s, v;
    };

  [[nodiscard]]
  constexpr auto hsv_of(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept -> hsv_t
    {
    float const rf{r / 255.f}, gf{g / 255.f}, bf{b / 255.f};
    float const mx{std::max({rf, gf, bf})}, mn{std::min({rf, gf, bf})}, c{mx - mn};
    float h{};
    if(c > 0.f)
      {
      if(mx == rf)
        h = 60.f * ((gf - bf) / c);
      else if(mx == gf)
        h = 60.f * ((bf - rf) / c + 2.f);
      else
        h = 60.f * ((rf - gf) / c + 4.f);
      if(h < 0.f)
        h += 360.f;
      }
    return hsv_t{h, mx > 0.f ? c / mx : 0.f, mx};
    }

  ///\brief degrees between two hues, 0..180
  [[nodiscard]]
  inline auto hue_distance(float a, float b) noexcept -> float
    {
    float const d{std::fabs(a - b)};
    return std::min(d, 360.f - d);
    }

  ///\brief the dot in an RGBA copy (pixel (x, y) at data + y * pitch + x * 4) whose disc's centre is (cx, cy) in the
  /// copy, within limit of the centre, told by colour against the disc, not by a colour of its own: the disc's hue is
  /// the most common hue of its coloured pixels this frame (orange in the game's own HUD, any colour in an EDHM theme),
  /// the dot is what is bright and either of another hue (more than 60 degrees off) or white. So it holds in every
  /// theme. last_dot_hue: the dot's hue in the last reading shown (-1 none): a disc of that hue is a flash, not read.
  [[nodiscard]]
  inline auto read_compass(std::uint8_t const * data, std::uint32_t pitch, std::int32_t width, std::int32_t height, float cx,
                           float cy, float limit, float last_dot_hue = -1.f) noexcept -> compass_reading_t
    {
    compass_reading_t out{};
    out.disc_hue = out.dot_hue = -1.f;
    float const limit2{limit * limit};
    auto const inside = [&](std::int32_t x, std::int32_t y) noexcept
      {
      float const dx{static_cast<float>(x) - cx}, dy{static_cast<float>(y) - cy};
      return dx * dx + dy * dy <= limit2;
      };
    auto const at = [&](std::int32_t x, std::int32_t y) noexcept
      {
      std::uint8_t const * p{data + static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * 4u};
      return hsv_of(p[0], p[1], p[2]);
      };
    // the disc's hue: a histogram of 36 bins over its coloured pixels
    std::uint32_t bins[36]{};
    for(std::int32_t y{}; y != height; ++y)
      for(std::int32_t x{}; x != width; ++x)
        if(inside(x, y))
          {
          hsv_t const c{at(x, y)};
          if(c.s > 0.2f and c.v > 0.1f)
            ++bins[std::min(35, static_cast<std::int32_t>(c.h / 10.f))];
          }
    std::int32_t top{-1};
    for(std::int32_t i{}; i != 36; ++i)
      if(bins[i] != 0u and (top < 0 or bins[i] > bins[top]))
        top = i;
    // a disc without colour (none drawn, a grey theme): then only what is white or coloured and bright is the dot
    out.disc_hue = top < 0 ? -1.f : static_cast<float>(top) * 10.f + 5.f;
    if(out.disc_hue >= 0.f and last_dot_hue >= 0.f and hue_distance(out.disc_hue, last_dot_hue) < 60.f)
      {
      out.flash = true;
      return out;
      }
    // the dot's pixel: bright, and white or of another hue than the disc
    auto const dot_pixel = [&](hsv_t const & c) noexcept
      {
      if(c.v <= 0.35f)
        return false;
      if(c.s <= 0.2f)
        return c.v > 0.67f;
      return out.disc_hue < 0.f or hue_distance(c.h, out.disc_hue) > 60.f;
      };
    double sx{}, sy{}, hx{}, hy{};
    std::uint32_t n{}, coloured{};
    for(std::int32_t y{}; y != height; ++y)
      for(std::int32_t x{}; x != width; ++x)
        {
        if(not inside(x, y))
          continue;
        hsv_t const c{at(x, y)};
        if(not dot_pixel(c))
          continue;
        bool const white{c.s <= 0.2f};
        sx += static_cast<float>(x) - cx;
        sy += static_cast<float>(y) - cy;
        ++n;
        if(not white)
          {
          ++coloured;
          hx += std::cos(c.h / 57.29577951308232f);
          hy += std::sin(c.h / 57.29577951308232f);
          }
        }
    if(n == 0)
      return out;
    out.found = true;
    out.pixels = n;
    out.x = static_cast<float>(sx / n);
    out.y = static_cast<float>(sy / n);
    if(coloured * 2u > n)
      {
      float h{static_cast<float>(std::atan2(hy, hx) * 57.29577951308232)};
      out.dot_hue = h < 0.f ? h + 360.f : h;
      }
    // the middle of the dot: the dot's own colour when filled, the disc's in the ring's hole when hollow
    std::int32_t const mx{static_cast<std::int32_t>(std::lround(cx + out.x))}, my{static_cast<std::int32_t>(std::lround(cy + out.y))};
    if(mx >= 0 and my >= 0 and mx < width and my < height)
      out.filled = dot_pixel(at(mx, my));
    return out;
    }

  ///\brief how many pixels a dot worth showing has, filled and hollow
  struct compass_limits_t
    {
    std::uint32_t filled_min, filled_max, hollow_min, hollow_max;
    };

  ///\brief the game's dot since its update of 2026-10-20 (2200x1800, a Kestrel): filled 176-206, hollow 111, with room
  inline constexpr compass_limits_t compass_default_limits{120u, 320u, 60u, 180u};

  ///\brief a reading worth showing: a handful of pixels (the dot half drawn, a flash entering an atmosphere) or many
  /// (the whole disc lit) is not the dot
  [[nodiscard]]
  constexpr auto compass_plausible(compass_reading_t const & r, compass_limits_t const & l = compass_default_limits) noexcept -> bool
    {
    return r.found and not r.flash
           and (r.filled ? r.pixels >= l.filled_min and r.pixels <= l.filled_max : r.pixels >= l.hollow_min and r.pixels <= l.hollow_max);
    }

  ///\brief the reading shown still holds: one came less than stale_s seconds ago (0: for ever, as before 2026-10-20)
  [[nodiscard]]
  constexpr auto compass_fresh(bool have, double age_s, float stale_s) noexcept -> bool
    {
    return have and (stale_s <= 0.f or age_s <= static_cast<double>(stale_s));
    }

  inline constexpr float compass_degrees{57.29577951308232f};

  ///\brief the target's direction from the dot, in degrees
  struct compass_angles_t
    {
    ///\brief between the nose and the target, 0..180
    float off_nose;
    ///\brief above (+) or below (-) the ship's horizontal plane (its wings' plane): asin(up)
    float up;
    ///\brief right (+) or left (-) of the ship's vertical plane, measured in the plane of nose and wings:
    /// atan2(right, forward), -180..180
    float right;
    };

  ///\brief the angles from the dot's offset (x right, y down, surface pixels) on a disc of radius radius; an offset
  /// past the rim counts as on it
  [[nodiscard]]
  inline auto compass_angles(float x, float y, float radius, bool filled) noexcept -> compass_angles_t
    {
    float rx{x / radius}, ry{-y / radius};
    float const r{std::sqrt(rx * rx + ry * ry)};
    if(r > 1.f)
      {
      rx /= r;
      ry /= r;
      }
    float const forward{std::sqrt(std::max(0.f, 1.f - rx * rx - ry * ry)) * (filled ? 1.f : -1.f)};
    return compass_angles_t{
      std::acos(std::clamp(forward, -1.f, 1.f)) * compass_degrees,
      std::asin(std::clamp(ry, -1.f, 1.f)) * compass_degrees,
      std::atan2(rx, forward) * compass_degrees
    };
    }

  ///\brief the flight path angle (degrees, + climbing) between two fixes over a sphere of radius planet_radius:
  /// latitude and longitude in degrees, altitudes in metres; none (NaN) when the fixes are the same place
  [[nodiscard]]
  inline auto flight_path_angle(double lat1, double lon1, double alt1, double lat2, double lon2, double alt2,
                                double planet_radius) noexcept -> double
    {
    constexpr double rad{3.14159265358979323846 / 180.0};
    double const a1{lat1 * rad}, a2{lat2 * rad}, dl{(lon2 - lon1) * rad};
    double const c{std::acos(std::clamp(std::sin(a1) * std::sin(a2) + std::cos(a1) * std::cos(a2) * std::cos(dl), -1.0, 1.0))};
    double const horizontal{c * (planet_radius + (alt1 + alt2) / 2.0)};
    double const vertical{alt2 - alt1};
    if(horizontal == 0.0 and vertical == 0.0)
      return std::nan("");
    return std::atan2(vertical, horizontal) / rad;
    }

  ///\brief the straight line from the ship to a place on the surface
  struct glide_t
    {
    double distance;  ///< metres over the ground, along the surface
    double below;     ///< degrees below the local horizon (negative: the place stands over it)
    };

  ///\brief the line to a place at the surface (latitude, longitude) from the ship's fix over a sphere of radius
  /// planet_radius: the place taken at the radius itself, its own height unknown
  [[nodiscard]]
  inline auto glide_to(double lat, double lon, double alt, double place_lat, double place_lon, double planet_radius) noexcept
    -> glide_t
    {
    constexpr double rad{3.14159265358979323846 / 180.0};
    double const a1{lat * rad}, a2{place_lat * rad}, dl{(place_lon - lon) * rad};
    // haversine: the cosine rule loses metres near the place, where they count
    double const sl{std::sin((a2 - a1) / 2.0)}, so{std::sin(dl / 2.0)};
    double const c{2.0 * std::asin(std::min(1.0, std::sqrt(sl * sl + std::cos(a1) * std::cos(a2) * so * so)))};
    double const horizontal{planet_radius * std::sin(c)};
    double const vertical{planet_radius + alt - planet_radius * std::cos(c)};
    return glide_t{c * planet_radius, horizontal == 0.0 and vertical == 0.0 ? 0.0 : std::atan2(vertical, horizontal) / rad};
    }

  // ---- the spheres drawn beside the dashboard (PoC): the target's direction in the ship's frame, and a view of it ----
  ///\brief a direction in the ship's frame: x right, y up, z forward (unit length)
  struct compass_direction_t
    {
    float x, y, z;
    };

  ///\brief the target's direction from the dot (x right, y down on the surface, radius in its pixels), as
  /// compass_angles reads it
  [[nodiscard]]
  inline auto compass_direction(float x, float y, float radius, bool filled) noexcept -> compass_direction_t
    {
    float rx{x / radius}, ry{-y / radius};
    float const r{std::sqrt(rx * rx + ry * ry)};
    if(r > 1.f)
      {
      rx /= r;
      ry /= r;
      }
    return compass_direction_t{rx, ry, std::sqrt(std::max(0.f, 1.f - rx * rx - ry * ry)) * (filled ? 1.f : -1.f)};
    }

  ///\brief a direction from its angles in degrees (up: above the wings' plane, right: atan2(right, forward))
  [[nodiscard]]
  inline auto direction_of(float up, float right) noexcept -> compass_direction_t
    {
    float const u{up / compass_degrees}, r{right / compass_degrees};
    return compass_direction_t{std::cos(u) * std::sin(r), std::sin(u), std::cos(u) * std::cos(r)};
    }

  // ---- the approach to a landing pad (PoC): the target below the ship, seen from above ----
  ///\brief the target below the ship as angles off the ship's vertical (its down): ahead (+) or behind (-) in the plane
  /// of nose and vertical, right (+) or left (-) in the plane of wings and vertical, and the whole angle off it
  struct pad_offsets_t
    {
    float forward;
    float right;
    float off_vertical;
    };

  [[nodiscard]]
  inline auto pad_offsets(compass_direction_t const & d) noexcept -> pad_offsets_t
    {
    float const down{-d.y};
    return pad_offsets_t{std::atan2(d.z, down) * compass_degrees, std::atan2(d.x, down) * compass_degrees,
                         std::acos(std::clamp(down, -1.f, 1.f)) * compass_degrees};
    }

  ///\brief whether the approach view shows: on once the target is below degrees or more under the wings' plane in
  /// normal flight and close (near_target; not "near", a macro of the Windows headers), off again once it rises above below - 10, the ship leaves normal flight,
  /// the target is no longer near or the reading went stale (have: a fresh one, compass_fresh)
  [[nodiscard]]
  constexpr auto approach_view(bool was_on, bool normal_flight, bool have, bool close, float up, float below) noexcept -> bool
    {
    if(not normal_flight or not have or not close)
      return false;
    return was_on ? up <= -(below - 10.f) : up <= -below;
    }

  ///\brief the target near enough for the approach view: a place on a body within km (from the ship's fix and the
  /// place's latitude and longitude, its own height unknown), or, without a place, a target in this system (a station,
  /// a carrier: Status.json's Destination.Body set) while the ship is mass locked (Flags bit 16)
  [[nodiscard]]
  inline auto near_target(bool has_position, bool has_place, double lat, double lon, double alt, double place_lat,
                          double place_lon, double planet_radius, std::uint32_t destination_body, std::uint64_t flags,
                          float km) noexcept -> bool
    {
    if(has_place)
      {
      if(not has_position or planet_radius <= 0.0)
        return false;
      glide_t const g{glide_to(lat, lon, alt, place_lat, place_lon, planet_radius)};
      return std::hypot(g.distance, alt) <= static_cast<double>(km) * 1000.0;
      }
    return destination_body != 0u and (flags & (1ull << 16)) != 0u;
    }

  ///\brief degrees between two directions
  [[nodiscard]]
  inline auto angle_between(compass_direction_t const & a, compass_direction_t const & b) noexcept -> float
    {
    return std::acos(std::clamp(a.x * b.x + a.y * b.y + a.z * b.z, -1.f, 1.f)) * compass_degrees;
    }

  ///\brief how far an exponential average with the time constant tau (seconds) moves towards a reading seconds after the
  /// previous one: 1 - exp(-seconds / tau); a reading at once after another hardly moves it, one a long time after replaces it
  [[nodiscard]]
  inline auto smoothing_weight(float seconds, float tau) noexcept -> float
    {
    if(tau <= 0.f)
      return 1.f;
    return 1.f - std::exp(-std::max(0.f, seconds) / tau);
    }

  ///\brief 0xRRGGBB a fraction t (0..1) of the way from a to b, each channel rounded
  [[nodiscard]]
  constexpr auto mix_rgb(std::uint32_t a, std::uint32_t b, float t) noexcept -> std::uint32_t
    {
    std::uint32_t out{};
    for(std::uint32_t shift{0u}; shift != 24u; shift += 8u)
      {
      float const ca{static_cast<float>((a >> shift) & 0xffu)}, cb{static_cast<float>((b >> shift) & 0xffu)};
      float const c{std::clamp(ca + (cb - ca) * t, 0.f, 255.f)};
      out |= static_cast<std::uint32_t>(c + 0.5f) << shift;
      }
    return out;
    }

  ///\brief a point of the unit sphere as a camera behind, to the left and above sees it: x right and y up on the
  /// picture (the sphere's outline is the unit circle), on_far_side: on the far side of the sphere from the camera
  struct sphere_point_t
    {
    float x, y;
    bool on_far_side;
    };

  ///\brief the camera turned yaw degrees about the ship's vertical (to its left) and tilted pitch degrees down
  /// over it; yaw 0, pitch 0 looks along the nose from behind
  [[nodiscard]]
  inline auto sphere_view(compass_direction_t const & d, float yaw, float pitch) noexcept -> sphere_point_t
    {
    float const cy{std::cos(yaw / compass_degrees)}, sy{std::sin(yaw / compass_degrees)};
    float const cp{std::cos(pitch / compass_degrees)}, sp{std::sin(pitch / compass_degrees)};
    float const x1{d.x * cy - d.z * sy}, z1{d.x * sy + d.z * cy};
    float const y2{d.y * cp + z1 * sp}, z2{-d.y * sp + z1 * cp};
    return sphere_point_t{x1, y2, z2 > 0.f};
    }
  }  // namespace edworld
